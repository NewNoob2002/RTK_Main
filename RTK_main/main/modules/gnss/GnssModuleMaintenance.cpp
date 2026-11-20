// GNSS 维护操作：按下发或确认结果更新 NVS，并合并自动保存。
// 仅 kIo 调用。写 NVS 可阻塞，非 ISR。
#include "modules/gnss/GnssModule.hpp"

#include <bit>
#include <span>

extern "C" {
#include "nvs.h"
}
#include "AppContext.hpp"
#include "fw/Log.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/gnss/GnssModulePrivate.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "modules/storage/StoreWriteGate.hpp"

namespace app {
namespace {

using ::gnss::Maintenance;
using ::gnss::MaintenanceResult;

/**
 * @brief 返回维护结果的诊断短句。
 * @param result: 维护结果。
 * @retval 静态文本。
 */
const char *saveFailureName(MaintenanceResult result) noexcept
{
    switch (result) {
    case MaintenanceResult::kNack:
        return "receiver rejected command";
    case MaintenanceResult::kTimeout:
        return "timeout";
    case MaintenanceResult::kUnsupported:
        return "not supported";
    case MaintenanceResult::kIoError:
        return "io error";
    default:
        return "unknown";
    }
}

/**
 * @brief 记录启动切速后模块保存的结果（失败不计入 baud_fail、不回退波特率）。
 * @param baud: 已切换的目标波特率。
 * @param result: 接收机保存结果。
 * @note 仅 kIo 调用，不阻塞。
 */
void logBootBaudSaveResult(uint32_t baud, MaintenanceResult result) noexcept
{
    if (result == MaintenanceResult::kOk) {
        FW_LOGI(fw::TraceCat::kGnss, "<cfg> Startup baud %lu saved to receiver",
                static_cast<unsigned long>(baud));
    } else if (result != MaintenanceResult::kUnsupported) {
        traceGnss(GnssTrace::kStepNack, 0U, static_cast<uint32_t>(result));
        FW_LOGW(fw::TraceCat::kGnss, "<cfg> Receiver save failed (%s)", saveFailureName(result));
    }
}

/**
 * @brief 删除 GNSS 探测缓存与用户波特率覆盖键。
 * @retval 存储结果，键缺失视为成功。
 */
fw::Result clearCacheAndUserBaud() noexcept
{
    const store::WriteGuard guard;
    if (!guard.active()) {
        return fw::Result::kBusy;
    }
    nvs_handle_t handle = 0U;
    esp_err_t error = nvs_open("gnss", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return fw::Result::kIoError;
    }
    for (const auto &key : gnss_store::kCacheKeys) {
        // 目录键直接引用 NUL 结尾字面量。
        const esp_err_t erased = nvs_erase_key(
            handle, key.key.data()); // NOLINT(bugprone-suspicious-stringview-data-usage)
        if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) {
            error = erased;
            break;
        }
    }
    if (error == ESP_OK) {
        // 擦除用户波特率覆盖，确保下次启动回到 RTK_GNSS_TARGET_BAUD。
        const auto &baudKey = gnss_store::kConfigKeys.back();
        const esp_err_t erased = nvs_erase_key(
            handle, baudKey.key.data()); // NOLINT(bugprone-suspicious-stringview-data-usage)
        if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) {
            error = erased;
        }
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error == ESP_OK ? fw::Result::kOk : fw::Result::kIoError;
}

} // namespace

