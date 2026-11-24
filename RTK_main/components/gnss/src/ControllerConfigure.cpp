// GNSS 控制器状态机：配置脚本、报文输出、运行守护与读回（ADR-0010 §2.6、ADR-0012 §2.4/§2.7）。
// 线程安全：否（单任务顺序调用）。ISR 安全：否（会改写控制器状态）。
// 堆与阻塞：不使用堆、不阻塞（时间由调用方传入 nowMs）；纯逻辑不读写 UART/波特率/电源，只经 Action 让 main 侧执行。
#include "ControllerImpl.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "gnss/NullReceiver.hpp"
#include "UbloxExt.hpp"

namespace gnss {
namespace {

using detail::kReadbackIdleMs;
using detail::reached;

constexpr uint8_t kCfgPhaseResyncRetry = 3U;
constexpr uint8_t kCfgPhaseResyncAdvance = 4U;
constexpr uint8_t kCfgPhaseFallbackSend = 5U;
constexpr uint8_t kOutPhaseResyncRetry = 2U;
constexpr uint8_t kOutPhaseResyncAdvance = 3U;

} // namespace

/**
 * @brief 判断当前配置步骤是否为可降级跳过的 F9P NMEA 静默步骤（RU1-01）。
 * @param self: 控制器。
 * @retval true F9P 12 键静默 VALSET；false 其他关键步骤。
 */
bool ControllerImpl::isNonfatalSilenceStep(const Controller &self) noexcept
{
    return (self.detected().probe.model == Model::kZedF9p) &&
           ublox::isSilenceNmeaValset(self.step_.cmd.view());
}
/**
 * @brief kConfiguring：按 phase 分派发送命令 / 等应答 / 切波特率。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作。
 * @retval kAgain: 需重新分派。
 * @retval kDone: 本轮结束。
 */
ControllerImpl::Round ControllerImpl::onConfiguring(Controller &self, uint32_t nowMs,
                                                    Action &out) noexcept
{
    if ((self.cfg_ == nullptr) || (self.detected_.receiver == nullptr)) {
        enterFailed(self, nowMs);
        return Round::kAgain;
    }
    switch (self.phase_) {
    case 0:
        return configSend(self, nowMs, out);
    case 1:
        return self.waiting_reply_ ? configAwait(self, nowMs, out)
                                   : configNoReply(self, nowMs, out);
    case 2:
        return configSwitchBaud(self, nowMs, out);
    case kCfgPhaseResyncRetry:
    case kCfgPhaseResyncAdvance:
        return configResync(self, nowMs, out);
    case kCfgPhaseFallbackSend: {
        ++self.stats_.stepsSent;
        self.waiting_reply_ = self.step_.expectReply;
        self.reply_ok_ = false;
        self.reply_nack_ = false;
        armDeadline(self, nowMs, self.step_.timeoutMs);
        self.phase_ = 1;
        out = write(self.step_.cmd.view());
        return Round::kDone;
    }
    default:
        return Round::kDone;
    }
}

/**
 * @brief 取脚本下一步并下发；脚本结束时进入报文输出阶段。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（kWrite）。
 * @retval kAgain: 脚本已完成，需重新分派。
 * @retval kDone: 命令已下发，等应答。
 */
ControllerImpl::Round ControllerImpl::configSend(Controller &self, uint32_t nowMs,
                                                 Action &out) noexcept
{
    ConfigStep step{};
    if (!self.detected_.receiver->roverStep(self.step_index_, self.detected_.probe.model,
                                            *self.cfg_, step)) {
        // 基础脚本结束：进入报文输出阶段（ADR-0012 §2.4），结束后才到 kRunning。
        enterOutput(self, false);
        return Round::kAgain;
    }
    self.step_ = step; // 命令字节必须在应答到达前一直有效
    ++self.stats_.stepsSent;
    self.waiting_reply_ = self.step_.expectReply;
    self.reply_ok_ = false;
    self.reply_nack_ = false;
    armDeadline(self, nowMs, self.step_.timeoutMs);
    self.phase_ = 1;
    out = write(self.step_.cmd.view());
    return Round::kDone;
}

/**
 * @brief 配置步收到 NAK/超时后执行重试、重同步或进入失败态。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param isValset: 当前命令是否为 VALSET（需要 300 ms 重同步窗）。
 * @retval kAgain/kDone 本轮分派结果。
 */
ControllerImpl::Round ControllerImpl::configRetryOrFail(Controller &self, uint32_t nowMs,
                                                        bool isValset) noexcept
{
    if (self.step_tries_ < self.timing_.stepRetries) {
        ++self.step_tries_;
        if (isValset) {
            armDeadline(self, nowMs, ublox::kValsetResyncWindowMs);
            self.phase_ = kCfgPhaseResyncRetry;
            return Round::kDone;
        }
        self.phase_ = 0;
        return Round::kAgain;
    }
    enterFailed(self, nowMs);
    return Round::kAgain;
}

/**
 * @brief 等配置应答：ACK/NAK/超时决定重试、切波特率或判失败。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 需重新分派。
 * @retval kDone: 本轮结束（继续等）。
 */
ControllerImpl::Round ControllerImpl::configAwait(Controller &self, uint32_t nowMs,
                                                  Action &out) noexcept
{
    static_cast<void>(out);
    const bool isValset = ublox::isValsetFrame(self.step_.cmd.view());
    const bool nonfatal = isNonfatalSilenceStep(self);
    if (self.reply_ok_ || self.reply_nack_) {
        const bool ok = self.reply_ok_ && !self.reply_nack_;
        self.waiting_reply_ = false;
        if (!ok) {
            ++self.stats_.stepNacks;
            if (nonfatal) {
                advanceStep(self);
                return Round::kAgain;
            }
            CmdBuf fallback{};
            if ((self.detected_.probe.model == Model::kZedF9p) &&
                ublox::buildPortConfigFallbackValset(self.step_.cmd.view(), fallback)) {
                // HPG 1.12 若拒绝 CFG-NMEA-HIGHPREC/SVNUMBERING，降级为 3 键基础端口配置继续。
                // 不清零 step_tries_：降级帧再被拒时经 configRetryOrFail 计数，重试会重新生成
                // 5 键帧，清零会让 5 键 NAK → 降级 NAK → 重试无限循环。
                self.step_.cmd = fallback;
                self.phase_ = kCfgPhaseFallbackSend;
                return Round::kAgain;
            }
            return configRetryOrFail(self, nowMs, isValset);
        }
        if (self.step_.switchBaud != 0U) {
            self.phase_ = 2;
            return Round::kAgain;
        }
        advanceStep(self);
        return Round::kAgain;
    }
    if (!reached(nowMs, self.deadline_ms_)) {
        return Round::kDone;
    }
    ++self.stats_.stepTimeouts;
    self.waiting_reply_ = false;
    if (nonfatal) {
        armDeadline(self, nowMs, ublox::kValsetResyncWindowMs);
        self.phase_ = kCfgPhaseResyncAdvance;
        return Round::kDone;
    }
    return configRetryOrFail(self, nowMs, isValset);
}

/**
 * @brief 配置阶段 300 ms VALSET 重同步等待：期间丢弃迟到 ACK/NAK，结束后重试或推进下一步。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 重同步完成，需重新分派。
 * @retval kDone: 仍在重同步窗口内。
 */
ControllerImpl::Round ControllerImpl::configResync(Controller &self, uint32_t nowMs,
                                                   Action &out) noexcept
{
    static_cast<void>(out);
    if (!reached(nowMs, self.deadline_ms_)) {
        return Round::kDone;
    }
    if (self.phase_ == kCfgPhaseResyncAdvance) {
        advanceStep(self);
    } else {
        self.phase_ = 0;
    }
    return Round::kAgain;
}

/**
 * @brief 无需应答的步骤：发送后立即推进；需要切波特率的进 phase 2。
 * @param self: 控制器。
 * @param nowMs: 当前时间（本状态不使用）。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 需重新分派。
 */
ControllerImpl::Round ControllerImpl::configNoReply(Controller &self, uint32_t nowMs,
                                                    Action &out) noexcept
{
    static_cast<void>(nowMs);
    static_cast<void>(out);
    // 无需应答的步骤：发送后立即推进；需要改波特率的在 phase 2 处理。
    if (self.step_.switchBaud != 0U) {
        self.phase_ = 2;
        return Round::kAgain;
    }
    advanceStep(self);
    return Round::kAgain;
}

/**
 * @brief 切换主机波特率，并让 detected_.baud 跟随配置结果。
 * @param self: 控制器。
 * @param nowMs: 当前时间（本状态不使用）。
 * @param out: 输出动作（kSetBaud）。
 * @retval kDone: 本轮结束（等 main 切完波特率）。
 */
ControllerImpl::Round ControllerImpl::configSwitchBaud(Controller &self, uint32_t nowMs,
                                                       Action &out) noexcept
{
    static_cast<void>(nowMs);
    self.host_baud_ = self.step_.switchBaud;
    self.detected_.baud = self.host_baud_; // 探测结论里的“当前波特率”跟随配置切换
    advanceStep(self);
    out = setBaud(self.host_baud_);
    return Round::kDone;
}

/**
 * @brief kOutput：phase 0 逐项下发输出配置，phase 1 等应答。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作。
 * @retval kAgain: 需重新分派。
 * @retval kDone: 本轮结束。
 */
ControllerImpl::Round ControllerImpl::onOutput(Controller &self, uint32_t nowMs,
                                               Action &out) noexcept
{
    if (self.detected_.receiver == nullptr) {
        enterFailed(self, nowMs);
        return Round::kAgain;
    }
    if (self.phase_ == 1) {
        return outputAwait(self, nowMs, out);
    }
    if ((self.phase_ == kOutPhaseResyncRetry) || (self.phase_ == kOutPhaseResyncAdvance)) {
        return outputResync(self, nowMs, out);
    }
    return outputSend(self, nowMs, out);
}

/**
 * @brief 下发下一条输出配置；列表跑完回到 kRunning 并自增 outputSeq。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（kWrite）。
 * @retval kAgain: 需重新分派（跳过或完成后）。
 * @retval kDone: 命令已下发，等应答。
 */
ControllerImpl::Round ControllerImpl::outputSend(Controller &self, uint32_t nowMs,
                                                 Action &out) noexcept
{
    if (self.output_index_ >= kOutputMsgCount) {
        // 输出阶段完成：结果掩码留在 stats_，seq 自增供 main 判断“我请求的那次完成了”。
        self.state_ = State::kRunning;
        self.last_frame_ms_ = nowMs;
        self.phase_ = 0;
        ++self.stats_.outputRuns;
        ++self.stats_.outputSeq;
        return Round::kAgain;
    }
    const auto msg = static_cast<OutputMsg>(static_cast<uint8_t>(self.output_index_));
    const uint32_t bit = 1U << static_cast<uint32_t>(self.output_index_);
    if ((self.plan_.periodMs[self.output_index_] == 0U) && !self.output_rerun_) {
        // 开机首轮：关闭的语句本来就没开，不发命令（host 黄金脚本按此序列断言）。
        ++self.output_index_;
        return Round::kAgain;
    }
    self.output_cmd_.clear();
    const fw::Status built = self.detected_.receiver->buildOutput(
        msg, self.plan_.periodMs[self.output_index_], self.detected_.probe.model, self.plan_,
        self.output_cmd_);
    if (built == fw::Status::kNotSupported) {
        // 型号不支持不是错误（ADR-0012 §2.5）：跳过并记入 unsupportedMask。
        self.stats_.unsupportedMask |= bit;
        ++self.output_index_;
        return Round::kAgain;
    }
    if (!fw::isOk(built) || (self.output_cmd_.len == 0U)) {
        self.stats_.failedMask |= bit;
        ++self.output_index_;
        return Round::kAgain;
    }
    ++self.stats_.stepsSent;
    self.waiting_reply_ = true;
    self.reply_ok_ = false;
    self.reply_nack_ = false;
    armDeadline(self, nowMs, kStepTimeoutMs);
    self.phase_ = 1;
    out = write(self.output_cmd_.view());
    return Round::kDone;
}

/**
 * @brief 等输出配置应答：成功记 appliedMask，失败重试或记 failedMask（不判整体失败）。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 需重新分派。
 * @retval kDone: 本轮结束（继续等）。
 */
ControllerImpl::Round ControllerImpl::outputAwait(Controller &self, uint32_t nowMs,
                                                  Action &out) noexcept
{
    static_cast<void>(out);
    const uint32_t bit = 1U << static_cast<uint32_t>(self.output_index_);
    const bool isValset = ublox::isValsetFrame(self.output_cmd_.view());
    if (self.reply_ok_ || self.reply_nack_) {
        const bool ok = self.reply_ok_ && !self.reply_nack_;
        self.waiting_reply_ = false;
        if (!ok) {
            ++self.stats_.stepNacks;
        }
        if (ok) {
            self.stats_.appliedMask |= bit;
        } else if (self.step_tries_ < self.timing_.stepRetries) {
            ++self.step_tries_;
            if (isValset) {
                armDeadline(self, nowMs, ublox::kValsetResyncWindowMs);
                self.phase_ = kOutPhaseResyncRetry;
                return Round::kDone;
            }
            self.phase_ = 0;
            return Round::kAgain;
        } else {
            // 单项失败只记 failedMask，不把整个模块判定为 kFailed。
            self.stats_.failedMask |= bit;
        }
        advanceOutput(self);
        return Round::kAgain;
    }
    if (!reached(nowMs, self.deadline_ms_)) {
        return Round::kDone;
    }
    ++self.stats_.stepTimeouts;
    self.waiting_reply_ = false;
    if (self.step_tries_ < self.timing_.stepRetries) {
        ++self.step_tries_;
        if (isValset) {
            armDeadline(self, nowMs, ublox::kValsetResyncWindowMs);
            self.phase_ = kOutPhaseResyncRetry;
            return Round::kDone;
        }
        self.phase_ = 0;
        return Round::kAgain;
    }
    self.stats_.failedMask |= bit;
    if (isValset) {
        armDeadline(self, nowMs, ublox::kValsetResyncWindowMs);
        self.phase_ = kOutPhaseResyncAdvance;
        return Round::kDone;
    }
    advanceOutput(self);
    return Round::kAgain;
}

/**
 * @brief 输出阶段 300 ms VALSET 重同步等待：期间丢弃迟到 ACK/NAK，结束后重试或推进下一项。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 重同步完成，需重新分派。
 * @retval kDone: 仍在重同步窗口内。
 */
ControllerImpl::Round ControllerImpl::outputResync(Controller &self, uint32_t nowMs,
                                                   Action &out) noexcept
{
    static_cast<void>(out);
    if (!reached(nowMs, self.deadline_ms_)) {
        return Round::kDone;
    }
    if (self.phase_ == kOutPhaseResyncAdvance) {
        advanceOutput(self);
    } else {
        self.phase_ = 0;
    }
    return Round::kAgain;
}

/**
 * @brief kRunning：模块静默超过 silenceMs 即触发重探。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（本状态不使用）。
 * @retval kAgain: 已转入探测，需重新分派。
 * @retval kDone: 本轮结束。
 */
ControllerImpl::Round ControllerImpl::onRunning(Controller &self, uint32_t nowMs,
                                                Action &out) noexcept
{
    static_cast<void>(out);
    if (static_cast<int32_t>(nowMs - self.last_frame_ms_) >
        static_cast<int32_t>(self.timing_.silenceMs)) {
        ++self.stats_.reprobes;
        self.detected_ = Detected{};
        enterProbing(self);
        return Round::kAgain;
    }
    return Round::kDone;
}

/**
 * @brief 读回阶段：先下发 CONFIG，再按“行间静默 1 s / 总时长 5 s”收尾。
 *
 * 静默截止时间不存成员（Controller 布局冻结），直接由 last_frame_ms_ 推得；读回期间只有 $CONFIG 行会
 * 刷新它（见 Controller::onFrame），所以模块持续输出的 NMEA 不会把窗口无限延长。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @param out: 输出动作（kDone 时有效）。
 * @retval kDone: 命令刚发出，或窗口还没到。
 * @retval kAgain: 读回结束，已回到 kRunning。
 */
ControllerImpl::Round ControllerImpl::onReadback(Controller &self, uint32_t nowMs,
                                                 Action &out) noexcept
{
    if (self.phase_ == 0) {
        self.phase_ = 1;
        self.last_frame_ms_ = nowMs;
        out = write(self.output_cmd_.view());
        return Round::kDone;
    }
    // reached() 用的是 int32 差值，now/last_frame_ms_ 回绕时仍然单调，无需另存截止时间字段。
    const bool idleDone = reached(nowMs, self.last_frame_ms_ + kReadbackIdleMs);
    const bool totalDone = reached(nowMs, self.deadline_ms_);
    if (!idleDone && !totalDone) {
        return Round::kDone;
    }
    ++self.stats_.readbackSeq;
    self.state_ = State::kRunning;
    self.phase_ = 0;
    // 回 kRunning 后重新起算，避免刚读完就立刻触发静默重探测。
    self.last_frame_ms_ = nowMs;
    return Round::kAgain;
}

} // namespace gnss
