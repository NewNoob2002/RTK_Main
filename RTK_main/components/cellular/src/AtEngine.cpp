// AT 命令引擎实现（ADR-0021 §2.4）。仅 kNet 任务调用；不阻塞、不分配堆。
#include "cellular/AtEngine.hpp"

#include <algorithm>
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
    const Slot &slot = queue_[head_];
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
    if (done.onDone != nullptr) {
        done.onDone(done.ctx, AtResponse{result, code, nowMs});
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
            done.onDone(done.ctx, AtResponse{AtResult::kAborted, 0, nowMs});
        }
    }
}

void AtEngine::reset(uint32_t nowMs) noexcept
{
    abortAll(nowMs);
    count_ = 0U;
    head_ = 0U;
    state_ = State::kIdle;
    lineLen_ = 0U;
    overflow_ = false;
    skipSpace_ = false;
    rawRemaining_ = 0U;
    timeouts_ = 0U;
    dead_ = false;
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
    if (reached(nowMs, deadlineMs_)) {
        finish(AtResult::kTimeout, 0, nowMs);
    }
}

uint32_t AtEngine::msToDeadline(uint32_t nowMs) const noexcept
{
    if ((count_ == 0U) || (state_ == State::kIdle)) {
        return std::numeric_limits<uint32_t>::max();
    }
    if (reached(nowMs, deadlineMs_)) {
        return 0U;
    }
    return deadlineMs_ - nowMs;
}

void AtEngine::feed(std::span<const uint8_t> bytes, uint32_t nowMs) noexcept
{
    size_t index = 0U;
    while (index < bytes.size()) {
        if (state_ == State::kRaw) {
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
 * @brief 判断该行是否属于队首命令的响应
 * @param line: 一行文本（不含换行）
 * @retval true 成功/条件成立
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool AtEngine::belongsToFront(std::string_view line) const noexcept
{
    if (count_ == 0U) {
        return false;
    }
    const size_t colon = line.find(':');
    if ((colon == std::string_view::npos) || (colon == 0U)) {
        return false;
    }
    const Slot &slot = queue_[head_];
    const std::string_view cmd{slot.text.data(), slot.len};
    return cmd.find(subview(line, 0U, colon)) != std::string_view::npos;
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
    const Slot &slot = queue_[head_];
    // 回显：与在途命令文本完全一致的行直接丢弃（ATE1 状态下）。
    if (line == std::string_view{slot.text.data(), slot.len}) {
        return;
    }
    if (line == kOk) {
        finish(AtResult::kOk, 0, nowMs);
        return;
    }
    if (line == kSendOk) {
        finish(AtResult::kOk, 0, nowMs);
        return;
    }
    if ((line == kError) || (line == kSendFail)) {
        finish(AtResult::kError, 0, nowMs);
        return;
    }
    if (line.starts_with(kCmeError) || line.starts_with(kCmsError)) {
        int32_t code = 0;
        static_cast<void>(parseInt(subview(line, line.find(':') + 1U), code));
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
        if (parseInt(subview(line, slot.rawPrefix.size()), rawLen) && (rawLen > 0)) {
            // rawLen 已校验 > 0，转 size_t 无损。
            rawRemaining_ = static_cast<size_t>(rawLen);
            state_ = State::kRaw;
        }
    }
}

} // namespace cellular