void GnssModule::cmdMaintenance(const fw::Command &cmd, uint32_t nowMs,
                                fw::CommandResult &result) noexcept
{
    const auto verb = static_cast<GnssMaintenanceVerb>(cmd.verb);
    if (verb == GnssMaintenanceVerb::kBaud) {
        cmdBaud(cmd, nowMs, result);
        return;
    }
    if (pending_apply_ != PendingApply::kNone || baud_deferred_ ||
        controller().state() != ::gnss::State::kRunning) {
        result.status = fw::Result::kBusy;
        return;
    }
    if (cmd.argLen != 0U) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const bool reset =
        static_cast<GnssMaintenanceVerb>(cmd.verb) == GnssMaintenanceVerb::kFactoryReset;
    const uint32_t seq = controller().stats().maintenanceSeq + 1U;
    const bool accepted = reset ? controller().requestFactoryReset() : controller().requestSave();
    if (!accepted) {
        result.status = fw::Result::kBusy;
        return;
    }
    active_maintenance_ = reset ? Maintenance::kFactoryReset : Maintenance::kSave;
    result.detail = std::bit_cast<int32_t>(seq);
    result.status = fw::Result::kOk;
    pollController(nowMs);
}

void GnssModule::cmdBaud(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept
{
    constexpr size_t kBaudBytes = sizeof(uint32_t);
    const std::span<const uint8_t> args{cmd.args};
    if (cmd.argLen != kBaudBytes) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const uint32_t baud = detail::readU32(args.first(kBaudBytes));
    if (!gnss_store::validUserBaud(baud)) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const auto state = controller().state();
    const bool defer =
        state == ::gnss::State::kMaintenance && active_maintenance_ == Maintenance::kSave;
    if (pending_apply_ != PendingApply::kNone || baud_deferred_ ||
        (state != ::gnss::State::kRunning && !defer)) {
        result.status = fw::Result::kBusy;
        return;
    }
    const uint32_t seq = controller().stats().maintenanceSeq + (defer ? 2U : 1U);
    if (!defer && !controller().requestBaud(baud)) {
        result.status = fw::Result::kBusy;
        return;
    }
    requested_baud_ = baud;
    baud_deferred_ = defer;
    result.detail = std::bit_cast<int32_t>(seq);
    result.status = fw::Result::kOk;
    if (!defer) {
        active_maintenance_ = Maintenance::kBaud;
        pollController(nowMs);
    }
}

fw::Result GnssModule::resetStoredSettings() noexcept
{
    auto &cfg = ensureRoverCfg();
    cfg.rateHz = static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ);
    cfg.gsv = true;
    preferred_baud_ = 0U;
    out_cfg_ = data::GnssOutConfig::defaults();
    ctx().data.gnssOutCfg.write(out_cfg_);
    gga_gate_cfg_ = ::gnss::GgaGateConfig::defaults();
    gga_gate_enabled_loaded_ = true;
    gga_gate_min_sats_loaded_ = true;
    gga_gate_save_pending_ = false;
    gga_gate_save_retry_ms_ = 0U;
    if (gga_gate_.has_value()) {
        gga_gate_->reset();
        static_cast<void>(gga_gate_->setConfig(gga_gate_cfg_));
    }
    publishGgaGateStatus();
    save_queue_.clear();
    hint_ = {};
    skip_hint_save_ = true;
    initBaudCandidates();
    // Controller 持有接收机出厂速率提示，NVS 缓存仍清空。
    controller().setOutputPlan(makePlan());
    parser().reset();
    ctx().data.gnssFix.write(::gnss::Fix{});
    ctx().data.gnssSats.write(::gnss::SatTable{});
    writeBaseConfig(GnssBaseConfig::defaults());
    if (work_mode_.has_value()) {
        auto &wm = *work_mode_;
        wm.reset();
        ctx().data.gnssWorkMode.write(wm.status());
    }
    // 仅保存 rate/gsv/out 缺省；gnss/baud 作为用户覆盖键一并擦除。
    const auto saved = ctx().storage.saveKeys(
        std::span{gnss_store::kConfigKeys}.first(gnss_store::kConfigKeys.size() - 1U));
    const auto savedBase = ctx().storage.saveKeys(gnss_store::kBaseKeys);
    const auto cleared = clearCacheAndUserBaud();
    if (!fw::isOk(saved)) {
        return saved;
    }
    return fw::isOk(savedBase) ? cleared : savedBase;
}

