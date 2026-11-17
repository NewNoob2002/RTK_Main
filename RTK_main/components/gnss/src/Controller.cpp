// GNSS 探测/配置/运行状态机实现（ADR-0010 §2.6，驱动接口见 ADR-0012 §2.2）。纯逻辑：不读写
// UART、不改波特率、不操作电源， 只通过 poll() 返回 Action 让 main 侧执行；所有时间由调用方传入 now_ms。
// 结构（P3r）：poll() 只保留外层循环与守卫，每个状态一个 ControllerImpl::on_*；迁移、计时与"发一步、推进一步"
// 这类重复动作抽成命名函数，状态顺序、计数器与 stats 更新时机与原实现逐行对应。
// 线程/调用约束：非线程安全，单任务顺序调用；不可阻塞、不可在 ISR 中使用。
// 堆：不使用。时间比较统一用 int32 差值，支持 uint32 回绕。
#include "gnss/Controller.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "gnss/NullReceiver.hpp"

namespace gnss {
namespace {

struct Candidate {
    const Receiver *receiver = nullptr;
    uint32_t baud = 0;
};

const Receiver *hint_receiver(std::span<const Receiver *const> receivers, const Hint &hint) noexcept
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

size_t candidate_count(std::span<const Receiver *const> receivers, std::span<const uint32_t> bauds,
                       const Hint &hint) noexcept
{
    size_t count = receivers.size() * bauds.size();
    if (hint_receiver(receivers, hint) != nullptr) {
        ++count; // 提示组合排在最前，优先探测
    }
    return count;
}

// 探测顺序：提示的（方言, 波特率）→ 启用方言 × 波特率全表（方言为外层循环）。
bool candidate_at(std::span<const Receiver *const> receivers, std::span<const uint32_t> bauds,
                  const Hint &hint, size_t index, Candidate &out) noexcept
{
    const Receiver *hinted = hint_receiver(receivers, hint);
    if (hinted != nullptr) {
        if (index == 0) {
            out.receiver = hinted;
            out.baud = hint.baud != 0 ? hint.baud : bauds.front();
            return true;
        }
        --index;
    }
    const size_t baud_count = bauds.size();
    if (baud_count == 0 || index >= receivers.size() * baud_count) {
        return false;
    }
    out.receiver = receivers[index / baud_count];
    out.baud = bauds[index % baud_count];
    return true;
}

bool reached(uint32_t now_ms, uint32_t deadline_ms_ms) noexcept
{
    return static_cast<int32_t>(now_ms - deadline_ms_ms) >= 0;
}

/** 单次 poll 内允许的状态迁移次数（原实现的 guard 上限）。 */
constexpr uint8_t kPollGuardRounds = 16;

// 读回窗口（ADR-0012 §2.7）：行间静默超过 1 s 或总时长超过 5 s 即收尾，避免永远停在 kReadback。
constexpr uint32_t kReadbackIdleMs = 1000U;
constexpr uint32_t kReadbackTotalMs = 5000U;

} // namespace

/** 状态机内部实现（friend struct，见 Controller.hpp）：只通过 `Controller &self` 访问私有状态。 */
struct ControllerImpl {
    /** 一轮分派的结果：kAgain = 状态已迁移，需要重新分派；kDone = 本轮有结论（out 可能为空动作）。 */
    enum class Round : uint8_t {
        kAgain,
        kDone,
    };

    /** @brief 空动作（本轮无事可做）。 */
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
    static Action set_baud(uint32_t baud) noexcept
    {
        Action action{};
        action.kind = Action::Kind::kSetBaud;
        action.baud = baud;
        return action;
    }

    /**
     * @brief 写 UART 的动作（bytes 指向控制器成员，下一次 poll/on_frame 前有效）。
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
     * @param now_ms: 当前时间。
     * @param timeout_ms: 超时（毫秒）。
     */
    static void arm_deadline(Controller &self, uint32_t now_ms, uint32_t timeout_ms) noexcept
    {
        self.deadline_ms_ = now_ms + timeout_ms;
    }

