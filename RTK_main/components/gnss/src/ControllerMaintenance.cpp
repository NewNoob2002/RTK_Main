// GNSS 控制器的一次性维护操作与目标串口验证。
// 仅控制器所属任务调用。无阻塞、无分配，只生成 UART 动作。
#include "ControllerImpl.hpp"

#include "UbloxExt.hpp"

namespace gnss {
namespace {

constexpr uint32_t kBaudSettleMs = 100U;
constexpr uint32_t kMaintenanceProbeMs = 700U;
constexpr uint8_t kModePhaseResyncRetry = 1U;

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

bool Controller::requestMode(const ModeRequest &req) noexcept
{
    if ((state_ != State::kRunning) || (detected_.receiver == nullptr) || (cfg_ == nullptr)) {
        return false;
    }
    pending_maintenance_ = Maintenance::kMode;
    mode_request_ = req;
    mode_step_index_ = 0U;
    mode_step_tries_ = 0U;
    step_ = ConfigStep{};
    if (!receiver().modeStep(mode_request_, 0U, step_)) {
        finishMaintenance(MaintenanceResult::kUnsupported);
        return true;
    }
    maintenance_old_baud_ = host_baud_;
    maintenance_phase_ = MaintenancePhase::kSend;
    phase_ = 0U;
    reply_ok_ = false;
    reply_nack_ = false;
    probe_hit_ = false;
    state_ = State::kMaintenance;
    return true;
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
    phase_ = 0U;
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
    phase_ = 0U;
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
        return ControllerImpl::pollMaintenanceAwaitAck(*this, nowMs);
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
    if ((maintenance_phase_ == MaintenancePhase::kAwaitAck && phase_ == 0U) ||
        maintenance_phase_ == MaintenancePhase::kSwitch) {
        ControllerImpl::markReply(*this, receiver().classifyReply(dec, frame, step_.cmd.view()));
    } else if (maintenance_phase_ == MaintenancePhase::kAwaitProbe) {
        ProbeResult result{};
        probe_hit_ = probe_hit_ || (receiver().matchProbe(dec, frame, result) &&
                                    result.model == detected_.probe.model);
    }
}

Action ControllerImpl::pollMaintenanceAwaitAck(Controller &self, uint32_t nowMs) noexcept
{
    if ((self.pending_maintenance_ == Maintenance::kMode) &&
        (self.phase_ == kModePhaseResyncRetry)) {
        if (detail::reached(nowMs, self.deadline_ms_)) {
            self.phase_ = 0U;
            self.maintenance_phase_ = Controller::MaintenancePhase::kSend;
            self.reply_ok_ = false;
            self.reply_nack_ = false;
            return self.pollMaintenance(nowMs);
        }
        return {};
    }
    if (self.reply_nack_ || self.reply_ok_ || detail::reached(nowMs, self.deadline_ms_)) {
        if (self.pending_maintenance_ == Maintenance::kMode) {
            return handleModeAck(self, nowMs);
        }
        self.finishMaintenance(self.reply_nack_ ? MaintenanceResult::kNack
                                                : (self.reply_ok_ ? MaintenanceResult::kOk
                                                                  : MaintenanceResult::kTimeout));
    }
    return {};
}

/**
 * @brief 处理工作模式多步维护命令的单步应答与重试。
 * @param self: 控制器。
 * @param nowMs: 当前时间。
 * @retval action: 若进入下一步或重试则返回待发送动作，否则为空。
 */
Action ControllerImpl::handleModeAck(Controller &self, uint32_t nowMs) noexcept
{
    const bool ok = self.reply_ok_ && !self.reply_nack_;
    if (!ok) {
        if (self.reply_nack_) {
            ++self.stats_.stepNacks;
        } else {
            ++self.stats_.stepTimeouts;
        }
        if (self.mode_step_tries_ < self.timing_.stepRetries) {
            ++self.mode_step_tries_;
            self.reply_ok_ = false;
            self.reply_nack_ = false;
            if (ublox::isValsetFrame(self.step_.cmd.view())) {
                ControllerImpl::armDeadline(self, nowMs, ublox::kValsetResyncWindowMs);
                self.phase_ = kModePhaseResyncRetry;
                return {};
            }
            self.maintenance_phase_ = Controller::MaintenancePhase::kSend;
            return self.pollMaintenance(nowMs);
        }
        self.finishMaintenance(self.reply_nack_ ? MaintenanceResult::kNack
                                                : MaintenanceResult::kTimeout);
        return {};
    }
    ++self.mode_step_index_;
    self.mode_step_tries_ = 0U;
    self.step_ = ConfigStep{};
    if (self.receiver().modeStep(self.mode_request_, self.mode_step_index_, self.step_)) {
        self.maintenance_phase_ = Controller::MaintenancePhase::kSend;
        self.reply_ok_ = false;
        self.reply_nack_ = false;
        return self.pollMaintenance(nowMs);
    }
    self.finishMaintenance(MaintenanceResult::kOk);
    return {};
}

} // namespace gnss