bool GnssModule::persistBaudAndSave(uint32_t baud) noexcept
{
    if (store::writeValue(gnss_store::kConfigKeys.back(), baud) != ESP_OK) {
        return false;
    }
    preferred_baud_ = baud;
    initBaudCandidates();
    if (!fw::isOk(persistDetectedHint())) {
        return false;
    }
    controller().setProbePreferences(hint_, std::span{bauds_}.first(baud_count_));
    baud_save_in_flight_ = controller().requestSave();
    active_maintenance_ = Maintenance::kSave;
    return baud_save_in_flight_;
}

void GnssModule::setConsoleMaintenanceResult(uint32_t seq, Maintenance kind,
                                             MaintenanceResult result) noexcept
{
    console_maintenance_seq_ = seq;
    console_maintenance_ = kind;
    console_maintenance_result_ = result;
}

void GnssModule::syncBootHint() noexcept
{
    if (!ctx().bootConfig.safeDefaults && ctx().bootConfig.userNvsReady) {
        static_cast<void>(persistDetectedHint());
    } else {
        const auto &det = controller().detected();
        if (det.receiver != nullptr) {
            hint_.family = det.receiver->family();
            hint_.model = det.probe.model;
            hint_.baud = (det.baud != 0U) ? det.baud : host_baud_;
        }
    }
    controller().setProbePreferences(hint_, std::span{bauds_}.first(baud_count_));
}

void GnssModule::completeBootBaudMaintenance(uint32_t baud, MaintenanceResult result) noexcept
{
    boot_baud_in_flight_ = false;
    if (result == MaintenanceResult::kOk) {
        // 启动切速仅更新运行波特率与 gnss.cache（不写用户覆盖键 gnss/baud），随后请求一次模块 Flash 保存（G-baud-save）。
        syncBootHint();
        boot_baud_save_in_flight_ = controller().requestSave();
        if (boot_baud_save_in_flight_) {
            active_maintenance_ = Maintenance::kSave;
        }
        FW_LOGI(fw::TraceCat::kGnss, "<cfg> Startup baud switched to %lu",
                static_cast<unsigned long>(baud));
        return;
    }
    ++baud_fail_count_;
    ctx().data.gnssBaudFailCount.store(baud_fail_count_, std::memory_order_relaxed);
    detail::traceGnssSub(GnssSubTrace::kBaudFail, baud, static_cast<uint32_t>(result));
    FW_LOGW(fw::TraceCat::kGnss, "<cfg> Startup baud switch to %lu failed (%s)",
            static_cast<unsigned long>(baud), saveFailureName(result));
    // NACK/不支持留在 kRunning，此时补写实际速率到 gnss.cache；超时回退由重探结束时写回。
    if (controller().state() == ::gnss::State::kRunning) {
        syncBootHint();
    }
}

void GnssModule::completeBaudMaintenance(const ::gnss::ControllerStats &stats,
                                         uint32_t previousSeq) noexcept
{
    auto result = stats.maintenanceResult;
    if (baud_save_in_flight_) {
        baud_save_in_flight_ = false;
        if (result != MaintenanceResult::kOk) {
            FW_LOGW(fw::TraceCat::kGnss, "<cfg> baud applied, receiver save failed (%s)",
                    saveFailureName(result));
        }
        setConsoleMaintenanceResult(previousSeq, Maintenance::kBaud, MaintenanceResult::kOk);
        return;
    }

    const uint32_t baud = requested_baud_;
    if (boot_baud_in_flight_) {
        completeBootBaudMaintenance(baud, result);
        return;
    }

    if (result == MaintenanceResult::kOk) {
        if (persistBaudAndSave(baud)) {
            return;
        }
        result = MaintenanceResult::kIoError;
    }
    setConsoleMaintenanceResult(stats.maintenanceSeq, Maintenance::kBaud, result);
}

