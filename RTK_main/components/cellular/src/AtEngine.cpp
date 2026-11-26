// AT 命令引擎实现（ADR-0021 §2.4）。仅 kNet 任务调用；不阻塞、不分配堆。
#include "cellular/AtEngine.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "cellular/AtParse.hpp"

namespace cellular {
namespace {

constexpr std::string_view kOk = "OK";
constexpr std::string_view kError = "ERROR";
constexpr std::string_view kCmeError = "+CME ERROR:";
constexpr std::string_view kCmsError = "+CMS ERROR:";
constexpr std::string_view kSendOk = "SEND OK";
constexpr std::string_view kSendFail = "SEND FAIL";
// 重新同步握手：带独有应答形状（前缀行再 OK），迟到的旧终结行无法冒充。
// 两条只读探针交替使用：上一轮超时后迟到的同形应答不会被下一轮误认为自己的应答。
struct Probe final {
    std::string_view cmd;
    std::string_view echo;
    std::string_view line;
};
constexpr std::array<Probe, 2> kProbes{{
    {"AT+CMEE?\r", "AT+CMEE?", "+CMEE:"},
    {"AT+CSCS?\r", "AT+CSCS?", "+CSCS:"},
}};

/**
 * @brief  tick 回绕安全的“已到期”判断
 * @param  nowMs:      当前毫秒
 * @param  deadlineMs: 截止毫秒
 * @retval true  nowMs 已到达或越过截止时间
 * @note   纯函数；利用无符号差转有符号，支持 uint32 回绕（单段等待 < 2^31 ms）。
 */
bool reached(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

} // namespace

/**
 * @brief 绑定串口写出与 URC 回调并复位引擎状态
 * @param port: 串口钩子集合（write/onUrc/urcPrefixes/ctx）
 * @note 仅 kNet 执行器任务或启动期单线程调用，非 ISR；不分配堆。
 */
void AtEngine::bind(const AtPort &port) noexcept
{
    port_ = port;
}

bool AtEngine::submit(const AtCommand &cmd, uint32_t nowMs) noexcept
{
    if (dead_ || (count_ >= kQueueDepth) || cmd.text.empty() || (cmd.text.size() >= kCmdBytes) ||
        (cmd.payload.size() > kMaxPayload)) {
        return false;
    }
    Slot &slot = queue_[(head_ + count_) % kQueueDepth];
    std::copy(cmd.text.begin(), cmd.text.end(), slot.text.begin());
    // 文本长度已限制 < kCmdBytes(128)，窄化到 uint8_t 无损。
    slot.len = static_cast<uint8_t>(cmd.text.size());
    slot.text[slot.len] = '\r';
    slot.timeoutMs = cmd.timeoutMs;
    slot.onLine = cmd.onLine;
    slot.onRaw = cmd.onRaw;
    slot.onDone = cmd.onDone;
    slot.ctx = cmd.ctx;
    slot.payload = cmd.payload;
    slot.rawPrefix = cmd.rawPrefix;
    slot.rawMax = cmd.rawMax;
    slot.match = cmd.match;
    slot.token = cmd.token;
    slot.prefixLen = static_cast<uint8_t>(std::min(cmd.matchPrefix.size(), kMatchPrefixBytes));
    std::copy_n(cmd.matchPrefix.begin(), slot.prefixLen, slot.prefix.begin());
    slot.quietFirst = cmd.quietFirst;
    ++count_;
    if ((count_ == 1U) && (state_ == State::kIdle)) {
        startFront(nowMs);
    }
    return true;
}

/**
 * @brief 启动队首命令：写出命令行并记录起始时间
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::startFront(uint32_t nowMs) noexcept
{
    if ((count_ == 0U) || (state_ != State::kIdle)) {
        return;
    }
    if (resync_) {
        enterQuiet(nowMs, true);
        return;
    }
    if (queue_[head_].quietFirst && !quietDone_) {
        enterQuiet(nowMs, false);
        return;
    }
    sendFront(nowMs);
}

/**
 * @brief 进入静默等待：丢弃所有到达的字节，直到通道连续静默 kQuietMs
 * @param nowMs: 当前毫秒时间戳
 * @param thenProbe: true 静默后发重新同步探针；false 静默后直接发队首命令
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::enterQuiet(uint32_t nowMs, bool thenProbe) noexcept
{
    state_ = State::kQuiet;
    quietThenProbe_ = thenProbe;
    lastRxMs_ = nowMs;
    deadlineMs_ = nowMs + kQuietMaxMs;
    lineLen_ = 0U;
    overflow_ = false;
    skipSpace_ = false;
}

/**
 * @brief 写出队首命令并起算超时（调用时状态为 kIdle 或刚结束静默）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::sendFront(uint32_t nowMs) noexcept
{
    const Slot &slot = queue_[head_];
    quietDone_ = false;
    deadlineMs_ = nowMs + slot.timeoutMs;
    state_ = slot.payload.empty() ? State::kWaitResponse : State::kWaitPrompt;
    lineLen_ = 0U;
    overflow_ = false;
    skipSpace_ = false;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): char 缓冲按字节只读视图，别名规则允许
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t *>(slot.text.data()),
                                         static_cast<size_t>(slot.len) + 1U};
    if ((port_.write == nullptr) || !port_.write(port_.ctx, bytes)) {
        ++stats_.writeFails;
        // 写失败视作未发出：按超时处理，让上层通过无响应计数走恢复路径。
        deadlineMs_ = nowMs;
    }
}

/**
 * @brief 发出重新同步握手 AT+CMEE?，期间丢弃旧响应
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::sendProbe(uint32_t nowMs) noexcept
{
    probeAlt_ = !probeAlt_;
    const Probe &probe = kProbes[probeAlt_ ? 1U : 0U];
    deadlineMs_ = nowMs + kProbeTimeoutMs;
    state_ = State::kProbe;
    probeSeen_ = false;
    lineLen_ = 0U;
    overflow_ = false;
    skipSpace_ = false;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 静态字符串按字节只读视图
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t *>(probe.cmd.data()),
                                         probe.cmd.size()};
    if ((port_.write == nullptr) || !port_.write(port_.ctx, bytes)) {
        ++stats_.writeFails;
        deadlineMs_ = nowMs;
    }
}

/**
 * @brief 处理重新同步握手期间收到的一行
 * @param line: 一行文本（不含换行）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::handleProbeLine(std::string_view line, uint32_t nowMs) noexcept
{
    const Probe &probe = kProbes[probeAlt_ ? 1U : 0U];
    if (line.starts_with(probe.line)) {
        probeSeen_ = true;
        return;
    }
    if ((line == kOk) && probeSeen_) {
        resync_ = false;
        timeouts_ = 0U;
        ++stats_.resyncs;
        state_ = State::kIdle;
        startFront(nowMs);
        return;
    }
    if (isUrc(line)) {
        dispatchUrc(line);
        return;
    }
    if (line != probe.echo) {
        ++stats_.stale;
    }
}

/**
 * @brief 以时间戳判定在途命令/握手是否已超时；超时先于其后喂入的字节生效
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::expire(uint32_t nowMs) noexcept
{
    if ((count_ == 0U) || (state_ == State::kIdle)) {
        return;
    }
    if ((state_ == State::kQuiet) && reached(nowMs, lastRxMs_ + kQuietMs)) {
        // 通道已静默：发探针（旧响应已被丢弃），或对 quietFirst 命令直接发出。
        if (quietThenProbe_) {
            sendProbe(nowMs);
        } else {
            quietDone_ = true;
            state_ = State::kIdle;
            sendFront(nowMs);
        }
        return;
    }
    if (!reached(nowMs, deadlineMs_)) {
        return;
    }
    if ((state_ == State::kProbe) || (state_ == State::kQuiet)) {
        ++stats_.timeouts;
        ++timeouts_;
        state_ = State::kIdle;
        if (timeouts_ >= deadThreshold_) {
            dead_ = true;
            abortAll(nowMs);
            return;
        }
        startFront(nowMs);
        return;
    }
    finish(AtResult::kTimeout, 0, nowMs);
}

/**
 * @brief 结束队首命令并回调 onDone
 * @param result: result
 * @param code: code
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::finish(AtResult result, int32_t code, uint32_t nowMs) noexcept
{
    if (count_ == 0U) {
        return;
    }
    const Slot done = queue_[head_];
    head_ = (head_ + 1U) % kQueueDepth;
    --count_;
    state_ = State::kIdle;
    lineLen_ = 0U;
    overflow_ = false;
    skipSpace_ = false;
    rawRemaining_ = 0U;
    if (result == AtResult::kTimeout) {
        ++stats_.timeouts;
        ++timeouts_;
        if (timeouts_ >= deadThreshold_) {
            dead_ = true;
        }
    } else if (result != AtResult::kAborted) {
        timeouts_ = 0U;
    }
    if ((result == AtResult::kTimeout) && resyncEnabled_) {
        resync_ = true; // 迟到的终结行可能随后到达：下一条命令前先握手隔离
    }
    if (done.onDone != nullptr) {
        done.onDone(done.ctx, AtResponse{result, code, nowMs, done.token});
    }
    if (dead_) {
        abortAll(nowMs);
        return;
    }
    startFront(nowMs);
}

/**
 * @brief 以给定结果终止全部在途/排队命令
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::abortAll(uint32_t nowMs) noexcept
{
    // 仅处理调用时已入队的命令，避免回调内重新提交造成死循环。
    size_t remaining = count_;
    while ((count_ != 0U) && (remaining != 0U)) {
        --remaining;
        const Slot done = queue_[head_];
        head_ = (head_ + 1U) % kQueueDepth;
        --count_;
        state_ = State::kIdle;
        if (done.onDone != nullptr) {
            done.onDone(done.ctx, AtResponse{AtResult::kAborted, 0, nowMs, done.token});
        }
    }
}

void AtEngine::reset(uint32_t nowMs) noexcept
{
    const bool inflight = state_ != State::kIdle;
    abortAll(nowMs);
    // 取消在途命令后其应答仍可能到达：保留/置位待同步标记，由下一条命令前的握手隔离。
    resync_ = resyncEnabled_ && (inflight || resync_);
    count_ = 0U;
    head_ = 0U;
    state_ = State::kIdle;
    lineLen_ = 0U;
    overflow_ = false;
    skipSpace_ = false;
    rawRemaining_ = 0U;
    timeouts_ = 0U;
    dead_ = false;
    quietDone_ = false;
}

void AtEngine::poll(uint32_t nowMs) noexcept
{
    if (count_ == 0U) {
        return;
    }
    if (state_ == State::kIdle) {
        startFront(nowMs);
        return;
    }
    expire(nowMs);
}

void AtEngine::onRxLost(uint32_t nowMs) noexcept
{
    ++stats_.rxLost;
    if (lineLen_ != 0U) {
        overflow_ = true; // 半行内容不可信：丢弃到下一个换行
    }
    lastRxMs_ = nowMs;
    switch (state_) {
    case State::kRaw:
    case State::kWaitResponse:
    case State::kWaitPrompt:
        // 丢了字节后，在途命令的应答已不完整：立即失败（不再等一个孤立的 OK），并隔离后续旧响应。
        resync_ = resyncEnabled_;
        finish(AtResult::kError, 0, nowMs);
        break;
    case State::kProbe:
        resync_ = true;
        enterQuiet(nowMs, true);
        break;
    case State::kQuiet:
    case State::kIdle:
    default:
        break;
    }
}

uint32_t AtEngine::msToDeadline(uint32_t nowMs) const noexcept
{
    if ((count_ == 0U) || (state_ == State::kIdle)) {
        return std::numeric_limits<uint32_t>::max();
    }
    uint32_t due = deadlineMs_;
    if (state_ == State::kQuiet) {
        const uint32_t quietEnd = lastRxMs_ + kQuietMs;
        if (static_cast<int32_t>(quietEnd - due) < 0) {
            due = quietEnd;
        }
    }
    if (reached(nowMs, due)) {
        return 0U;
    }
    return due - nowMs;
}

void AtEngine::feed(std::span<const uint8_t> bytes, uint32_t nowMs) noexcept
{
    // 以时间戳而非调用顺序判超时：截止之后才到的字节属于迟到响应，不得完成该命令。
    expire(nowMs);
    if (!bytes.empty()) {
        lastRxMs_ = nowMs;
    }
    if (state_ == State::kQuiet) {
        if (!bytes.empty()) {
            ++stats_.stale;
        }
        return; // 静默等待：旧响应一律丢弃，到达的字节同时刷新静默计时
    }
    size_t index = 0U;
    while (index < bytes.size()) {
        if (state_ == State::kRaw) {
            if (count_ == 0U) {
                state_ = State::kIdle;
                continue;
            }
            const size_t take = std::min(rawRemaining_, bytes.size() - index);
            const Slot &slot = queue_[head_];
            if (slot.onRaw != nullptr) {
                slot.onRaw(slot.ctx, bytes.subspan(index, take));
            }
            rawRemaining_ -= take;
            index += take;
            if (rawRemaining_ == 0U) {
                state_ = State::kWaitResponse;
                lineLen_ = 0U;
            }
            continue;
        }
        onByte(bytes[index], nowMs);
        ++index;
    }
}

/**
 * @brief 逐字节喂入行装配器，遇换行时交给 handleLine
 * @param byte: byte
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::onByte(uint8_t byte, uint32_t nowMs) noexcept
{
    if (byte == '\n') {
        const bool dropped = overflow_;
        const size_t len = lineLen_;
        lineLen_ = 0U;
        overflow_ = false;
        skipSpace_ = false;
        if (dropped) {
            ++stats_.overflow;
            return;
        }
        if (len != 0U) {
            handleLine(std::string_view{line_.data(), len}, nowMs);
        }
        return;
    }
    if (byte == '\r') {
        return;
    }
    if (skipSpace_) {
        skipSpace_ = false;
        if (byte == ' ') {
            return;
        }
    }
    if ((byte == '>') && (lineLen_ == 0U) && (state_ == State::kWaitPrompt)) {
        const Slot &slot = queue_[head_];
        state_ = State::kWaitResponse;
        skipSpace_ = true;
        if ((port_.write == nullptr) || !port_.write(port_.ctx, slot.payload)) {
            ++stats_.writeFails;
            deadlineMs_ = nowMs;
        }
        return;
    }
    if (lineLen_ < kLineBytes) {
        line_[lineLen_] = static_cast<char>(byte);
        ++lineLen_;
    } else {
        overflow_ = true;
    }
}

/**
 * @brief 判断一行是否为已登记 URC 前缀
 * @param line: 一行文本（不含换行）
 * @retval true 成功/条件成立
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool AtEngine::isUrc(std::string_view line) const noexcept
{
    return std::any_of(port_.urcPrefixes.begin(), port_.urcPrefixes.end(),
                       [line](std::string_view prefix) { return line.starts_with(prefix); });
}

/**
 * @brief 判断 URC 前缀行是否属于队首命令的查询应答
 * @param line: 一行文本（不含换行）
 * @retval true 由命令自带的形状判定函数确认
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。命令未提供 match 时一律按 URC 处理。
 */
bool AtEngine::belongsToFront(std::string_view line) const noexcept
{
    if (count_ == 0U) {
        return false;
    }
    const Slot &slot = queue_[head_];
    if ((slot.prefixLen != 0U) &&
        line.starts_with(std::string_view{slot.prefix.data(), slot.prefixLen})) {
        return true;
    }
    return (slot.match != nullptr) && slot.match(line);
}

/**
 * @brief 把 URC 行派发给上层回调
 * @param line: 一行文本（不含换行）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::dispatchUrc(std::string_view line) noexcept
{
    ++stats_.urcs;
    if (port_.onUrc != nullptr) {
        port_.onUrc(port_.ctx, line);
    }
}

// NOLINTBEGIN(readability-function-size): 行分类按 ADR-0021 §2.3 顺序平铺（回显/终结/URC/原始长度），拆分会割裂归属规则，主机测试逐分支覆盖。
/**
 * @brief 处理一行完整文本：URC 分流、响应行转发或终结行判定
 * @param line: 一行文本（不含换行）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void AtEngine::handleLine(std::string_view line, uint32_t nowMs) noexcept
{
    ++stats_.lines;
    if (count_ == 0U) {
        if (isUrc(line)) {
            dispatchUrc(line);
        } else {
            ++stats_.stray;
        }
        return;
    }
    if (state_ == State::kProbe) {
        handleProbeLine(line, nowMs);
        return;
    }
    const Slot &slot = queue_[head_];
    // 回显：与在途命令文本完全一致的行直接丢弃（ATE1 状态下）。
    if (line == std::string_view{slot.text.data(), slot.len}) {
        return;
    }
    if ((line == kOk) || (line == kSendOk)) {
        finish(AtResult::kOk, 0, nowMs);
        return;
    }
    if ((line == kError) || (line == kSendFail)) {
        finish(AtResult::kError, 0, nowMs);
        return;
    }
    if (line.starts_with(kCmeError) || line.starts_with(kCmsError)) {
        int32_t code = 0;
        const std::string_view detail = subview(line, line.find(':') + 1U);
        if (!parseInt(detail, code)) {
            code = cmeCodeFromText(detail); // +CMEE=2 的文本形式
        }
        finish(AtResult::kCmeError, code, nowMs);
        return;
    }
    if (isUrc(line) && !belongsToFront(line)) {
        dispatchUrc(line);
        return;
    }
    if (slot.onLine != nullptr) {
        slot.onLine(slot.ctx, line);
    }
    if (!slot.rawPrefix.empty() && line.starts_with(slot.rawPrefix)) {
        int32_t rawLen = 0;
        if (!parseInt(subview(line, slot.rawPrefix.size()), rawLen) || (rawLen < 0) ||
            (static_cast<size_t>(rawLen) > slot.rawMax)) {
            // 长度字段非法或超过请求上限：帧边界不可信，终止命令并强制重新同步。
            ++stats_.protocol;
            resync_ = true;
            finish(AtResult::kError, 0, nowMs);
            return;
        }
        if (rawLen > 0) {
            // rawLen 已校验 > 0，转 size_t 无损。
            rawRemaining_ = static_cast<size_t>(rawLen);
            state_ = State::kRaw;
        }
    }
}
// NOLINTEND(readability-function-size)

} // namespace cellular
