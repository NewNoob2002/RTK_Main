// GNSS 模块命令侧：应用层指令解析（探测/复位/频率/输出/透传）与异步执行（ADR-0010 §2.7）。
// 线程：init() 启动前单线程；start()/wait()/poll()/onCommand 仅 kIo 任务；keys/load/save 由 storage 与 kIo 调用。
// 均非 ISR；不分配堆。实时路径只用 GnssTrace；文本诊断经 FW_LOG*（ADR-0009 §2.12）：状态变化 I，逐帧/逐句 D。

#include "modules/gnss/GnssModule.hpp"
#include "modules/gnss/GnssModulePrivate.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "modules/storage/StoreWriteGate.hpp"
#include "board/BoardProfile.hpp"
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "fw/Status.hpp"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "gnss/Receiver.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/gnss/GnssConsoleValue.hpp"
#include "modules/storage/StorageModule.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "rtos/EspStatus.hpp"

namespace app {

using detail::isValidRateHz;
using detail::traceGnssSub;

namespace {

/**
 * @brief 判断控制器是否处于配置/输出/读回瞬态（此时不可直写 NVS，须返回 kBusy）。
 * @param state: 控制器当前状态。
 * @retval true 处于瞬态。
 */
[[nodiscard]] bool isTransientApplyState(::gnss::State state) noexcept
{
    return (state == ::gnss::State::kConfiguring) || (state == ::gnss::State::kOutput) ||
           (state == ::gnss::State::kReadback);
}

} // namespace

/**
 * @brief 分派常规 GNSS 命令与扩展子命令（已在 kRunning 或允许状态下）。
 * @param cmd: 待执行指令。
 * @param nowMs: 当前时间。
 * @param result: 命令执行结果。
 */
void GnssModule::dispatchStandardCmd(const fw::Command &cmd, uint32_t nowMs,
                                     fw::CommandResult &result) noexcept
{
    switch (static_cast<GnssVerb>(cmd.verb)) {
    case GnssVerb::kProbe:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdProbe(nowMs, result);
        return;
    case GnssVerb::kReset:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdReset(nowMs, result);
        return;
    case GnssVerb::kSetRate:
        cmdSetRate(cmd, nowMs, result);
        return;
    case GnssVerb::kSend:
        sendRaw(cmd, result);
        return;
    default:
        break;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetOut) {
        cmdSetOut(cmd, nowMs, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kReadback) {
        startReadback(nowMs, result);
        return;
    }
    result.status = fw::Result::kNotSupported;
}

// 三组命令字共用 cmd.verb 一个字节，onCommand 先按维护命令分流；编号重叠会把别的命令当成维护命令。
static_assert(static_cast<uint8_t>(GnssSubVerb::kMode) >
                  static_cast<uint8_t>(GnssMaintenanceVerb::kBaud),
              "GnssSubVerb::kMode must not overlap GnssMaintenanceVerb");
static_assert(static_cast<uint8_t>(GnssSubVerb::kSetBase) >
                  static_cast<uint8_t>(GnssMaintenanceVerb::kBaud),
              "GnssSubVerb::kSetBase must not overlap GnssMaintenanceVerb");
static_assert(static_cast<uint8_t>(GnssSubVerb::kSetBase) >
                  static_cast<uint8_t>(GnssSubVerb::kMode),
              "GnssSubVerb::kSetBase must follow GnssSubVerb::kMode");
static_assert(static_cast<uint8_t>(GnssSubVerb::kReadback) <
                  static_cast<uint8_t>(GnssMaintenanceVerb::kSave),
              "GnssSubVerb::kReadback must not overlap GnssMaintenanceVerb");

/** @brief 派发 kIo 命令，PPP/星座序列运行期间独占串口（允许 kProbe/kReset 打断）。 @param cmd: 请求。 @param[out] result: 受理结果。 */
void GnssModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!initialized_) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t now = rtos::nowMs();
    if (cmd.verb >= static_cast<uint8_t>(GnssSubVerb::kApplyRate) &&
        cmd.verb <= static_cast<uint8_t>(GnssSubVerb::kFlushGgaGate)) {
        cmdSetting(cmd, now, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetGgaGate) {
        cmdSetGgaGate(cmd, now, result);
        return;
    }
    const bool restart = cmd.verb == static_cast<uint8_t>(GnssVerb::kProbe) ||
                         cmd.verb == static_cast<uint8_t>(GnssVerb::kReset);
    if (restart && (ppp_seq_.active() || sat_seq_.active())) {
        dispatchStandardCmd(cmd, now, result);
        return;
    }
    if (ppp_seq_.active() || sat_seq_.active()) {
        result.status = fw::Result::kBusy;
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kApplyPpp) {
        cmdApplyPpp(cmd, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kApplySatTrack) {
        cmdApplySatTrack(cmd, result);
        return;
    }
    const auto maintenanceVerb = static_cast<GnssMaintenanceVerb>(cmd.verb);
    if (maintenanceVerb == GnssMaintenanceVerb::kSave ||
        maintenanceVerb == GnssMaintenanceVerb::kFactoryReset ||
        maintenanceVerb == GnssMaintenanceVerb::kBaud) {
        cmdMaintenance(cmd, now, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kMode) {
        cmdMode(cmd, now, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetBase) {
        cmdSetBase(cmd, result);
        return;
    }
    if (restart && (applyTicketSeq_ != 0U || maintenanceTicketSeq_ != 0U)) {
        dispatchStandardCmd(cmd, now, result);
        return;
    }
    if (baud_deferred_ || (controller().state() == ::gnss::State::kMaintenance &&
                           (active_maintenance_ != ::gnss::Maintenance::kSave ||
                            (cmd.verb != static_cast<uint8_t>(GnssVerb::kSetRate) &&
                             cmd.verb != static_cast<uint8_t>(GnssSubVerb::kSetOut))))) {
        result.status = fw::Result::kBusy;
        return;
    }
    dispatchStandardCmd(cmd, now, result);
}

void GnssModule::cmdSetting(const fw::Command &cmd, uint32_t nowMs,
                            fw::CommandResult &result) noexcept
{
    const auto verb = static_cast<GnssSubVerb>(cmd.verb);
    if (verb == GnssSubVerb::kFlushGgaGate) {
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        tickGgaGateSave(nowMs, true);
        publishPendingNvs();
        result.status = ctx().data.ggaGatePersist.load(std::memory_order_acquire) ==
                                data::GgaGatePersist::kClean
                            ? fw::Result::kOk
                            : fw::Result::kIoError;
        return;
    }
    if (verb == GnssSubVerb::kApplyGgaGate) {
        cmdApplyGgaGate(cmd, result);
        return;
    }
    if (ppp_seq_.active() || sat_seq_.active() || baud_deferred_ ||
        pending_apply_ != PendingApply::kNone) {
        result.status = fw::Result::kBusy;
        return;
    }
    const auto st = controller().state();
    if (verb == GnssSubVerb::kSaveReceiver) {
        if (st != ::gnss::State::kRunning) {
            result.status = fw::Result::kBusy;
            return;
        }
        cmdMaintenance(cmd, nowMs, result);
        return;
    }
    if ((st == ::gnss::State::kMaintenance) &&
        (active_maintenance_ != ::gnss::Maintenance::kSave)) {
        result.status = fw::Result::kBusy;
        return;
    }
    const auto model = controller().detected().probe.model;
    if (verb == GnssSubVerb::kApplyRate) {
        if (model != ::gnss::Model::kUnknown) {
            const auto rates = controller().receiver().navRatesHz(model);
            if (cmd.argLen != 1U || std::ranges::find(rates, cmd.args[0]) == rates.end()) {
                result.status = fw::Result::kInvalidArg;
                return;
            }
        }
        cmdSetRate(cmd, nowMs, result);
    } else {
        cmdPatchOutput(cmd, nowMs, result);
    }
    if (fw::isOk(result.status) && pending_apply_ != PendingApply::kNone) {
        applyTicketSeq_ = cmd.seq;
        result.detail = fw::kDetailDeferred;
    }
}

void GnssModule::cmdPatchOutput(const fw::Command &cmd, uint32_t nowMs,
                                fw::CommandResult &result) noexcept
{
    constexpr uint32_t kMsPerSecond = 1000U;
    const auto model = controller().detected().probe.model;
    if (cmd.argLen != 2U || cmd.args[0] >= data::kGnssAppMsgCount) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    // 已验证下标处于 OutputMsg 共用前缀，间隔按二补码还原。
    const auto msg = static_cast<::gnss::OutputMsg>(cmd.args[0]);
    const auto interval = std::bit_cast<int8_t>(cmd.args[1]);
    if (model != ::gnss::Model::kUnknown) {
        const auto periods =
            controller().receiver().outputPeriodsMs(outputCapabilityMsg(msg), model);
        const uint32_t period = core::services::GnssService::intervalToPeriodMs(interval);
        const uint8_t curRate = ensureRoverCfg().rateHz;
        const uint32_t fixPeriod = kMsPerSecond / std::max(curRate, uint8_t{1U});
        const bool gsvDisabled = msg == ::gnss::OutputMsg::kSatsInfo && !ensureRoverCfg().gsv;
        const bool invalidPeriod =
            (period != 0U) &&
            (gsvDisabled || ((model == ::gnss::Model::kZedF9p) ? !isF9pPeriodValid(period, curRate)
                                                               : (period < fixPeriod)));
        if (invalidPeriod || std::ranges::find(periods, period) == periods.end()) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
    }
    auto next = out_cfg_;
    result.status = next.applyInterval(cmd.args[0], interval);
    if (!fw::isOk(result.status)) {
        return;
    }
    fw::Command patched{};
    size_t encodedLen = 0U;
    if (!fw::isOk(next.encode(patched.args, encodedLen))) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    // encode 成功时固定写入 8 字节。
    patched.argLen = static_cast<uint8_t>(encodedLen);
    cmdSetOut(patched, nowMs, result);
}

void GnssModule::completeSetting(uint32_t &seq, fw::Result status) noexcept
{
    if (seq != 0U && ctx().ioExecutor != nullptr) {
        ctx().ioExecutor->complete(seq, {.seq = seq, .status = status});
    }
    seq = 0U;
}

void GnssModule::abortPendingSettings() noexcept
{
    if (pending_apply_ != PendingApply::kNone) {
        restorePendingApply();
        controller().setOutputPlan(makePlan());
    }
    completeSetting(applyTicketSeq_, fw::Result::kInvalidState);
    completeSetting(maintenanceTicketSeq_, fw::Result::kInvalidState);
    maintenanceTicketTarget_ = 0U;
    if (ppp_seq_.active()) {
        ppp_seq_ = {};
    }
    completeSetting(ppp_ticket_seq_, fw::Result::kInvalidState);
    if (sat_seq_.active()) {
        sat_seq_ = {};
    }
    sat_rollback_in_flight_ = false;
    sat_replay_forced_gps_ = false;
    completeSetting(sat_ticket_seq_, fw::Result::kInvalidState);
}

void GnssModule::cmdProbe(uint32_t nowMs, fw::CommandResult &result) noexcept
{
    abortPendingSettings();
    // 人工重探：清空最近定位，避免旧定位在重探期间被当成当前值发布。
    manual_reprobe_ = true;
    parser().reset();
    ctx().data.gnssFix.write(::gnss::Fix{});
    controller().requestReprobe(nowMs);
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

void GnssModule::cmdReset(uint32_t nowMs, fw::CommandResult &result) noexcept
{
    abortPendingSettings();
    // 人工复位：先清空定位再停/启控制器，重配流程由控制器自己推进。
    manual_reprobe_ = true;
    parser().reset();
    ctx().data.gnssFix.write(::gnss::Fix{});
    controller().stop(nowMs);
    pollController(nowMs);
    controller().start(nowMs);
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

void GnssModule::cmdSetRate(const fw::Command &cmd, uint32_t nowMs,
                            fw::CommandResult &result) noexcept
{
    if ((cmd.argLen != 1U) || !isValidRateHz(cmd.args[0])) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    // 上一次变更尚未判定时拒绝：否则 pending_prev_* 会被暂存值覆盖，回滚到的不是 NVS 中的旧值。
    if (pending_apply_ != PendingApply::kNone) {
        result.status = fw::Result::kBusy;
        return;
    }
    const auto st = controller().state();
    if (isTransientApplyState(st)) {
        result.status = fw::Result::kBusy;
        return;
    }
    if (st != ::gnss::State::kRunning && st != ::gnss::State::kMaintenance) {
        // 与 kSetOut 一致：kOff/kPowerUp/kProbing/kFailed 无运行中接收机，直接落盘，下次配置按新频率执行。
        const uint8_t prevRate = ensureRoverCfg().rateHz;
        const data::GnssOutConfig prevOut = out_cfg_;
        ensureRoverCfg().rateHz = cmd.args[0];
        const bool outAligned = (controller().detected().probe.model == ::gnss::Model::kZedF9p) &&
                                alignF9pOutputConfig(cmd.args[0]);
        const fw::Result saveRes = persistRateWithOut(prevRate, outAligned);
        if (!fw::isOk(saveRes)) {
            ensureRoverCfg().rateHz = prevRate;
            out_cfg_ = prevOut;
            result.status = saveRes;
            return;
        }
        if (outAligned) {
            ctx().data.gnssOutCfg.write(out_cfg_);
        }
        controller().setOutputPlan(makePlan());
        last_apply_ = GnssApply::kOk;
        ++apply_seq_;
        publishStatus(nowMs);
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
        result.status = fw::Result::kOk;
        return;
    }
    // 两阶段提交（ADR-0012 §2.4）：先暂存并重配，成功后才写 NVS；失败由 tickPendingApply 回滚。
    pending_prev_rate_ = ensureRoverCfg().rateHz;
    pending_prev_out_ = out_cfg_;
    pending_rate_out_aligned_ = false;
    ensureRoverCfg().rateHz = cmd.args[0];
    if (controller().detected().probe.model == ::gnss::Model::kZedF9p) {
        pending_rate_out_aligned_ = alignF9pOutputConfig(cmd.args[0]);
    }
    pending_apply_ = PendingApply::kRate;
    apply_deferred_ = controller().state() == ::gnss::State::kMaintenance;
    if (!apply_deferred_) {
        beginPendingApply(nowMs);
    }
    result.status = fw::Result::kOk;
}

void GnssModule::cmdSetOut(const fw::Command &cmd, uint32_t nowMs,
                           fw::CommandResult &result) noexcept
{
    data::GnssOutConfig next{};
    if ((cmd.argLen != data::GnssOutConfig::kBlobBytes) ||
        !fw::isOk(data::GnssOutConfig::decode(std::span<const uint8_t>{cmd.args.data(), cmd.argLen},
                                              next))) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    if (pending_apply_ != PendingApply::kNone) {
        result.status = fw::Result::kBusy;
        return;
    }
    const auto st = controller().state();
    if (isTransientApplyState(st)) {
        result.status = fw::Result::kBusy;
        return;
    }
    const data::GnssOutConfig prev = out_cfg_;
    out_cfg_ = next;
    ctx().data.gnssOutCfg.write(out_cfg_);
    traceGnssSub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
    if (st != ::gnss::State::kRunning && st != ::gnss::State::kMaintenance) {
        // kOff/kPowerUp/kProbing/kFailed 无可下发的输出阶段：直接落盘，下次配置完成时按新表重算计划。
        const fw::Result saveRes = ctx().storage.saveKeys(
            std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kOutKeyIndex, 1U>());
        if (!fw::isOk(saveRes)) {
            out_cfg_ = prev;
            ctx().data.gnssOutCfg.write(out_cfg_);
            result.status = saveRes;
            return;
        }
        controller().setOutputPlan(makePlan());
        last_apply_ = GnssApply::kOk;
        ++apply_seq_;
        publishStatus(nowMs);
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
        result.status = fw::Result::kOk;
        return;
    }
    pending_prev_out_ = prev;
    pending_apply_ = PendingApply::kOut;
    apply_deferred_ = controller().state() == ::gnss::State::kMaintenance;
    if (!apply_deferred_) {
        beginPendingApply(nowMs);
    }
    result.status = fw::Result::kOk;
}

void GnssModule::sendRaw(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto st = controller().state();
    if ((st != ::gnss::State::kRunning) && (st != ::gnss::State::kFailed)) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    if ((cmd.argLen == 0U) || (cmd.argLen > cmd.args.size())) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    // 按字符查看命令字节。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *command = reinterpret_cast<const char *>(cmd.args.data());
    const std::string_view text{command, cmd.argLen};
    if (text.find_first_of("\r\n") != std::string_view::npos ||
        text.find('\0') != std::string_view::npos ||
        detail::changesConnectedGnssBaud(text, board::gnssReceiverPort())) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    std::array<char, fw::kCommandArgBytes + 2U> line{};
    // argLen 已检查不超过固定 args；size_t 足以表示它。
    const auto len = static_cast<size_t>(cmd.argLen);
    std::memcpy(line.data(), cmd.args.data(), len);
    line[len] = '\r';
    line[len + 1U] = '\n';
    // Kconfig UART 号限制为 0..2。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int written = uart_write_bytes(port, line.data(), len + 2U);
    if (written < 0) {
        result.status = fw::Result::kIoError;
        return;
    }
    // 驱动返回非负写入字节数，最大为 line.size()，uint32_t 足以表示。
    uart_tx_bytes_ += static_cast<uint32_t>(written);
    result.status = fw::Result::kOk;
}

bool GnssModule::ggaAllowed() const noexcept
{
    return (context_ != nullptr) ? context_->data.ggaAllowed() : true;
}

bool GnssModule::decodeGgaGateChange(const fw::Command &cmd,
                                     ::gnss::GgaGateConfig &next) const noexcept
{
    constexpr uint8_t kValidFlags = kGgaGateFlagEnabled | kGgaGateFlagMinSats;
    if ((cmd.argLen != kGgaGateCmdArgLen) || (cmd.args[0] == 0U) ||
        ((cmd.args[0] & ~kValidFlags) != 0U)) {
        return false;
    }
    next = gga_gate_cfg_;
    if (!gga_gate_min_sats_loaded_) {
        next.minSats = ::gnss::GgaGateConfig::kDefaultMinSats;
    }
    if ((cmd.args[0] & kGgaGateFlagEnabled) != 0U) {
        if (cmd.args[1] > 1U) {
            return false;
        }
        next.enabled = (cmd.args[1] != 0U);
    }
    if ((cmd.args[0] & kGgaGateFlagMinSats) != 0U) {
        if (cmd.args[2] > ::gnss::GgaGateConfig::kMaxMinSats) {
            return false;
        }
        next.minSats = cmd.args[2];
    }
    return true;
}

void GnssModule::cmdApplyGgaGate(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!ctx().bootConfig.userNvsReady || ctx().bootConfig.safeDefaults) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    ::gnss::GgaGateConfig next{};
    const bool changeEnabled = cmd.args[0] == kGgaGateFlagEnabled;
    if ((!changeEnabled && cmd.args[0] != kGgaGateFlagMinSats) || !gga_gate_.has_value() ||
        !decodeGgaGateChange(cmd, next)) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    // 在副本上应用配置，避免 NVS 写失败改变拦截状态或连续达标计数。
    auto gate = *gga_gate_;
    if (!gate.setConfig(next)) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const size_t key =
        changeEnabled ? gnss_store::kGgaGateKeyIndex : gnss_store::kGgaMinSatsKeyIndex;
    const uint32_t value = changeEnabled ? static_cast<uint32_t>(next.enabled) : next.minSats;
    const esp_err_t error = store::writeValue(gnss_store::kConfigKeys[key], value);
    if (error != ESP_OK) {
        result.status = detail::toResult(rtos::fromEspErr(error));
        return;
    }
    gga_gate_cfg_ = next;
    *gga_gate_ = gate;
    gga_gate_enabled_loaded_ = true;
    gga_gate_min_sats_loaded_ = true;
    publishGgaGateStatus();
    ctx().data.configRev.fetch_add(1U, std::memory_order_release);
    result.status = fw::Result::kOk;
}

void GnssModule::cmdSetGgaGate(const fw::Command &cmd, uint32_t nowMs,
                               fw::CommandResult &result) noexcept
{
    ::gnss::GgaGateConfig next{};
    if (!gga_gate_.has_value() || !decodeGgaGateChange(cmd, next) || !gga_gate_->setConfig(next)) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    gga_gate_cfg_ = next;
    gga_gate_enabled_loaded_ = true;
    gga_gate_min_sats_loaded_ = true;
    publishGgaGateStatus();
    gga_gate_save_pending_ = true;
    gga_gate_save_changed_ms_ = nowMs;
    gga_gate_save_retry_ms_ = 0U;
    ctx().data.ggaGatePersist.store(data::GgaGatePersist::kPending, std::memory_order_release);
    result.status = fw::Result::kOk;
}

void GnssModule::tickGgaGateSave(uint32_t nowMs, bool forceImmediate) noexcept
{
    constexpr uint32_t kGgaGateDebounceMs = 500U;
    constexpr uint32_t kGgaGateRetryInitialMs = 1000U;
    constexpr uint32_t kGgaGateRetryMaxMs = 30000U;
    if (!gga_gate_save_pending_ || !store::canStartWrite()) {
        return;
    }
    if (!forceImmediate) {
        if ((nowMs - gga_gate_save_changed_ms_) < kGgaGateDebounceMs) {
            return;
        }
        if ((gga_gate_save_retry_ms_ != 0U) &&
            ((nowMs - gga_gate_save_attempt_ms_) < gga_gate_save_retry_ms_)) {
            return;
        }
    }
    if (!ctx().bootConfig.userNvsReady || ctx().bootConfig.safeDefaults) {
        gga_gate_save_pending_ = false;
        ctx().data.ggaGatePersist.store(data::GgaGatePersist::kFailed, std::memory_order_release);
        return;
    }
    const fw::Result res = ctx().storage.saveKeys(
        std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kGgaGateKeyIndex, 2U>());
    if (fw::isOk(res)) {
        gga_gate_save_pending_ = false;
        gga_gate_save_retry_ms_ = 0U;
        ctx().data.ggaGatePersist.store(data::GgaGatePersist::kClean, std::memory_order_release);
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
    } else {
        gga_gate_save_attempt_ms_ = nowMs;
        gga_gate_save_retry_ms_ = (gga_gate_save_retry_ms_ == 0U)
                                      ? kGgaGateRetryInitialMs
                                      : std::min(gga_gate_save_retry_ms_ * 2U, kGgaGateRetryMaxMs);
        ctx().data.ggaGatePersist.store(data::GgaGatePersist::kFailed, std::memory_order_release);
        FW_LOGW(fw::TraceCat::kGnss, "<cfg> GGA gate save failed (%s), retry in %lu ms",
                fw::toString(res), static_cast<unsigned long>(gga_gate_save_retry_ms_));
    }
}

} // namespace app