void GnssModule::completeMaintenance(uint32_t nowMs) noexcept
{
    const auto stats = controller().stats();
    const uint32_t previousSeq = completed_maintenance_seq_;
    completed_maintenance_seq_ = stats.maintenanceSeq;
    if (boot_baud_save_in_flight_) {
        boot_baud_save_in_flight_ = false;
        logBootBaudSaveResult(requested_baud_, stats.maintenanceResult);
    } else if (baud_save_in_flight_ || stats.maintenance == Maintenance::kBaud) {
        completeBaudMaintenance(stats, previousSeq);
    } else if (stats.maintenance == Maintenance::kMode) {
        // 工作模式步骤由 GnssWorkMode::poll() 直接读取 maintenanceSeq 消费。
    } else if (auto_save_in_flight_) {
        if (stats.maintenanceResult != MaintenanceResult::kOk) {
            FW_LOGW(fw::TraceCat::kGnss, "<cfg> Receiver save failed (%s)",
                    saveFailureName(stats.maintenanceResult));
        }
        auto_save_in_flight_ = false;
    } else {
        auto result = stats.maintenanceResult;
        if (stats.maintenance == Maintenance::kFactoryReset && result == MaintenanceResult::kOk &&
            !fw::isOk(resetStoredSettings())) {
            result = MaintenanceResult::kIoError;
        }
        setConsoleMaintenanceResult(stats.maintenanceSeq, stats.maintenance, result);
        if ((webMaintSeq_ != 0U) && (stats.maintenanceSeq == webMaintTarget_)) {
            const fw::Result webRes =
                (result == MaintenanceResult::kOk) ? fw::Result::kOk : fw::Result::kIoError;
            ctx().data.webGnssOutcome.write(
                {webMaintSeq_, data::WebGnssPhase::kDone, webRes, static_cast<int32_t>(result)});
            webMaintSeq_ = 0U;
            webMaintTarget_ = 0U;
        }
    }
    publishStatus(nowMs);
}

uint32_t GnssModule::targetBaud() const noexcept
{
    return (preferred_baud_ != 0U) ? preferred_baud_ : kGnssTargetBaud;
}

void GnssModule::alignBootBaud(uint32_t nowMs) noexcept
{
    boot_baud_pending_ = false;
    const uint32_t target = targetBaud();
    if (controller().detected().baud == target) {
        syncBootHint();
        return;
    }
    requested_baud_ = target;
    boot_baud_in_flight_ = controller().requestBaud(target);
    if (boot_baud_in_flight_) {
        active_maintenance_ = Maintenance::kBaud;
        pollController(nowMs);
    } else {
        syncBootHint();
    }
}

void GnssModule::tickMaintenance(uint32_t nowMs) noexcept
{
    if (controller().stats().maintenanceSeq != completed_maintenance_seq_) {
        return;
    }
    if (boot_baud_pending_ && controller().state() == ::gnss::State::kRunning) {
        alignBootBaud(nowMs);
        return;
    }
    if (baud_deferred_ && controller().state() == ::gnss::State::kRunning) {
        if (controller().requestBaud(requested_baud_)) {
            baud_deferred_ = false;
            active_maintenance_ = Maintenance::kBaud;
        }
        return;
    }
    if (save_queue_.take(controller().state() == ::gnss::State::kRunning &&
                         pending_apply_ == PendingApply::kNone)) {
        auto_save_in_flight_ = controller().requestSave();
        active_maintenance_ = Maintenance::kSave;
    }
}

void GnssModule::beginPendingApply(uint32_t nowMs) noexcept
{
    apply_deferred_ = false;
    pending_seq_ = controller().stats().outputSeq + 1U;
    pending_deadline_ms_ = nowMs + detail::kApplyTimeoutMs;
    controller().setOutputPlan(makePlan());
    if (pending_apply_ == PendingApply::kRate) {
        controller().requestReconfigure(nowMs);
    } else {
        static_cast<void>(controller().requestOutput(controller().outputPlan(), nowMs));
    }
    pollController(nowMs);
    publishStatus(nowMs);
}

} // namespace app
