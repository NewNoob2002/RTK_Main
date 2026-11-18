// GNSS 探测/配置/运行状态机的公共接口与轮次驱动（ADR-0010 §2.6，驱动接口见 ADR-0012 §2.2）；状态处理见 ControllerProbe.cpp / ControllerConfigure.cpp。
// 线程安全：否（单任务顺序调用）。ISR 安全：否（会读写自身状态）。
// 堆与阻塞：不使用堆、不阻塞；时间比较统一用 int32 差值（支持 uint32 回绕），纯逻辑不读写 UART/波特率/电源，只经 poll() 返回 Action。
#include "gnss/Controller.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "gnss/NullReceiver.hpp"
#include "ControllerImpl.hpp"

namespace gnss {
namespace {

/** 单次 poll 内允许的状态迁移次数（原实现的 guard 上限）。 */
constexpr uint8_t kPollGuardRounds = 16;

// 读回总时长（ADR-0012 §2.7）：静默与总时长都到位才收尾。
constexpr uint32_t kReadbackTotalMs = 5000U;

} // namespace

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
    factory_reset_probe_ = false;
    return true;
}

void Controller::start(uint32_t nowMs) noexcept
{
    factory_reset_probe_ = false;
    state_ = State::kPowerUp;
    phase_ = 0;
    probe_index_ = 0;
    probe_round_ = 0;
    probe_hit_ = false;
    waiting_reply_ = false;
    detected_ = Detected{};
    last_frame_ms_ = nowMs;
    deadline_ms_ = nowMs;
}

void Controller::stop(uint32_t nowMs) noexcept
{
    state_ = State::kOff;
    phase_ = 1; // 下一轮 poll 先发一次断电动作
    waiting_reply_ = false;
    deadline_ms_ = nowMs;
}

void Controller::requestReprobe(uint32_t nowMs) noexcept
{
    factory_reset_probe_ = false;
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
    last_frame_ms_ = nowMs;
}

void Controller::requestReconfigure(uint32_t nowMs) noexcept
{
    if (detected_.receiver == nullptr) {
        requestReprobe(nowMs);
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

bool Controller::requestOutput(const OutputPlan &plan, uint32_t nowMs) noexcept
{
    static_cast<void>(nowMs);
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
    stats_.appliedMask = 0U;
    stats_.unsupportedMask = 0U;
    stats_.failedMask = 0U;
    output_rerun_ = true; // 运行时改动：关闭项要显式下发 UNLOG
    return true;
}

// 命令复用 output_cmd_：kReadback 只能从 kRunning 进入，与 kOutput 阶段不会重叠（详见头文件声明）。
bool Controller::requestReadback(uint32_t nowMs) noexcept
{
    if ((state_ != State::kRunning) || (detected_.receiver == nullptr)) {
        return false;
    }
    if (!detected_.receiver->buildReadback(output_cmd_)) {
        return false; // 驱动不支持读回（u-blox/ComNav）：不进 kReadback，也不动 stats。
    }
    state_ = State::kReadback;
    phase_ = 0;
    waiting_reply_ = false;
    stats_.readbackLines = 0U;
    last_frame_ms_ = nowMs;
    deadline_ms_ = nowMs + kReadbackTotalMs;
    return true;
}

Action Controller::poll(uint32_t nowMs) noexcept
{
    if (state_ == State::kMaintenance) {
        return pollMaintenance(nowMs);
    }
    Action action{};
    for (uint8_t guard = 0; guard < kPollGuardRounds; ++guard) {
        action = Action{};
        if (ControllerImpl::dispatch(*this, nowMs, action) == ControllerImpl::Round::kDone) {
            return action;
        }
    }
    return Action{};
}

void Controller::onFrame(const demux::Decoder &dec, std::span<const uint8_t> frame,
                         uint32_t nowMs) noexcept
{
    if (state_ == State::kReadback) {
        // 读回窗口只认 $CONFIG 行：模块照常输出的 NMEA 仍算“在工作”，但不得延长读回窗口。
        std::array<char, 8U> scratch{}; // 只判命中，正文由 main 侧另取，故截断不影响返回值
        if ((detected_.receiver != nullptr) &&
            (detected_.receiver->matchReadback(dec, frame, scratch) > 0U)) {
            ++stats_.readbackLines;
            last_frame_ms_ = nowMs;
        }
        return;
    }
    last_frame_ms_ = nowMs; // 任意协议帧都算“模块还在说话”
    if (state_ == State::kMaintenance) {
        onMaintenanceFrame(dec, frame);
        return;
    }
    if ((state_ == State::kProbing) && (phase_ == 1) && (probe_receiver_ != nullptr)) {
        ProbeResult result{};
        if (probe_receiver_->matchProbe(dec, frame, result)) {
            detected_.probe = result;
            // 命中后换成该型号的驱动实例（家族驱动的 forModel 默认返回自身）。
            detected_.receiver = &probe_receiver_->forModel(result.model);
            detected_.baud = host_baud_;
            probe_hit_ = true;
        }
        return;
    }
    if ((state_ == State::kConfiguring) && (phase_ == 1) && waiting_reply_ &&
        (detected_.receiver != nullptr)) {
        ControllerImpl::markReply(*this,
                                  detected_.receiver->classifyReply(dec, frame, step_.cmd.view()));
        return;
    }
    if ((state_ == State::kOutput) && (phase_ == 1) && waiting_reply_ &&
        (detected_.receiver != nullptr)) {
        ControllerImpl::markReply(
            *this, detected_.receiver->classifyReply(dec, frame, output_cmd_.view()));
    }
}

const Receiver &Controller::receiver() const noexcept
{
    return detected_.receiver != nullptr ? *detected_.receiver : nullReceiver();
}

const Receiver &Controller::probeReceiver() const noexcept
{
    return probe_receiver_ != nullptr ? *probe_receiver_ : nullReceiver();
}

const char *stateName(State state) noexcept
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
    case State::kMaintenance:
        return "maintenance";
    default:
        return "unknown";
    }
}

} // namespace gnss