    /** @brief 进入探测：清空探测游标与当前应答状态。 */
    static void enter_probing(Controller &self) noexcept
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
    static void enter_configuring(Controller &self) noexcept
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
    static void enter_output(Controller &self, bool rerun) noexcept
    {
        self.state_ = State::kOutput;
        self.phase_ = 0;
        self.output_index_ = 0;
        self.output_rerun_ = rerun;
        self.step_tries_ = 0;
        self.waiting_reply_ = false;
        self.reply_ok_ = false;
        self.reply_nack_ = false;
        self.stats_.applied_mask = 0U;
        self.stats_.unsupported_mask = 0U;
        self.stats_.failed_mask = 0U;
    }

    /** @brief 进入 kFailed：计数并设置重试退避。 */
    static void enter_failed(Controller &self, uint32_t now_ms) noexcept
    {
        self.state_ = State::kFailed;
        ++self.stats_.failures;
        arm_deadline(self, now_ms, self.timing_.retry_backoff_ms);
    }

    /** @brief 推进到脚本下一步（清空本步重试计数与子阶段）。 */
    static void advance_step(Controller &self) noexcept
    {
        ++self.step_index_;
        self.step_tries_ = 0;
        self.phase_ = 0;
    }

    /** @brief 推进到输出列表下一项（清空本项重试计数与子阶段）。 */
    static void advance_output(Controller &self) noexcept
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
    static void mark_reply(Controller &self, Ack reply) noexcept
    {
        if (reply == Ack::kAck) {
            self.reply_ok_ = true;
        } else if (reply == Ack::kNack) {
            self.reply_nack_ = true;
        }
    }

