// GNSS 控制器状态机的内部实现声明（组件私有头，ADR-0010 §2.6；`friend struct` 见 Controller.hpp），公共 API 在 Controller.cpp。
// 线程安全：否（单任务顺序调用）。ISR 安全：否（声明含状态成员）。
// 堆与阻塞：不使用堆、不阻塞；声明与静态动作构造在此，状态处理在 ControllerProbe.cpp 与 ControllerConfigure.cpp。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "gnss/Controller.hpp"

namespace gnss {

struct ControllerImpl {
    static constexpr uint8_t kProbeBootWaitPhase = 2U;

    /** 一轮分派的结果：kAgain = 状态已迁移，需要重新分派；kDone = 本轮有结论（out 可能为空动作）。 */
    enum class Round : uint8_t {
        kAgain,
        kDone,
    };

    /**
     * @brief 空动作（本轮无事可做）。
     * @retval action: kind = kNone。
     */
    static Action none() noexcept
    {
        return Action{};
    }

    /**
     * @brief 电源动作。
     * @param on: true 上电，false 断电。
     * @retval action: kind = kPower。
     */
    static Action power(bool on) noexcept
    {
        Action action{};
        action.kind = Action::Kind::kPower;
        action.on = on;
        return action;
    }

    /**
     * @brief 切换主机 UART 波特率的动作。
     * @param baud: 目标波特率。
     * @retval action: kind = kSetBaud。
     */
    static Action setBaud(uint32_t baud) noexcept
    {
        Action action{};
        action.kind = Action::Kind::kSetBaud;
        action.baud = baud;
        return action;
    }

    /**
     * @brief 写 UART 的动作（bytes 指向控制器成员，下一次 poll/onFrame 前有效）。
     * @param bytes: 待写字节。
     * @retval action: kind = kWrite。
     */
    static Action write(std::span<const uint8_t> bytes) noexcept
    {
        Action action{};
        action.kind = Action::Kind::kWrite;
        action.bytes = bytes;
        return action;
    }

    /**
     * @brief 设置当前等待的截止时间。
     * @param self: 控制器。
     * @param nowMs: 当前时间。
     * @param timeoutMs: 超时（毫秒）。
     */
    static void armDeadline(Controller &self, uint32_t nowMs, uint32_t timeoutMs) noexcept
    {
        self.deadline_ms_ = nowMs + timeoutMs;
    }

    /**
     * @brief 进入探测：清空探测游标与当前应答状态。
     * @param self: 控制器。
     */
    static void enterProbing(Controller &self) noexcept
    {
        self.state_ = State::kProbing;
        self.phase_ = 0;
        self.probe_index_ = 0;
        self.probe_round_ = 0;
        self.probe_hit_ = false;
        self.waiting_reply_ = false;
    }

    /**
     * @brief 探测命中：定格型号/能力并进入配置脚本。
     * @param self: 控制器（读 detected_，写 caps/state_/phase_）。
     */
    static void enterConfiguring(Controller &self) noexcept
    {
        // 能力必须问“具体型号的那个驱动”：家族驱动对 UM982 只报 UM980 能力（ADR-0012 §2.8）。
        self.detected_.caps = self.detected_.receiver != nullptr
                                  ? self.detected_.receiver->caps(self.detected_.probe.model)
                                  : 0U;
        self.state_ = State::kConfiguring;
        self.phase_ = 0;
        self.step_index_ = 0;
        self.step_tries_ = 0;
        self.waiting_reply_ = false;
        self.reply_ok_ = false;
        self.reply_nack_ = false;
        self.probe_hit_ = false;
    }

    /**
     * @brief 进入报文输出阶段并复位本轮结果掩码。
     * @param self: 控制器。
     * @param rerun: true = 运行期改动（关闭项要显式下发 UNLOG），false = 开机首轮（跳过周期 0 的项）。
     */
    static void enterOutput(Controller &self, bool rerun) noexcept
    {
        self.state_ = State::kOutput;
        self.phase_ = 0;
        self.output_index_ = 0;
        self.output_rerun_ = rerun;
        self.step_tries_ = 0;
        self.waiting_reply_ = false;
        self.reply_ok_ = false;
        self.reply_nack_ = false;
        self.stats_.appliedMask = 0U;
        self.stats_.unsupportedMask = 0U;
        self.stats_.failedMask = 0U;
    }

