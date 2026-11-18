// GNSS 上层工作模式状态机实现：kRover/kBaseConfig/kSurveyWait/kSurvey/kBaseTx/kFallback。
// 线程模型：仅 kIo 任务单线程调用，非 ISR；不阻塞、不分配堆，日志只经过 FW_LOGI/W。
#include "modules/gnss/GnssWorkMode.hpp"

#include "fw/Log.hpp"

namespace app {
namespace {

constexpr uint32_t kMsPerSec = 1000U;

} // namespace

void GnssWorkMode::reset() noexcept
{
    mode_ = WorkModeKind::kRover;
    baseType_ = BaseType::kSurvey;
    state_ = WorkModeState::kRover;
    lastErr_ = WorkModeErr::kNone;
    pending_ = false;
    pendingMode_ = WorkModeKind::kRover;
    pendingSave_ = false;
    saveOnSuccess_ = false;
    bootApplied_ = false;
    waitingCtrlAck_ = false;
    expectedSeq_ = 0U;
    fallbackPending_ = false;
    reqStartMs_ = 0U;
    surveyStartMs_ = 0U;
    rtcm1005Armed_ = false;
    seenRtcm1005_ = false;
    elapsedS_ = 0U;
    hAccCm_ = ::gnss::kU16Unknown;
    ++seq_;
}

void GnssWorkMode::onControllerReprobe() noexcept
{
    waitingCtrlAck_ = false;
    fallbackPending_ = false;
    rtcm1005Armed_ = false;
    seenRtcm1005_ = false;
    if (state_ != WorkModeState::kRover) {
        state_ = WorkModeState::kRover;
        ++seq_;
    }
    bootApplied_ = false;
}

void GnssWorkMode::requestMode(WorkModeKind mode, bool saveOnSuccess) noexcept
{
    if (mode == WorkModeKind::kSmartBase) {
        return;
    }
    pending_ = true;
    pendingMode_ = mode;
    pendingSave_ = saveOnSuccess;
    // 若用户已在等待态下显式下发了新模式，启动自动模式不再覆盖它。
    bootApplied_ = true;
}

void GnssWorkMode::onRtcmFrame(uint16_t msgId) noexcept
{
    constexpr uint16_t kRtcmStationArp = 1005U;
    if (rtcm1005Armed_ && (msgId == kRtcmStationArp)) {
        seenRtcm1005_ = true;
    }
}

WorkModeErr GnssWorkMode::mapMaintErr(::gnss::MaintenanceResult res) noexcept
{
    switch (res) {
    case ::gnss::MaintenanceResult::kUnsupported:
        return WorkModeErr::kUnsupported;
    case ::gnss::MaintenanceResult::kNack:
        return WorkModeErr::kNack;
    case ::gnss::MaintenanceResult::kTimeout:
    case ::gnss::MaintenanceResult::kIoError:
    default:
        return WorkModeErr::kTimeout;
    }
}

::gnss::ModeRequest GnssWorkMode::makeReq(::gnss::ModeAction action, const GnssBaseConfig &cfg,
                                          uint8_t port, uint8_t navRateHz,
                                          bool useConfiguredPort) noexcept
{
    ::gnss::ModeRequest req{};
    req.action = action;
    req.port = port;
    req.navRateHz = navRateHz;
    req.useConfiguredPort = useConfiguredPort;
    req.surveySeconds = cfg.svyS;
    req.surveyAccCm = cfg.svyAccCm;
    req.fixLatE9 = cfg.fixLatE9;
    req.fixLonE9 = cfg.fixLonE9;
    req.fixHeightMm = cfg.fixHeightMm;
    req.rtcmPeriodMs = cfg.toPeriodsMs();
    return req;
}

void GnssWorkMode::startCtrlStep(::gnss::Controller &ctrl, ::gnss::ModeAction action,
                                 const GnssBaseConfig &cfg, uint8_t port, uint8_t navRateHz,
                                 bool useConfiguredPort) noexcept
{
    const uint32_t nextSeq = ctrl.stats().maintenanceSeq + 1U;
    const auto req = makeReq(action, cfg, port, navRateHz, useConfiguredPort);
    if (!ctrl.requestMode(req)) {
        return;
    }
    inflightAction_ = action;
    expectedSeq_ = nextSeq;
    waitingCtrlAck_ = true;
}

void GnssWorkMode::beginRequest(::gnss::Controller &ctrl, WorkModeKind target, bool saveOnSuccess,
                                const GnssBaseConfig &cfg, uint8_t port, uint8_t navRateHz,
                                bool useConfiguredPort, uint32_t nowMs) noexcept
{
    pending_ = false;
    mode_ = target;
    baseType_ = cfg.baseType;
    saveOnSuccess_ = saveOnSuccess;
    lastErr_ = WorkModeErr::kNone;
    reqStartMs_ = nowMs;
    surveyStartMs_ = nowMs;
    elapsedS_ = 0U;
    rtcm1005Armed_ = false;
    seenRtcm1005_ = false;
    fallbackPending_ = false;

    if (target == WorkModeKind::kRover) {
        FW_LOGI(fw::TraceCat::kGnss, "<mode> Switching to rover.");
        state_ = WorkModeState::kRover;
        startCtrlStep(ctrl, ::gnss::ModeAction::kRover, cfg, port, navRateHz, useConfiguredPort);
        ++seq_;
        return;
    }

    state_ = WorkModeState::kBaseConfig;
    const auto action = (cfg.baseType == BaseType::kFixed) ? ::gnss::ModeAction::kBaseFixed
                                                           : ::gnss::ModeAction::kBasePrep;
    FW_LOGI(fw::TraceCat::kGnss, "<mode> Switching to base, %s.", baseTypeName(cfg.baseType));
    startCtrlStep(ctrl, action, cfg, port, navRateHz, useConfiguredPort);
    ++seq_;
}

void GnssWorkMode::enterFallback(::gnss::Controller &ctrl, WorkModeErr err,
                                 const GnssBaseConfig &cfg, uint8_t port, uint8_t navRateHz,
                                 bool useConfiguredPort) noexcept
{
    lastErr_ = err;
    saveOnSuccess_ = false;
    rtcm1005Armed_ = false;
    seenRtcm1005_ = false;
    mode_ = WorkModeKind::kRover;
    state_ = WorkModeState::kFallback;
    ++seq_;
    if (err == WorkModeErr::kSurveyTimeout) {
        FW_LOGW(fw::TraceCat::kGnss, "<mode> Survey-in timed out after %u s, back to rover.",
                static_cast<unsigned>(cfg.svyToS));
    } else {
        FW_LOGW(fw::TraceCat::kGnss, "<mode> Base mode failed, %s, back to rover.",
                workModeErrName(err));
    }
    fallbackPending_ = true;
    startCtrlStep(ctrl, ::gnss::ModeAction::kRover, cfg, port, navRateHz, useConfiguredPort);
}

void GnssWorkMode::handleCtrlStepOk(uint32_t nowMs, const GnssBaseConfig &cfg,
                                    bool &outNeedSave) noexcept
{
    switch (inflightAction_) {
    case ::gnss::ModeAction::kRover:
        mode_ = WorkModeKind::kRover;
        state_ = WorkModeState::kRover;
        lastErr_ = WorkModeErr::kNone;
        ++seq_;
        FW_LOGI(fw::TraceCat::kGnss, "<mode> Rover mode active.");
        if (saveOnSuccess_) {
            outNeedSave = true;
            saveOnSuccess_ = false;
        }
        break;
    case ::gnss::ModeAction::kBasePrep:
        state_ = WorkModeState::kSurveyWait;
        ++seq_;
        FW_LOGI(fw::TraceCat::kGnss,
                "<mode> Waiting for horizontal accuracy <= %u cm before survey-in.",
                static_cast<unsigned>(cfg.svyAccCm));
        break;
    case ::gnss::ModeAction::kBaseSurvey:
        surveyStartMs_ = nowMs;
        elapsedS_ = 0U;
        rtcm1005Armed_ = true;
        seenRtcm1005_ = false;
        FW_LOGI(fw::TraceCat::kGnss, "<mode> Survey-in started, %u s, accuracy %u cm.",
                static_cast<unsigned>(cfg.svyS), static_cast<unsigned>(cfg.svyAccCm));
        break;
    case ::gnss::ModeAction::kBaseFixed:
        mode_ = WorkModeKind::kBase;
        state_ = WorkModeState::kBaseTx;
        lastErr_ = WorkModeErr::kNone;
        ++seq_;
        FW_LOGI(fw::TraceCat::kGnss, "<mode> Base transmitting RTCM, fixed position.");
        if (saveOnSuccess_) {
            outNeedSave = true;
            saveOnSuccess_ = false;
        }
        break;
    default:
        break;
    }
}

void GnssWorkMode::onCtrlStepDone(::gnss::Controller &ctrl, ::gnss::MaintenanceResult res,
                                  const GnssBaseConfig &cfg, uint8_t port, uint8_t navRateHz,
                                  bool useConfiguredPort, uint32_t nowMs,
                                  bool &outNeedSave) noexcept
{
    waitingCtrlAck_ = false;
    if (fallbackPending_) {
        fallbackPending_ = false;
        state_ = WorkModeState::kRover;
        mode_ = WorkModeKind::kRover;
        ++seq_;
        return;
    }
    if (res == ::gnss::MaintenanceResult::kOk) {
        handleCtrlStepOk(nowMs, cfg, outNeedSave);
        return;
    }
    const WorkModeErr err = mapMaintErr(res);
    if (inflightAction_ == ::gnss::ModeAction::kRover) {
        lastErr_ = err;
        state_ = WorkModeState::kRover;
        ++seq_;
        FW_LOGW(fw::TraceCat::kGnss, "<mode> Rover command failed, %s.", workModeErrName(err));
        return;
    }
    if (err == WorkModeErr::kUnsupported) {
        // 接收机不支持基站模式时无需再下发 rover 命令，直接记录 W 日志回到 kRover。
        lastErr_ = err;
        saveOnSuccess_ = false;
        mode_ = WorkModeKind::kRover;
        state_ = WorkModeState::kRover;
        ++seq_;
        FW_LOGW(fw::TraceCat::kGnss, "<mode> Base mode failed, %s, back to rover.",
                workModeErrName(err));
        return;
    }
    enterFallback(ctrl, err, cfg, port, navRateHz, useConfiguredPort);
}

void GnssWorkMode::stepSurvey(::gnss::Controller &ctrl, const GnssBaseConfig &cfg, uint8_t port,
                              uint8_t navRateHz, bool useConfiguredPort, uint16_t hAccCm,
                              uint32_t nowMs, bool &outNeedSave) noexcept
{
    const uint32_t totalElapsedMs = nowMs - reqStartMs_;
    const uint32_t timeoutMs = static_cast<uint32_t>(cfg.svyToS) * kMsPerSec;
    if (totalElapsedMs >= timeoutMs) {
        enterFallback(ctrl, WorkModeErr::kSurveyTimeout, cfg, port, navRateHz, useConfiguredPort);
        return;
    }

    if (state_ == WorkModeState::kSurveyWait) {
        elapsedS_ = static_cast<uint16_t>(totalElapsedMs / kMsPerSec);
        if ((hAccCm != ::gnss::kU16Unknown) && (hAccCm <= cfg.svyAccCm)) {
            state_ = WorkModeState::kSurvey;
            surveyStartMs_ = nowMs;
            elapsedS_ = 0U;
            ++seq_;
            startCtrlStep(ctrl, ::gnss::ModeAction::kBaseSurvey, cfg, port, navRateHz,
                          useConfiguredPort);
        }
        return;
    }

    if (state_ == WorkModeState::kSurvey) {
        const uint32_t surveyMs = nowMs - surveyStartMs_;
        elapsedS_ = static_cast<uint16_t>(surveyMs / kMsPerSec);
        const uint32_t targetMs = static_cast<uint32_t>(cfg.svyS) * kMsPerSec;
        if (!waitingCtrlAck_ && (surveyMs >= targetMs) && seenRtcm1005_) {
            mode_ = WorkModeKind::kBase;
            state_ = WorkModeState::kBaseTx;
            rtcm1005Armed_ = false;
            lastErr_ = WorkModeErr::kNone;
            ++seq_;
            FW_LOGI(fw::TraceCat::kGnss, "<mode> Base transmitting RTCM, survey %u s.",
                    static_cast<unsigned>(cfg.svyS));
            if (saveOnSuccess_) {
                outNeedSave = true;
                saveOnSuccess_ = false;
            }
        }
    }
}

/**
 * @brief 周期驱动工作模式迁移，必要时通过 ctrl.requestMode 下发驱动命令。
 * @param ctrl: 底层 GNSS 控制器。
 * @param cfg: 当前 gnss.base 配置。
 * @param port: ESP32 所接模块串口号。
 * @param navRateHz: 当前导航输出频率（Hz）。
 * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
 * @param hAccCm: 最新 GST 水平精度估计（cm；kU16Unknown 表示未知）。
 * @param nowMs: 当前单调时间（ms）。
 * @param outNeedSave: 输出 true 表示需要请求接收机 SAVECONFIG/CFG-CFG 持久化。
 * @retval true: 状态快照较上次发布有实质变化（需刷新 DataCenter::gnssWorkMode）。
 * @retval false: 无对外可见变化。
 */
bool GnssWorkMode::poll(::gnss::Controller &ctrl, const GnssBaseConfig &cfg, uint8_t port,
                        uint8_t navRateHz, bool useConfiguredPort, uint16_t hAccCm, uint32_t nowMs,
                        bool &outNeedSave) noexcept
{
    outNeedSave = false;
    hAccCm_ = hAccCm;
    baseType_ = cfg.baseType;

    // 首次进入 kRunning 且无挂起请求时，按 NVS gnss/mode 自动切到 base（rover 由 roverStep 已完成）。
    if (!bootApplied_ && (ctrl.state() == ::gnss::State::kRunning)) {
        bootApplied_ = true;
        if (!pending_ && (cfg.mode == WorkModeKind::kBase)) {
            beginRequest(ctrl, WorkModeKind::kBase, false, cfg, port, navRateHz, useConfiguredPort,
                         nowMs);
        }
    }

    if (waitingCtrlAck_ && (ctrl.stats().maintenanceSeq == expectedSeq_)) {
        onCtrlStepDone(ctrl, ctrl.stats().maintenanceResult, cfg, port, navRateHz,
                       useConfiguredPort, nowMs, outNeedSave);
    }

    // 回退恢复（发送 MODE ROVER）期间若仍在等底层空闲，一旦空闲即发送。
    if (fallbackPending_ && !waitingCtrlAck_ && (ctrl.state() == ::gnss::State::kRunning)) {
        startCtrlStep(ctrl, ::gnss::ModeAction::kRover, cfg, port, navRateHz, useConfiguredPort);
        if (waitingCtrlAck_ && (ctrl.stats().maintenanceSeq == expectedSeq_)) {
            onCtrlStepDone(ctrl, ctrl.stats().maintenanceResult, cfg, port, navRateHz,
                           useConfiguredPort, nowMs, outNeedSave);
        }
    }

    // 收到新的模式请求且当前无底层事务阻塞时立即下发；若正在自测/回退中，则中断并切换。
    if (pending_ && !waitingCtrlAck_ && (ctrl.state() == ::gnss::State::kRunning)) {
        beginRequest(ctrl, pendingMode_, pendingSave_, cfg, port, navRateHz, useConfiguredPort,
                     nowMs);
        if (waitingCtrlAck_ && (ctrl.stats().maintenanceSeq == expectedSeq_)) {
            onCtrlStepDone(ctrl, ctrl.stats().maintenanceResult, cfg, port, navRateHz,
                           useConfiguredPort, nowMs, outNeedSave);
        }
    } else if (!fallbackPending_ &&
               ((state_ == WorkModeState::kSurveyWait) || (state_ == WorkModeState::kSurvey))) {
        stepSurvey(ctrl, cfg, port, navRateHz, useConfiguredPort, hAccCm, nowMs, outNeedSave);
    }

    return finalizeSnapshot(cfg, hAccCm);
}

bool GnssWorkMode::finalizeSnapshot(const GnssBaseConfig &cfg, uint16_t hAccCm) noexcept
{
    const GnssWorkModeStatus cur = status();
    baseType_ = cfg.baseType;
    hAccCm_ = hAccCm;
    if (!hasPublished_) {
        hasPublished_ = true;
        lastPublished_ = cur;
        return true;
    }
    const bool changed =
        (cur.seq != lastPublished_.seq) || (cur.state != lastPublished_.state) ||
        (cur.mode != lastPublished_.mode) || (cur.elapsedS != lastPublished_.elapsedS) ||
        (cur.hAccCm != lastPublished_.hAccCm) || (cur.lastErr != lastPublished_.lastErr);
    if (changed) {
        lastPublished_ = cur;
    }
    return changed;
}

GnssWorkModeStatus GnssWorkMode::status() const noexcept
{
    GnssWorkModeStatus s{};
    s.mode = mode_;
    s.baseType = baseType_;
    s.state = state_;
    s.lastErr = lastErr_;
    s.elapsedS = elapsedS_;
    s.hAccCm = hAccCm_;
    s.seq = seq_;
    s.active = (state_ != WorkModeState::kRover);
    return s;
}

} // namespace app
