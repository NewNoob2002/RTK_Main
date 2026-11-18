// GNSS 控制器状态机：探测与失败重试（ADR-0010 §2.6）——断电/上电、候选枚举、探测下发与等待；配置/输出/运行/读回见 ControllerConfigure.cpp。
// 线程安全：否（单任务顺序调用）。ISR 安全：否（会改写控制器状态）。
// 堆与阻塞：不使用堆、不阻塞（时间由调用方传入 nowMs）；纯逻辑不读写 UART/波特率/电源，只经 Action 让 main 侧执行。
#include "ControllerImpl.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "gnss/NullReceiver.hpp"

namespace gnss {
namespace {

using detail::reached;

/** 一次探测尝试：驱动 + 波特率。 */
struct Candidate {
    const Receiver *receiver = nullptr;
    uint32_t baud = 0;
};

/**
 * @brief 从启用驱动里取出与提示同家族的那个（提示组合在探测顺序里排最前）。
 * @param receivers: 启用的驱动表。
 * @param hint: 上次探测的提示。
 * @retval 指针: 同家族驱动；无提示或未启用时 nullptr。
 */
const Receiver *hintReceiver(std::span<const Receiver *const> receivers, const Hint &hint) noexcept
{
    if (hint.family == Family::kUnknown) {
        return nullptr;
    }
    for (const Receiver *receiver : receivers) {
        if (receiver != nullptr && receiver->family() == hint.family) {
            return receiver;
        }
    }
    return nullptr;
}

/**
 * @brief 探测候选总数：提示组合（若有）+ 启用方言 × 波特率全表。
 * @param receivers: 启用的驱动表。
 * @param bauds: 候选波特率表。
 * @param hint: 探测提示。
 * @retval 数量: 候选组合个数。
 */
size_t candidateCount(std::span<const Receiver *const> receivers, std::span<const uint32_t> bauds,
                      const Hint &hint) noexcept
{
    size_t count = receivers.size() * bauds.size();
    if (hintReceiver(receivers, hint) != nullptr) {
        ++count; // 提示组合排在最前，优先探测
    }
    return count;
}

/**
 * @brief 按探测顺序取第 index 个候选（提示组合在前，方言为外层循环）。
 * @param receivers: 启用的驱动表。
 * @param bauds: 候选波特率表。
 * @param hint: 探测提示。
 * @param index: 候选序号。
 * @param out: 输出候选；越界时不改写。
 * @retval true: 取到候选。
 * @retval false: index 越界。
 */
bool candidateAt(std::span<const Receiver *const> receivers, std::span<const uint32_t> bauds,
                 const Hint &hint, size_t index, Candidate &out) noexcept
{
    const Receiver *hinted = hintReceiver(receivers, hint);
    if (hinted != nullptr) {
        if (index == 0) {
            out.receiver = hinted;
            out.baud = hint.baud != 0 ? hint.baud : bauds.front();
            return true;
        }
        --index;
    }
    const size_t baudCount = bauds.size();
    if (baudCount == 0 || index >= receivers.size() * baudCount) {
        return false;
    }
    out.receiver = receivers[index / baudCount];
    out.baud = bauds[index % baudCount];
    return true;
}
} // namespace

/**
 * @brief kOff：stop() 置 phase_ = 1 时先补发一次断电动作。
 * @param self: 控制器。
 * @param nowMs: 当前时间（本状态不使用）。
 * @param out: 输出动作。
 * @retval kDone: 本轮有结论。
 */
ControllerImpl::Round ControllerImpl::onOff(Controller &self, uint32_t nowMs, Action &out) noexcept
{
    static_cast<void>(nowMs);
    if (self.phase_ == 1) {
        // stop() 置 phase_ = 1：下一轮先发一次断电动作。
        self.phase_ = 0;
        out = power(false);
    }
    return Round::kDone;
}

/**
 * @brief kPowerUp：先断电一段时间，再上电并等 bootMs 后进入探测。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作。
 * @retval kAgain: 状态已迁移，需重新分派。
 * @retval kDone: 本轮结束（等定时）。
 */
ControllerImpl::Round ControllerImpl::onPowerUp(Controller &self, uint32_t nowMs,
                                                Action &out) noexcept
{
    if (self.phase_ == 0) {
        armDeadline(self, nowMs, self.timing_.powerOffMs);
        self.phase_ = 1;
        out = power(false);
        return Round::kDone;
    }
    if (self.phase_ == 1) {
        if (!reached(nowMs, self.deadline_ms_)) {
            return Round::kDone;
        }
        self.phase_ = 2;
        armDeadline(self, nowMs, self.timing_.bootMs);
        out = power(true);
        return Round::kDone;
    }
    if (!reached(nowMs, self.deadline_ms_)) {
        return Round::kDone;
    }
    enterProbing(self);
    return Round::kAgain;
}

/**
 * @brief kProbing：复位后先等启动窗口，再下发探测、等应答或超时换候选。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作。
 * @retval kAgain: 需重新分派。
 * @retval kDone: 本轮结束。
 */
ControllerImpl::Round ControllerImpl::onProbing(Controller &self, uint32_t nowMs,
                                                Action &out) noexcept
{
    if (self.phase_ == kProbeBootWaitPhase) {
        if (!reached(nowMs, self.deadline_ms_)) {
            return Round::kDone;
        }
        self.phase_ = 0U;
    }
    if (self.phase_ == 0) {
        return probeStart(self, nowMs, out);
    }
    return probeAwait(self, nowMs, out);
}

/**
 * @brief 探测下发：取下一个候选写探测命令；全表跑满轮数仍未命中则进入 kFailed。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作。
 * @retval kAgain: 需重新分派。
 * @retval kDone: 命令已下发，等应答。
 */
ControllerImpl::Round ControllerImpl::probeStart(Controller &self, uint32_t nowMs,
                                                 Action &out) noexcept
{
    const size_t total = candidateCount(self.receivers_, self.bauds_, self.hint_);
    if (total == 0U) {
        enterFailed(self, nowMs);
        return Round::kAgain;
    }
    if (self.probe_index_ >= total) {
        ++self.probe_round_; // 完成一轮全表探测
        self.probe_index_ = 0;
        if (self.probe_round_ >= self.timing_.probeRounds) {
            enterFailed(self, nowMs);
            return Round::kAgain;
        }
    }
    Candidate candidate{};
    if (!candidateAt(self.receivers_, self.bauds_, self.hint_, self.probe_index_, candidate)) {
        enterFailed(self, nowMs);
        return Round::kAgain;
    }
    if (self.host_baud_ != candidate.baud) {
        self.host_baud_ = candidate.baud;
        out = setBaud(candidate.baud);
        return Round::kDone;
    }
    self.probe_receiver_ = candidate.receiver;
    self.probe_cmd_.clear();
    // 复位后的 UART2 可能在第一轮仍未启动，每轮都在出厂速率重试协议恢复。
    const bool factoryProbe = self.factory_reset_probe_ && candidate.baud == self.hint_.baud &&
                              candidate.receiver->family() == self.hint_.family;
    const bool built =
        factoryProbe ? candidate.receiver->buildFactoryResetProbe(self.cfg_->port, self.probe_cmd_)
                     : candidate.receiver->buildProbe(self.probe_cmd_);
    if (!built) {
        ++self.probe_index_; // 驱动自身无法生成查询命令：跳过该组合
        return Round::kAgain;
    }
    ++self.stats_.probesSent;
    self.waiting_reply_ = true;
    self.probe_hit_ = false;
    self.phase_ = 1;
    armDeadline(self, nowMs, self.timing_.probeTimeoutMs);
    out = write(self.probe_cmd_.view());
    return Round::kDone;
}

/**
 * @brief 探测等待：命中则进配置（onFrame 已写好 detected_），超时则换下一个候选。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 需重新分派。
 * @retval kDone: 本轮结束（继续等）。
 */
ControllerImpl::Round ControllerImpl::probeAwait(Controller &self, uint32_t nowMs,
                                                 Action &out) noexcept
{
    static_cast<void>(out);
    if (self.probe_hit_) {
        self.factory_reset_probe_ = false;
        // onFrame() 已把型号与波特率写进 detected_，这里只做状态迁移。
        enterConfiguring(self);
        return Round::kAgain;
    }
    if (!reached(nowMs, self.deadline_ms_)) {
        return Round::kDone;
    }
    self.waiting_reply_ = false;
    ++self.probe_index_;
    self.phase_ = 0;
    return Round::kAgain;
}

/**
 * @brief kFailed：按 retryBackoffMs 退避后回到上电序列。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 退避结束，需重新分派。
 * @retval kDone: 仍在退避。
 */
ControllerImpl::Round ControllerImpl::onFailed(Controller &self, uint32_t nowMs,
                                               Action &out) noexcept
{
    static_cast<void>(out);
    if (!reached(nowMs, self.deadline_ms_)) {
        return Round::kDone;
    }
    self.state_ = State::kPowerUp;
    self.phase_ = 0;
    return Round::kAgain;
}

} // namespace gnss