    /**
     * @brief 进入 kFailed：计数并设置重试退避。
     * @param self: 控制器。
     * @param nowMs: 当前时间。
     */
    static void enterFailed(Controller &self, uint32_t nowMs) noexcept
    {
        self.state_ = State::kFailed;
        ++self.stats_.failures;
        armDeadline(self, nowMs, self.timing_.retryBackoffMs);
    }

    /**
     * @brief 推进到脚本下一步（清空本步重试计数与子阶段）。
     * @param self: 控制器。
     */
    static void advanceStep(Controller &self) noexcept
    {
        ++self.step_index_;
        self.step_tries_ = 0;
        self.phase_ = 0;
    }

    /**
     * @brief 推进到输出列表下一项（清空本项重试计数与子阶段）。
     * @param self: 控制器。
     */
    static void advanceOutput(Controller &self) noexcept
    {
        ++self.output_index_;
        self.step_tries_ = 0;
        self.phase_ = 0;
    }

    /**
     * @brief 记录一次应答判定结果。
     * @param self: 控制器。
     * @param reply: 驱动给出的判定（kNone 忽略）。
     */
    static void markReply(Controller &self, Ack reply) noexcept
    {
        if (reply == Ack::kAck) {
            self.reply_ok_ = true;
        } else if (reply == Ack::kNack) {
            self.reply_nack_ = true;
        }
    }

    static Round onOff(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round onPowerUp(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round onProbing(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round onConfiguring(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round onOutput(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round onRunning(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round onReadback(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round onFailed(Controller &self, uint32_t nowMs, Action &out) noexcept;

    static Round probeStart(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round probeAwait(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round configSend(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round configAwait(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round configNoReply(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round configSwitchBaud(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round outputSend(Controller &self, uint32_t nowMs, Action &out) noexcept;
    static Round outputAwait(Controller &self, uint32_t nowMs, Action &out) noexcept;

    /**
     * @brief 按当前状态分派一轮。
     * @param self: 控制器。
     * @param nowMs: 当前时间。
     * @param out: 输出动作（kDone 时有效）。
     * @retval kAgain: 状态已迁移，调用方应重新分派。
     * @retval kDone: 本轮结束。
     */
    static Round dispatch(Controller &self, uint32_t nowMs, Action &out) noexcept
    {
        switch (self.state_) {
        case State::kOff:
            return onOff(self, nowMs, out);
        case State::kPowerUp:
            return onPowerUp(self, nowMs, out);
        case State::kProbing:
            return onProbing(self, nowMs, out);
        case State::kConfiguring:
            return onConfiguring(self, nowMs, out);
        case State::kOutput:
            return onOutput(self, nowMs, out);
        case State::kRunning:
            return onRunning(self, nowMs, out);
        case State::kReadback:
            return onReadback(self, nowMs, out);
        case State::kFailed:
            return onFailed(self, nowMs, out);
        default:
            return Round::kDone;
        }
    }

    /**
     * @brief 处理工作模式多步维护命令的单步应答与重试。
     * @param self: 控制器。
     * @param nowMs: 当前时间。
     * @retval action: 若进入下一步或重试则返回待发送动作，否则为空。
     */
    static Action handleModeAck(Controller &self, uint32_t nowMs) noexcept;
};

namespace detail {

/**
 * @brief 判断 nowMs 是否已到截止时间（int32 差值，支持 uint32 回绕）。
 * @param nowMs: 当前时间。
 * @param deadlineMsMs: 截止时间。
 * @retval true: 已到或已过。
 * @retval false: 未到。
 */
inline bool reached(uint32_t nowMs, uint32_t deadlineMsMs) noexcept
{
    return static_cast<int32_t>(nowMs - deadlineMsMs) >= 0;
}

// 读回窗口（ADR-0012 §2.7）：行间静默超过 1 s 即收尾，避免永远停在 kReadback。
inline constexpr uint32_t kReadbackIdleMs = 1000U;

} // namespace detail

} // namespace gnss