    static Round on_off(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round on_power_up(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round on_probing(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round on_configuring(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round on_output(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round on_running(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round on_readback(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round on_failed(Controller &self, uint32_t now_ms, Action &out) noexcept;

    static Round probe_start(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round probe_await(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round config_send(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round config_await(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round config_no_reply(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round config_switch_baud(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round output_send(Controller &self, uint32_t now_ms, Action &out) noexcept;
    static Round output_await(Controller &self, uint32_t now_ms, Action &out) noexcept;

    /**
     * @brief 按当前状态分派一轮。
     * @param self: 控制器。
     * @param now_ms: 当前时间。
     * @param out: 输出动作（kDone 时有效）。
     * @retval kAgain: 状态已迁移，调用方应重新分派。
     * @retval kDone: 本轮结束。
     */
    static Round dispatch(Controller &self, uint32_t now_ms, Action &out) noexcept
    {
        switch (self.state_) {
        case State::kOff:
            return on_off(self, now_ms, out);
        case State::kPowerUp:
            return on_power_up(self, now_ms, out);
        case State::kProbing:
            return on_probing(self, now_ms, out);
        case State::kConfiguring:
            return on_configuring(self, now_ms, out);
        case State::kOutput:
            return on_output(self, now_ms, out);
        case State::kRunning:
            return on_running(self, now_ms, out);
        case State::kReadback:
            return on_readback(self, now_ms, out);
        case State::kFailed:
            return on_failed(self, now_ms, out);
        default:
            return Round::kDone;
        }
    }
};

ControllerImpl::Round ControllerImpl::on_off(Controller &self, uint32_t now_ms,
                                             Action &out) noexcept
{
    static_cast<void>(now_ms);
    if (self.phase_ == 1) {
        // stop() 置 phase_ = 1：下一轮先发一次断电动作。
        self.phase_ = 0;
        out = power(false);
    }
    return Round::kDone;
}

ControllerImpl::Round ControllerImpl::on_power_up(Controller &self, uint32_t now_ms,
                                                  Action &out) noexcept
{
    if (self.phase_ == 0) {
        arm_deadline(self, now_ms, self.timing_.power_off_ms);
        self.phase_ = 1;
        out = power(false);
        return Round::kDone;
    }
    if (self.phase_ == 1) {
        if (!reached(now_ms, self.deadline_ms_)) {
            return Round::kDone;
        }
        self.phase_ = 2;
        arm_deadline(self, now_ms, self.timing_.boot_ms);
        out = power(true);
        return Round::kDone;
    }
    if (!reached(now_ms, self.deadline_ms_)) {
        return Round::kDone;
    }
    enter_probing(self);
    return Round::kAgain;
}

ControllerImpl::Round ControllerImpl::on_probing(Controller &self, uint32_t now_ms,
                                                 Action &out) noexcept
{
    if (self.phase_ == 0) {
        return probe_start(self, now_ms, out);
    }
    return probe_await(self, now_ms, out);
}

ControllerImpl::Round ControllerImpl::probe_start(Controller &self, uint32_t now_ms,
                                                  Action &out) noexcept
{
    const size_t total = candidate_count(self.receivers_, self.bauds_, self.hint_);
    if (total == 0U) {
        enter_failed(self, now_ms);
        return Round::kAgain;
    }
    if (self.probe_index_ >= total) {
        ++self.probe_round_; // 完成一轮全表探测
        self.probe_index_ = 0;
        if (self.probe_round_ >= self.timing_.probe_rounds) {
            enter_failed(self, now_ms);
            return Round::kAgain;
        }
    }
    Candidate candidate{};
    if (!candidate_at(self.receivers_, self.bauds_, self.hint_, self.probe_index_, candidate)) {
        enter_failed(self, now_ms);
        return Round::kAgain;
    }
    if (self.host_baud_ != candidate.baud) {
        self.host_baud_ = candidate.baud;
        out = set_baud(candidate.baud);
        return Round::kDone;
    }
    self.probe_receiver_ = candidate.receiver;
    self.probe_cmd_.clear();
    if (!candidate.receiver->build_probe(self.probe_cmd_)) {
        ++self.probe_index_; // 驱动自身无法生成查询命令：跳过该组合
        return Round::kAgain;
    }
    ++self.stats_.probes_sent;
    self.waiting_reply_ = true;
    self.probe_hit_ = false;
    self.phase_ = 1;
    arm_deadline(self, now_ms, self.timing_.probe_timeout_ms);
    out = write(self.probe_cmd_.view());
    return Round::kDone;
}

ControllerImpl::Round ControllerImpl::probe_await(Controller &self, uint32_t now_ms,
                                                  Action &out) noexcept
{
    static_cast<void>(out);
    if (self.probe_hit_) {
        // on_frame() 已把型号与波特率写进 detected_，这里只做状态迁移。
        enter_configuring(self);
        return Round::kAgain;
    }
    if (!reached(now_ms, self.deadline_ms_)) {
        return Round::kDone;
    }
    self.waiting_reply_ = false;
    ++self.probe_index_;
    self.phase_ = 0;
    return Round::kAgain;
}

ControllerImpl::Round ControllerImpl::on_configuring(Controller &self, uint32_t now_ms,
                                                     Action &out) noexcept
{
    if ((self.cfg_ == nullptr) || (self.detected_.receiver == nullptr)) {
        enter_failed(self, now_ms);
        return Round::kAgain;
    }
    switch (self.phase_) {
    case 0:
        return config_send(self, now_ms, out);
    case 1:
        return self.waiting_reply_ ? config_await(self, now_ms, out)
                                   : config_no_reply(self, now_ms, out);
    case 2:
        return config_switch_baud(self, now_ms, out);
    default:
        return Round::kDone;
    }
}

ControllerImpl::Round ControllerImpl::config_send(Controller &self, uint32_t now_ms,
                                                  Action &out) noexcept
{
    ConfigStep step{};
    if (!self.detected_.receiver->rover_step(self.step_index_, self.detected_.probe.model,
                                             *self.cfg_, step)) {
        // 基础脚本结束：进入报文输出阶段（ADR-0012 §2.4），结束后才到 kRunning。
        enter_output(self, false);
        return Round::kAgain;
    }
    self.step_ = step; // 命令字节必须在应答到达前一直有效
    ++self.stats_.steps_sent;
    self.waiting_reply_ = self.step_.expect_reply;
    self.reply_ok_ = false;
    self.reply_nack_ = false;
    arm_deadline(self, now_ms, self.step_.timeout_ms);
    self.phase_ = 1;
    out = write(self.step_.cmd.view());
    return Round::kDone;
}

ControllerImpl::Round ControllerImpl::config_await(Controller &self, uint32_t now_ms,
                                                   Action &out) noexcept
{
    static_cast<void>(out);
    if (self.reply_ok_ || self.reply_nack_) {
        const bool ok = self.reply_ok_ && !self.reply_nack_;
        self.waiting_reply_ = false;
        if (!ok) {
            ++self.stats_.step_nacks;
            if (self.step_tries_ < self.timing_.step_retries) {
                ++self.step_tries_;
                self.phase_ = 0;
                return Round::kAgain;
            }
            enter_failed(self, now_ms);
            return Round::kAgain;
        }
        if (self.step_.switch_baud != 0U) {
            self.phase_ = 2;
            return Round::kAgain;
        }
        advance_step(self);
        return Round::kAgain;
    }
    if (!reached(now_ms, self.deadline_ms_)) {
        return Round::kDone;
    }
    ++self.stats_.step_timeouts;
    self.waiting_reply_ = false;
    if (self.step_tries_ < self.timing_.step_retries) {
        ++self.step_tries_;
        self.phase_ = 0;
        return Round::kAgain;
    }
    enter_failed(self, now_ms);
    return Round::kAgain;
}

ControllerImpl::Round ControllerImpl::config_no_reply(Controller &self, uint32_t now_ms,
                                                      Action &out) noexcept
{
    static_cast<void>(now_ms);
    static_cast<void>(out);
    // 无需应答的步骤：发送后立即推进；需要改波特率的在 phase 2 处理。
    if (self.step_.switch_baud != 0U) {
        self.phase_ = 2;
        return Round::kAgain;
    }
    advance_step(self);
    return Round::kAgain;
}

ControllerImpl::Round ControllerImpl::config_switch_baud(Controller &self, uint32_t now_ms,
                                                         Action &out) noexcept
{
    static_cast<void>(now_ms);
    self.host_baud_ = self.step_.switch_baud;
    self.detected_.baud = self.host_baud_; // 探测结论里的“当前波特率”跟随配置切换
    advance_step(self);
    out = set_baud(self.host_baud_);
    return Round::kDone;
}

ControllerImpl::Round ControllerImpl::on_output(Controller &self, uint32_t now_ms,
                                                Action &out) noexcept
{
    if (self.detected_.receiver == nullptr) {
        enter_failed(self, now_ms);
        return Round::kAgain;
    }
    if (self.phase_ == 1) {
        return output_await(self, now_ms, out);
    }
    return output_send(self, now_ms, out);
}

ControllerImpl::Round ControllerImpl::output_send(Controller &self, uint32_t now_ms,
                                                  Action &out) noexcept
{
    if (self.output_index_ >= kOutputMsgCount) {
        // 输出阶段完成：结果掩码留在 stats_，seq 自增供 main 判断“我请求的那次完成了”。
        self.state_ = State::kRunning;
        self.last_frame_ms_ = now_ms;
        self.phase_ = 0;
        ++self.stats_.output_runs;
        ++self.stats_.output_seq;
        return Round::kAgain;
    }
    const auto msg = static_cast<OutputMsg>(static_cast<uint8_t>(self.output_index_));
    const uint32_t bit = 1U << static_cast<uint32_t>(self.output_index_);
    if ((self.plan_.period_ms[self.output_index_] == 0U) && !self.output_rerun_) {
        // 开机首轮：关闭的语句本来就没开，不发命令（与 P3a 黄金脚本一致）。
        ++self.output_index_;
        return Round::kAgain;
    }
    self.output_cmd_.clear();
    const fw::Status built = self.detected_.receiver->build_output(
        msg, self.plan_.period_ms[self.output_index_], self.detected_.probe.model, self.plan_,
        self.output_cmd_);
    if (built == fw::Status::kNotSupported) {
        // 型号不支持不是错误（ADR-0012 §2.5）：跳过并记入 unsupported_mask。
        self.stats_.unsupported_mask |= bit;
        ++self.output_index_;
        return Round::kAgain;
    }
    if (!fw::is_ok(built) || (self.output_cmd_.len == 0U)) {
        self.stats_.failed_mask |= bit;
        ++self.output_index_;
        return Round::kAgain;
    }
    ++self.stats_.steps_sent;
    self.waiting_reply_ = true;
    self.reply_ok_ = false;
    self.reply_nack_ = false;
    arm_deadline(self, now_ms, kStepTimeoutMs);
    self.phase_ = 1;
    out = write(self.output_cmd_.view());
    return Round::kDone;
}

ControllerImpl::Round ControllerImpl::output_await(Controller &self, uint32_t now_ms,
                                                   Action &out) noexcept
{
    static_cast<void>(out);
    const uint32_t bit = 1U << static_cast<uint32_t>(self.output_index_);
    if (self.reply_ok_ || self.reply_nack_) {
        const bool ok = self.reply_ok_ && !self.reply_nack_;
        self.waiting_reply_ = false;
        if (!ok) {
            ++self.stats_.step_nacks;
        }
        if (ok) {
            self.stats_.applied_mask |= bit;
        } else if (self.step_tries_ < self.timing_.step_retries) {
            ++self.step_tries_;
            self.phase_ = 0;
            return Round::kAgain;
        } else {
            // 单项失败只记 failed_mask，不把整个模块判定为 kFailed。
            self.stats_.failed_mask |= bit;
        }
        advance_output(self);
        return Round::kAgain;
    }
    if (!reached(now_ms, self.deadline_ms_)) {
        return Round::kDone;
    }
    ++self.stats_.step_timeouts;
    self.waiting_reply_ = false;
    if (self.step_tries_ < self.timing_.step_retries) {
        ++self.step_tries_;
        self.phase_ = 0;
        return Round::kAgain;
    }
    self.stats_.failed_mask |= bit;
    advance_output(self);
    return Round::kAgain;
}

ControllerImpl::Round ControllerImpl::on_running(Controller &self, uint32_t now_ms,
                                                 Action &out) noexcept
{
    static_cast<void>(out);
    if (static_cast<int32_t>(now_ms - self.last_frame_ms_) >
        static_cast<int32_t>(self.timing_.silence_ms)) {
        ++self.stats_.reprobes;
        self.detected_ = Detected{};
        enter_probing(self);
        return Round::kAgain;
    }
    return Round::kDone;
}

/**
 * @brief 读回阶段：先下发 CONFIG，再按“行间静默 1 s / 总时长 5 s”收尾。
 *
 * 静默截止时间不存成员（Controller 布局冻结），直接由 last_frame_ms_ 推得；读回期间只有 $CONFIG 行会
 * 刷新它（见 Controller::on_frame），所以模块持续输出的 NMEA 不会把窗口无限延长。
 * @param self: 控制器。
 * @param now_ms: 当前时间。
 * @param out: 输出动作（kDone 时有效）。
 * @retval kDone: 命令刚发出，或窗口还没到。
 * @retval kAgain: 读回结束，已回到 kRunning。
 */
ControllerImpl::Round ControllerImpl::on_readback(Controller &self, uint32_t now_ms,
                                                  Action &out) noexcept
{
    if (self.phase_ == 0) {
        self.phase_ = 1;
        self.last_frame_ms_ = now_ms;
        out = write(self.output_cmd_.view());
        return Round::kDone;
    }
    // reached() 用的是 int32 差值，now/last_frame_ms_ 回绕时仍然单调，无需另存截止时间字段。
    const bool idle_done = reached(now_ms, self.last_frame_ms_ + kReadbackIdleMs);
    const bool total_done = reached(now_ms, self.deadline_ms_);
    if (!idle_done && !total_done) {
        return Round::kDone;
    }
    ++self.stats_.readback_seq;
    self.state_ = State::kRunning;
    self.phase_ = 0;
    // 回 kRunning 后重新起算，避免刚读完就立刻触发静默重探测。
    self.last_frame_ms_ = now_ms;
    return Round::kAgain;
}

ControllerImpl::Round ControllerImpl::on_failed(Controller &self, uint32_t now_ms,
                                                Action &out) noexcept
{
    static_cast<void>(out);
    if (!reached(now_ms, self.deadline_ms_)) {
        return Round::kDone;
    }
    self.state_ = State::kPowerUp;
    self.phase_ = 0;
    return Round::kAgain;
}

bool Controller::init(std::span<const Receiver *const> receivers, std::span<const uint32_t> bauds,
                      const RoverConfig &cfg, const OutputPlan &plan, const Hint &hint,
                      const Timing &timing) noexcept
{
    if (receivers.empty() || bauds.empty()) {
        return false;
    }
    for (const Receiver *receiver : receivers) {
        if (receiver == nullptr) {
            return false;
        }
    }
    for (const uint32_t baud : bauds) {
        if (baud == 0) {
            return false;
        }
    }
    receivers_ = receivers;
    bauds_ = bauds;
    cfg_ = &cfg;
    plan_ = plan;
    hint_ = hint;
    timing_ = timing;
    host_baud_ = bauds.front();
    state_ = State::kOff;
    phase_ = 0;
    probe_index_ = 0;
    probe_round_ = 0;
    step_index_ = 0;
    step_tries_ = 0;
    output_index_ = 0;
    waiting_reply_ = false;
    reply_ok_ = false;
    reply_nack_ = false;
    probe_hit_ = false;
    detected_ = Detected{};
    stats_ = ControllerStats{};
    return true;
}

void Controller::start(uint32_t now_ms) noexcept
{
    state_ = State::kPowerUp;
    phase_ = 0;
    probe_index_ = 0;
    probe_round_ = 0;
    probe_hit_ = false;
    waiting_reply_ = false;
    detected_ = Detected{};
    last_frame_ms_ = now_ms;
    deadline_ms_ = now_ms;
}

void Controller::stop(uint32_t now_ms) noexcept
{
    state_ = State::kOff;
    phase_ = 1; // 下一轮 poll 先发一次断电动作
    waiting_reply_ = false;
    deadline_ms_ = now_ms;
}

void Controller::request_reprobe(uint32_t now_ms) noexcept
{
    if (state_ != State::kOff) {
        ++stats_.reprobes;
    }
    state_ = State::kProbing;
    phase_ = 0;
    probe_index_ = 0;
    probe_round_ = 0;
    probe_hit_ = false;
    waiting_reply_ = false;
    detected_ = Detected{};
    last_frame_ms_ = now_ms;
}

void Controller::request_reconfigure(uint32_t now_ms) noexcept
{
    if (detected_.receiver == nullptr) {
        request_reprobe(now_ms);
        return;
    }
    state_ = State::kConfiguring;
    phase_ = 0;
    step_index_ = 0;
    step_tries_ = 0;
    output_index_ = 0;
    waiting_reply_ = false;
    reply_ok_ = false;
    reply_nack_ = false;
}

// 只重跑输出阶段：探测/基础脚本已经成功的事实不变，因此不动 detected_，也不改状态机之外的东西。
// output_seq 每完成一轮 kOutput 自增一次，main 侧用它判断“这次提交是否落地”（ADR-0012 §2.4）。
bool Controller::request_output(const OutputPlan &plan, uint32_t now_ms) noexcept
{
    static_cast<void>(now_ms);
    if (state_ != State::kRunning || detected_.receiver == nullptr) {
        return false;
    }
    plan_ = plan;
    state_ = State::kOutput;
    phase_ = 0;
    output_index_ = 0;
    step_tries_ = 0;
    waiting_reply_ = false;
    reply_ok_ = false;
    reply_nack_ = false;
    stats_.applied_mask = 0U;
    stats_.unsupported_mask = 0U;
    stats_.failed_mask = 0U;
    output_rerun_ = true; // 运行时改动：关闭项要显式下发 UNLOG
    return true;
}

/**
 * @brief 读回模块当前配置（ADR-0012 §2.7）。
 *
 * 只在 kRunning 且驱动支持读回时成立：命令写进 output_cmd_（kReadback 只能从 kRunning 进入，与 kOutput
 * 阶段不会重叠，故复用同一缓冲），随后 poll() 负责下发、on_frame() 负责计数。
 * @param now_ms: 当前时间，用于起算总时长。
 * @retval true: 已进入 kReadback。
 * @retval false: 状态不对或驱动不支持，状态未改变。
 */
bool Controller::request_readback(uint32_t now_ms) noexcept
{
    if ((state_ != State::kRunning) || (detected_.receiver == nullptr)) {
        return false;
    }
    if (!detected_.receiver->build_readback(output_cmd_)) {
        return false; // 驱动不支持读回（u-blox/ComNav）：不进 kReadback，也不动 stats。
    }
    state_ = State::kReadback;
    phase_ = 0;
    waiting_reply_ = false;
    stats_.readback_lines = 0U;
    last_frame_ms_ = now_ms;
    deadline_ms_ = now_ms + kReadbackTotalMs;
    return true;
}

Action Controller::poll(uint32_t now_ms) noexcept
{
    Action action{};
    for (uint8_t guard = 0; guard < kPollGuardRounds; ++guard) {
        action = Action{};
        if (ControllerImpl::dispatch(*this, now_ms, action) == ControllerImpl::Round::kDone) {
            return action;
        }
    }
    return Action{};
}

/**
 * @brief 把一帧协议数据交给当前阶段：探测时试匹配型号，等待应答时判 ACK/NACK。
 * @param dec: 该帧的解码器。
 * @param frame: 整帧字节。
 * @param now_ms: 当前时间（只用于判“模块还在说话”）。
 */
void Controller::on_frame(const demux::Decoder &dec, std::span<const uint8_t> frame,
                          uint32_t now_ms) noexcept
{
    if (state_ == State::kReadback) {
        // 读回窗口只认 $CONFIG 行：模块照常输出的 NMEA 仍算“在工作”，但不得延长读回窗口。
        std::array<char, 8U> scratch{}; // 只判命中，正文由 main 侧另取，故截断不影响返回值
        if ((detected_.receiver != nullptr) &&
            (detected_.receiver->match_readback(dec, frame, scratch) > 0U)) {
            ++stats_.readback_lines;
            last_frame_ms_ = now_ms;
        }
        return;
    }
    last_frame_ms_ = now_ms; // 任意协议帧都算“模块还在说话”
    if ((state_ == State::kProbing) && (phase_ == 1) && (probe_receiver_ != nullptr)) {
        ProbeResult result{};
        if (probe_receiver_->match_probe(dec, frame, result)) {
            detected_.probe = result;
            // 命中后换成该型号的驱动实例（家族驱动的 for_model 默认返回自身）。
            detected_.receiver = &probe_receiver_->for_model(result.model);
            detected_.baud = host_baud_;
            probe_hit_ = true;
        }
        return;
    }
    if ((state_ == State::kConfiguring) && (phase_ == 1) && waiting_reply_ &&
        (detected_.receiver != nullptr)) {
        ControllerImpl::mark_reply(
            *this, detected_.receiver->classify_reply(dec, frame, step_.cmd.view()));
        return;
    }
    if ((state_ == State::kOutput) && (phase_ == 1) && waiting_reply_ &&
        (detected_.receiver != nullptr)) {
        ControllerImpl::mark_reply(
            *this, detected_.receiver->classify_reply(dec, frame, output_cmd_.view()));
    }
}

const Receiver &Controller::receiver() const noexcept
{
    return detected_.receiver != nullptr ? *detected_.receiver : null_receiver();
}

const Receiver &Controller::probe_receiver() const noexcept
{
    return probe_receiver_ != nullptr ? *probe_receiver_ : null_receiver();
}

/**
 * @brief  控制器状态的日志/控制台短名
 * @param  state: 控制器状态
 * @retval 静态字符串，未知值返回 "unknown"
 */
const char *state_name(State state) noexcept
{
    switch (state) {
    case State::kOff:
        return "off";
    case State::kPowerUp:
        return "power-up";
    case State::kProbing:
        return "probing";
    case State::kConfiguring:
        return "configuring";
    case State::kRunning:
        return "running";
    case State::kFailed:
        return "failed";
    case State::kOutput:
        return "output";
    case State::kReadback:
        return "readback";
    default:
        return "unknown";
    }
}

} // namespace gnss
