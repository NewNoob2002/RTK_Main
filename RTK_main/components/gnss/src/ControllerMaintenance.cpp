// GNSS 控制器的一次性维护操作与目标串口验证。
// 仅控制器所属任务调用。无阻塞、无分配，只生成 UART 动作。
#include "ControllerImpl.hpp"

namespace gnss {
namespace {

constexpr uint32_t kBaudSettleMs = 100U;
constexpr uint32_t kMaintenanceProbeMs = 700U;

} // namespace

bool Controller::requestSave() noexcept
{
    return beginMaintenance(Maintenance::kSave, 0U);
}

bool Controller::requestFactoryReset() noexcept
{
    return beginMaintenance(Maintenance::kFactoryReset, 0U);
}

bool Controller::requestBaud(uint32_t baud) noexcept
{
    return beginMaintenance(Maintenance::kBaud, baud);
}

void Controller::setProbePreferences(const Hint &hint, std::span<const uint32_t> bauds) noexcept
{
    hint_ = hint;
    bauds_ = bauds;
}

bool Controller::beginMaintenance(Maintenance kind, uint32_t baud) noexcept
{
    if (state_ != State::kRunning || detected_.receiver == nullptr || cfg_ == nullptr) {
        return false;
    }
    pending_maintenance_ = kind;
    step_ = ConfigStep{};
    bool supported = false;
    switch (kind) {
    case Maintenance::kSave:
        supported = receiver().saveStep(step_);
        break;
    case Maintenance::kFactoryReset:
        supported = receiver().factoryResetStep(cfg_->port, detected_.probe, step_);
        break;
    case Maintenance::kBaud:
        supported = baud != 0U && receiver().baudStep(cfg_->port, baud, step_);
        break;
    default:
        break;
    }
    if (!supported) {
        finishMaintenance(MaintenanceResult::kUnsupported);
        return true;
    }
    maintenance_old_baud_ = host_baud_;
    maintenance_phase_ = MaintenancePhase::kSend;
    reply_ok_ = false;
    reply_nack_ = false;
    probe_hit_ = false;
    state_ = State::kMaintenance;
    return true;
}

void Controller::finishMaintenance(MaintenanceResult result) noexcept
{
    stats_.maintenance = pending_maintenance_;
    stats_.maintenanceResult = result;
    ++stats_.maintenanceSeq;
    state_ = State::kRunning;
}

void Controller::restoreMaintenanceBaud(MaintenanceResult result) noexcept
{
    restore_result_ = result;
    maintenance_phase_ = MaintenancePhase::kRestore;
}

void Controller::maintenanceIoFailed(uint32_t nowMs) noexcept
{
    if (state_ != State::kMaintenance) {
        return;
    }
    if (host_baud_ != maintenance_old_baud_ && maintenance_phase_ != MaintenancePhase::kRestore) {
        restoreMaintenanceBaud(MaintenanceResult::kIoError);
        return;
    }
    finishMaintenance(MaintenanceResult::kIoError);
    if (pending_maintenance_ != Maintenance::kSave) {
        requestReprobe(nowMs);
    }
}

Action Controller::pollMaintenance(uint32_t nowMs) noexcept
{
    switch (maintenance_phase_) {
    case MaintenancePhase::kSend:
        if (pending_maintenance_ == Maintenance::kFactoryReset) {
            maintenance_phase_ = MaintenancePhase::kResetSent;
            return ControllerImpl::write(step_.cmd.view());
        }
        maintenance_phase_ =
            step_.expectReply ? MaintenancePhase::kAwaitAck : MaintenancePhase::kSwitch;
        ControllerImpl::armDeadline(*this, nowMs,
                                    step_.expectReply ? step_.timeoutMs : kBaudSettleMs);
        return ControllerImpl::write(step_.cmd.view());
    case MaintenancePhase::kResetSent:
        // 写入失败由调用方在下一次 poll 前报告。启动等待也给旧波特率的 TX 队列留出发送时间。
        hint_ = Hint{receiver().family(), detected_.probe.model, step_.switchBaud};
        finishMaintenance(MaintenanceResult::kOk);
        requestReprobe(nowMs);
        factory_reset_probe_ = true;
        phase_ = ControllerImpl::kProbeBootWaitPhase;
        ControllerImpl::armDeadline(*this, nowMs, timing_.bootMs);
        break;
    case MaintenancePhase::kAwaitAck:
        if (reply_nack_ || reply_ok_ || detail::reached(nowMs, deadline_ms_)) {
            finishMaintenance(
                reply_nack_ ? MaintenanceResult::kNack
                            : (reply_ok_ ? MaintenanceResult::kOk : MaintenanceResult::kTimeout));
        }
        break;
    case MaintenancePhase::kSwitch:
        if (reply_nack_) {
            finishMaintenance(MaintenanceResult::kNack);
        } else if (detail::reached(nowMs, deadline_ms_)) {
            maintenance_phase_ = MaintenancePhase::kProbe;
            host_baud_ = step_.switchBaud;
            return ControllerImpl::setBaud(host_baud_);
        }
        break;
    case MaintenancePhase::kProbe:
    case MaintenancePhase::kAwaitProbe:
        return pollMaintenanceProbe(nowMs);
    case MaintenancePhase::kRestore:
        host_baud_ = maintenance_old_baud_;
        hint_ = Hint{receiver().family(), detected_.probe.model, host_baud_};
        finishMaintenance(restore_result_);
        requestReprobe(nowMs);
        return ControllerImpl::setBaud(host_baud_);
    default:
        break;
    }
    return {};
}

Action Controller::pollMaintenanceProbe(uint32_t nowMs) noexcept
{
    if (maintenance_phase_ == MaintenancePhase::kProbe) {
        if (!receiver().buildProbe(probe_cmd_)) {
            restoreMaintenanceBaud(MaintenanceResult::kUnsupported);
            return {};
        }
        maintenance_phase_ = MaintenancePhase::kAwaitProbe;
        ControllerImpl::armDeadline(*this, nowMs, kMaintenanceProbeMs);
        ++stats_.probesSent;
        return ControllerImpl::write(probe_cmd_.view());
    }
    if (probe_hit_) {
        detected_.baud = host_baud_;
        finishMaintenance(MaintenanceResult::kOk);
    } else if (detail::reached(nowMs, deadline_ms_)) {
        restoreMaintenanceBaud(MaintenanceResult::kTimeout);
    }
    return {};
}

void Controller::onMaintenanceFrame(const demux::Decoder &dec,
                                    std::span<const uint8_t> frame) noexcept
{
    if (maintenance_phase_ == MaintenancePhase::kAwaitAck ||
        maintenance_phase_ == MaintenancePhase::kSwitch) {
        ControllerImpl::markReply(*this, receiver().classifyReply(dec, frame, step_.cmd.view()));
    } else if (maintenance_phase_ == MaintenancePhase::kAwaitProbe) {
        ProbeResult result{};
        probe_hit_ = probe_hit_ || (receiver().matchProbe(dec, frame, result) &&
                                    result.model == detected_.probe.model);
    }
}

} // namespace gnss
