// GNSS 模块控制器侧：状态机轮询、状态迁移日志、配置读回与待落地变更（ADR-0010 §2.7、ADR-0012 §2.7）。
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
#include "modules/gnss/GnssTxLog.hpp"
#include "modules/storage/StorageModule.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {
constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint8_t kConstBitGpsRequired = 1U << 7U;
constexpr uint32_t kUbloxConfigTimeoutMs = 500U;
constexpr uint32_t kUbloxUart1BaudKey = 0x40520001U;
constexpr uint32_t kUbloxUart2BaudKey = 0x40530001U;

/**
 * @brief 判断一帧 VALSET 是否为无应答的 UART 波特率切换命令。
 * @param bytes: 完整 UBX 帧字节。
 * @retval true 单键 UART1/UART2 波特率 VALSET；false 其他 VALSET。
 */
bool isBaudSwitchValset(std::span<const uint8_t> bytes) noexcept
{
    std::array<uint32_t, 2U> keys{};
    const size_t count = ::gnss::ublox::extractValsetKeys(bytes, keys);
    return (count == 1U) && ((keys[0U] == kUbloxUart1BaudKey) || (keys[0U] == kUbloxUart2BaudKey));
}

} // namespace

using detail::kMaxActionsPerPoll;
using detail::kReadbackLineBytes;
using detail::toResult;

void GnssModule::pollController(uint32_t nowMs) noexcept
{
    if (!rtcm_framer_.atFrameBoundary() && !ensureRtcmFrameBoundary(nowMs)) {
        return;
    }
    const uint32_t maintenanceSeq = controller().stats().maintenanceSeq;
    for (size_t iter = 0U; iter < kMaxActionsPerPoll; ++iter) {
        const ::gnss::Action act = controller().poll(nowMs);
        if (act.kind == ::gnss::Action::Kind::kNone) {
            break;
        }
        applyAction(act, nowMs);
        if (controller().stats().maintenanceSeq != maintenanceSeq) {
            break;
        }
    }
    updateControllerStatus(nowMs);
    if (controller().stats().maintenanceSeq != completed_maintenance_seq_) {
        completeMaintenance(nowMs);
    }
    finishReadbackIfDone();
}

void GnssModule::applyAction(const ::gnss::Action &act, uint32_t nowMs) noexcept
{
    switch (act.kind) {
    case ::gnss::Action::Kind::kPower: {
        const fw::Result err = applyPower(act.on);
        if (!fw::isOk(err)) {
            traceGnss(GnssTrace::kState, static_cast<uint32_t>(controller().state()),
                      static_cast<uint32_t>(err));
        }
        break;
    }
    case ::gnss::Action::Kind::kSetBaud: {
        const uint32_t oldBaud = host_baud_;
        const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
        // 9600 bps 下 1024 B 的 TX 队列最多约 1.07 s；必须等旧速率字节发完。
        constexpr uint32_t kUartDrainMs = 1200U;
        if (controller().state() == ::gnss::State::kMaintenance &&
            uart_wait_tx_done(port, pdMS_TO_TICKS(kUartDrainMs)) != ESP_OK) {
            controller().maintenanceIoFailed(rtos::nowMs());
            break;
        }
        const fw::Result err = applyBaud(act.baud);
        if (fw::isOk(err)) {
            traceGnss(GnssTrace::kBaud, act.baud, oldBaud);
            FW_LOGI(fw::TraceCat::kGnss, "<uart> baud %lu (was %lu)",
                    static_cast<unsigned long>(act.baud), static_cast<unsigned long>(oldBaud));
        } else {
            controller().maintenanceIoFailed(nowMs);
        }
        break;
    }
    case ::gnss::Action::Kind::kWrite:
        applyWriteAction(act, nowMs);
        break;
    case ::gnss::Action::Kind::kNone:
    default:
        break;
    }
}

void GnssModule::finishReadbackIfDone() noexcept
{
    if (controller().stats().readbackSeq == readback_base_seq_) {
        return;
    }
    readback_base_seq_ = controller().stats().readbackSeq;
    FW_LOGI(fw::TraceCat::kGnss, "<cfg> Readback done, %u lines%s",
            static_cast<unsigned>(readback_lines_), readback_truncated_ ? " (truncated)" : "");
    publishReadback(readback_lines_, readback_len_, true, readback_truncated_, false);
}

/** @brief 发布控制器迁移诊断并标记待重放设置。 @param nowMs: 本轮单调时钟（毫秒）。 @note 仅 kIo。 */
void GnssModule::updateControllerStatus(uint32_t nowMs) noexcept
{
    const auto curStats = controller().stats();
    if (curStats.stepNacks > prev_ctrl_stats_.stepNacks) {
        traceGnss(GnssTrace::kStepNack, step_idx_, 0U);
    }
    if (curStats.stepTimeouts > prev_ctrl_stats_.stepTimeouts) {
        traceGnss(GnssTrace::kStepTimeout, step_idx_, step_retry_ + 1U);
    }
    const auto curState = controller().state();
    if (curState != last_state_) {
        if ((last_state_ == ::gnss::State::kProbing) && (curState == ::gnss::State::kConfiguring)) {
            ppp_replay_ = true;
            sat_replay_ = true;
        }
        traceGnss(GnssTrace::kState, static_cast<uint32_t>(curState),
                  static_cast<uint32_t>(last_state_));
        FW_LOGI(fw::TraceCat::kGnss, "<sm> state %s -> %s", ::gnss::stateName(last_state_),
                ::gnss::stateName(curState));
        onControllerStateChanged(last_state_, curState, nowMs);
        last_state_ = curState;
    }
    prev_ctrl_stats_ = curStats;
}

void GnssModule::appendReadbackLine(const demux::Decoder &decoder,
                                    std::span<const uint8_t> frame) noexcept
{
    std::array<char, kReadbackLineBytes> line{};
    // 留一格给 '\0'：下面按 %s 打 D 级日志，正文截断到 kReadbackLineBytes-1 才不会越界读。
    const size_t len = controller().receiver().matchReadback(
        decoder, frame, std::span<char>(line.data(), line.size() - 1U));
    if (len == 0U) {
        return;
    }
    ++readback_lines_; // 命中即计数：即使整行因容量不足被丢弃也算一行（见 GnssReadback::lines）
    FW_LOGD(fw::TraceCat::kGnss, "<cfg> RX %s", line.data());
    auto &text = ctx().data.gnssReadbackText;
    const size_t used = readback_len_;
    if ((used + len + 1U) > text.size()) {
        readback_truncated_ = true;
        return;
    }
    for (size_t i = 0U; i < len; ++i) {
        text[used + i] = line[i];
    }
    text[used + len] = '\n';
    // 上面的容量检查已保证 used + len + 1 <= text.size()（1536），窄化到 uint16_t 安全。
    readback_len_ = static_cast<uint16_t>(used + len + 1U);
}

void GnssModule::startReadback(uint32_t nowMs, fw::CommandResult &result) noexcept
{
    ::gnss::CmdBuf probe{};
    // 先单独问一句驱动能力：requestReadback() 把“不支持”与“状态不对”合为一个 false。
    const bool supported = controller().receiver().buildReadback(probe);
    if (controller().requestReadback(nowMs)) {
        readback_len_ = 0U;
        readback_lines_ = 0U;
        readback_truncated_ = false;
        readback_base_seq_ = controller().stats().readbackSeq;
        result.status = fw::Result::kOk;
        return;
    }
    publishReadback(0U, 0U, supported, false, supported);
    result.status = supported ? fw::Result::kBusy : fw::Result::kNotSupported;
}

void GnssModule::publishReadback(uint16_t lines, uint16_t len, bool supported, bool truncated,
                                 bool busy) noexcept
{
    const uint32_t statsSeq = controller().stats().readbackSeq;
    const uint32_t seq = statsSeq > readback_pub_seq_ ? statsSeq : (readback_pub_seq_ + 1U);
    readback_pub_seq_ = seq;
    GnssReadback out{};
    out.seq = seq;
    out.lines = lines;
    out.len = len;
    out.supported = supported;
    out.truncated = truncated;
    out.busy = busy;
    ctx().data.gnssReadback.write(out);
}

void GnssModule::tickPendingApply(uint32_t nowMs) noexcept
{
    if (pending_apply_ == PendingApply::kNone) {
        return;
    }
    if (apply_deferred_) {
        if (controller().state() == ::gnss::State::kRunning) {
            beginPendingApply(nowMs);
        }
        return;
    }
    const auto stats = controller().stats();
    const bool landed = stats.outputSeq == pending_seq_;
    // 超时按有符号差判断，毫秒计数回绕时仍成立。
    const bool timeout = static_cast<int32_t>(nowMs - pending_deadline_ms_) >= 0;
    if (!landed && !timeout) {
        return;
    }
    fw::Result saveRes = fw::Result::kIoError;
    if (landed && (stats.failedMask == 0U)) {
        if (pending_apply_ == PendingApply::kRate) {
            saveRes = persistRateWithOut(pending_prev_rate_, pending_rate_out_aligned_);
            if (fw::isOk(saveRes) && pending_rate_out_aligned_) {
                ctx().data.gnssOutCfg.write(out_cfg_);
            }
        } else {
            saveRes = ctx().storage.saveKeys(
                std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kOutKeyIndex, 1U>());
        }
    }
    if (fw::isOk(saveRes)) {
        last_apply_ = GnssApply::kOk;
        pending_apply_ = PendingApply::kNone;
        pending_rate_out_aligned_ = false;
        ++apply_seq_;
    } else {
        // 有语句失败或超时：NVS 保持旧值，界面与计划一起回滚到变更前；改频率回滚须重配 CFG-RATE-MEAS。
        const bool wasRate = (pending_apply_ == PendingApply::kRate);
        restorePendingApply();
        controller().setOutputPlan(makePlan());
        if (wasRate) {
            controller().requestReconfigure(nowMs);
        } else {
            static_cast<void>(controller().requestOutput(controller().outputPlan(), nowMs));
        }
    }
    publishStatus(nowMs);
    if (fw::isOk(saveRes)) {
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
    }
    completeSetting(applyTicketSeq_, saveRes);
}

fw::Result GnssModule::persistRateWithOut(uint8_t prevRate, bool outAligned) noexcept
{
    const auto kRateKeys =
        std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kRateKeyIndex, 1U>();
    fw::Result res = ctx().storage.saveKeys(kRateKeys);
    if (!fw::isOk(res) || !outAligned) {
        if (!fw::isOk(res)) {
            ensureRoverCfg().rateHz = prevRate;
        }
        return res;
    }
    res = ctx().storage.saveKeys(
        std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kOutKeyIndex, 1U>());
    if (fw::isOk(res)) {
        return res;
    }
    // out 写失败而 rate 已落盘：把旧 rate 写回，避免 NVS 留下“新 rate + 旧 out”。
    const uint8_t newRate = ensureRoverCfg().rateHz;
    ensureRoverCfg().rateHz = prevRate;
    const fw::Result comp = ctx().storage.saveKeys(kRateKeys);
    if (!fw::isOk(comp)) {
        FW_LOGE(fw::TraceCat::kGnss,
                "<cfg> rate %u->%u: out save failed (%s), rate compensation failed (%s); "
                "boot will realign F9P output",
                static_cast<unsigned>(prevRate), static_cast<unsigned>(newRate), fw::toString(res),
                fw::toString(comp));
    }
    return res;
}

void GnssModule::restorePendingApply() noexcept
{
    if (pending_apply_ == PendingApply::kOut) {
        out_cfg_ = pending_prev_out_;
        ctx().data.gnssOutCfg.write(out_cfg_);
    } else {
        ensureRoverCfg().rateHz = pending_prev_rate_;
        if (pending_rate_out_aligned_) {
            out_cfg_ = pending_prev_out_;
            ctx().data.gnssOutCfg.write(out_cfg_);
        }
    }
    pending_apply_ = PendingApply::kNone;
    pending_rate_out_aligned_ = false;
    apply_deferred_ = false;
    pending_seq_ = 0U;
    pending_deadline_ms_ = 0U;
    last_apply_ = GnssApply::kRestored;
    ++apply_seq_;
}

fw::Result GnssModule::persistDetectedHint() noexcept
{
    const auto &det = controller().detected();
    if (det.receiver == nullptr) {
        return fw::Result::kOk;
    }
    const ::gnss::Family detFamily = det.receiver->family();
    const ::gnss::Model detModel = det.probe.model;
    const uint32_t detBaud = (det.baud != 0U) ? det.baud : host_baud_;
    if ((detFamily == hint_.family) && (detModel == hint_.model) && (detBaud == hint_.baud)) {
        return fw::Result::kOk;
    }
    hint_.family = detFamily;
    hint_.model = detModel;
    hint_.baud = detBaud;
    // StorageModule::saveKeys 内部发 kStore trace；保留 hint_ 内存值避免每轮重复写盘。
    return ctx().storage.saveKeys(gnss_store::kCacheKeys);
}

void GnssModule::persistProbeHint() noexcept
{
    if (skip_hint_save_) {
        skip_hint_save_ = false;
    } else if (!boot_baud_pending_ && !ctx().bootConfig.safeDefaults &&
               ctx().bootConfig.userNvsReady) {
        static_cast<void>(persistDetectedHint());
    }
}

fw::Result GnssModule::applyPower(bool on) noexcept
{
    const auto &pins = board::current().gnss;
    if (pins.powerGpio < 0) {
        return fw::Result::kOk;
    }
    // 非负的电源脚由板型表限定为已核实的输出 GPIO。
    const auto powerPin = static_cast<gpio_num_t>(pins.powerGpio);
    const uint32_t level = (on == pins.powerActiveHigh) ? 1U : 0U;
    return toResult(rtos::fromEspErr(gpio_set_level(powerPin, level)));
}

fw::Result GnssModule::applyBaud(uint32_t baud) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t baudErr = uart_set_baudrate(port, baud);
    if (baudErr != ESP_OK) {
        return toResult(rtos::fromEspErr(baudErr));
    }
    const esp_err_t flushErr = uart_flush_input(port);
    if (flushErr != ESP_OK) {
        return toResult(rtos::fromEspErr(flushErr));
    }
    host_baud_ = baud;
    return fw::Result::kOk;
}

uint8_t GnssModule::computeConstMask(::gnss::Model model) const noexcept
{
    if (!::gnss::supportsSatMask(model)) {
        return 0U;
    }
    if (model == ::gnss::Model::kZedF9p) {
        // F9P 强制要求 GPS 开启（不可由网页/手机关闭），SBAS 仅 HPG >= 1.13 支持。
        uint8_t mask = static_cast<uint8_t>(kConstBitGpsRequired | settings::const_bit::kBds |
                                            settings::const_bit::kGlo | settings::const_bit::kGal);
        if (f9pIncludeSbas()) {
            mask = static_cast<uint8_t>(mask | settings::const_bit::kSbas);
        }
        return mask;
    }
    return static_cast<uint8_t>(settings::const_bit::kGps | settings::const_bit::kBds |
                                settings::const_bit::kGlo | settings::const_bit::kGal |
                                settings::const_bit::kSbas);
}

void GnssModule::publishCaps() noexcept
{
    const auto model = controller().detected().probe.model;
    settings::GnssCaps caps{.enabled = initialized_};
    if (model != ::gnss::Model::kUnknown) {
        const auto &receiver = controller().receiver();
        caps.valid = true;
        // 两阶段提交期间保持已确认的定位频率。
        caps.navRateHz =
            pending_apply_ == PendingApply::kRate ? pending_prev_rate_ : ensureRoverCfg().rateHz;
        for (const auto rate : receiver.navRatesHz(model)) {
            if (caps.navRateCount < caps.navRatesHz.size()) {
                caps.navRatesHz[caps.navRateCount++] = rate;
            }
        }
        constexpr std::array kMessages{::gnss::OutputMsg::kGga, ::gnss::OutputMsg::kGsa,
                                       ::gnss::OutputMsg::kGsv, ::gnss::OutputMsg::kGst,
                                       ::gnss::OutputMsg::kRmc};
        const uint32_t minPeriod = kMsPerSecond / std::max(caps.navRateHz, uint8_t{1U});
        for (size_t i = 0U; i < kMessages.size(); ++i) {
            const bool gsvDisabled =
                kMessages[i] == ::gnss::OutputMsg::kGsv && !ensureRoverCfg().gsv;
            fillOutputPeriodDomain(model, caps.navRateHz, minPeriod, gsvDisabled,
                                   receiver.outputPeriodsMs(kMessages[i], model),
                                   caps.outPeriods[i]);
        }
        caps.pppSupported = ::gnss::supportsPpp(model);
        caps.constMask = computeConstMask(model);
    }
    if (!caps.sameCaps(ctx().data.gnssCaps.read())) {
        ctx().data.gnssCaps.write(caps);
    }
}

void GnssModule::persistRestoredRate() noexcept
{
    if (ctx().bootConfig.safeDefaults || !ctx().bootConfig.userNvsReady) {
        return;
    }
    const fw::Result saveRes = ctx().storage.saveKeys(
        std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kRateKeyIndex, 1U>());
    if (fw::isOk(saveRes)) {
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
    } else {
        FW_LOGW(fw::TraceCat::kGnss, "<cfg> persist restored GNSS rate failed (%s)",
                fw::toString(saveRes));
    }
}

void GnssModule::restoreSettingsForModel() noexcept
{
    const auto model = controller().detected().probe.model;
    const auto &receiver = controller().receiver();
    const auto rates = receiver.navRatesHz(model);
    if (rates.empty()) {
        return;
    }
    auto &rate = ensureRoverCfg().rateHz;
    if (std::ranges::find(rates, rate) == rates.end()) {
        const auto upper = std::ranges::upper_bound(rates, rate);
        const auto restored = upper == rates.begin() ? rates.front() : *(upper - 1);
        FW_LOGW(fw::TraceCat::kGnss, "<cfg> restored GNSS rate %u as %u Hz",
                static_cast<unsigned>(rate), static_cast<unsigned>(restored));
        rate = restored;
        persistRestoredRate();
    }
    if (restoredSettings_) {
        return;
    }
    restoredSettings_ = true;

    restoreOutputPeriods();
}

::gnss::OutputMsg GnssModule::outputCapabilityMsg(::gnss::OutputMsg msg) noexcept
{
    constexpr uint32_t kSatsInfoBit = 1U << static_cast<uint8_t>(::gnss::OutputMsg::kSatsInfo);
    const auto model = controller().detected().probe.model;
    if (msg == ::gnss::OutputMsg::kSatsInfo &&
        (controller().receiver().outputMask(model) & kSatsInfoBit) == 0U) {
        return ::gnss::OutputMsg::kGsv;
    }
    return msg;
}

bool GnssModule::isF9pPeriodValid(uint32_t periodMs, uint8_t rateHz) noexcept
{
    constexpr uint32_t kMaxEpochs = 255U;
    if (periodMs == 0U) {
        return true;
    }
    const uint32_t hz = (rateHz == 0U) ? 1U : static_cast<uint32_t>(rateHz);
    if ((kMsPerSecond % hz) != 0U) {
        return false;
    }
    const uint32_t navPeriod = kMsPerSecond / hz;
    return (periodMs >= navPeriod) && ((periodMs % navPeriod) == 0U) &&
           ((periodMs / navPeriod) <= kMaxEpochs);
}

uint16_t GnssModule::alignF9pPeriodMs(std::span<const uint16_t> periods, uint32_t periodMs,
                                      uint8_t rateHz) noexcept
{
    if (periodMs == 0U) {
        return 0U;
    }
    uint16_t lastValid = 0U;
    for (const uint16_t candidate : periods) {
        if ((candidate == 0U) || !isF9pPeriodValid(candidate, rateHz)) {
            continue;
        }
        lastValid = candidate;
        if (candidate >= periodMs) {
            return candidate;
        }
    }
    return lastValid;
}

bool GnssModule::alignF9pOutputConfig(uint8_t rateHz) noexcept
{
    const auto model = controller().detected().probe.model;
    const auto &receiver = controller().receiver();
    bool changed = false;
    for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
        if ((out_cfg_.mask & (1U << i)) == 0U || data::followsFixRate(out_cfg_, i)) {
            continue;
        }
        // App 语句下标与 OutputMsg 的前缀一致。
        const auto msg = outputCapabilityMsg(static_cast<::gnss::OutputMsg>(i));
        const auto periods = receiver.outputPeriodsMs(msg, model);
        if (periods.empty()) {
            continue;
        }
        const uint32_t period =
            core::services::GnssService::intervalToPeriodMs(out_cfg_.intervals[i]);
        const uint16_t aligned = alignF9pPeriodMs(periods, period, rateHz);
        if (aligned == period) {
            continue;
        }
        int8_t interval = 0;
        if (!settings::periodMsToInterval(static_cast<int32_t>(aligned), interval)) {
            interval = 0;
        }
        if (fw::isOk(out_cfg_.applyInterval(i, interval))) {
            changed = true;
            FW_LOGW(fw::TraceCat::kGnss, "<cfg> aligned F9P output[%u] from %lu to %lu ms",
                    static_cast<unsigned>(i), static_cast<unsigned long>(period),
                    static_cast<unsigned long>(
                        core::services::GnssService::intervalToPeriodMs(interval)));
        }
    }
    return changed;
}

void GnssModule::fillOutputPeriodDomain(::gnss::Model model, uint8_t navRateHz, uint32_t minPeriod,
                                        bool gsvDisabled, std::span<const uint16_t> periods,
                                        settings::PeriodDomain &domain) noexcept
{
    for (const auto period : periods) {
        const bool allowed =
            (period == 0U) ||
            (!gsvDisabled && (model == ::gnss::Model::kZedF9p ? isF9pPeriodValid(period, navRateHz)
                                                              : (period >= minPeriod)));
        if (allowed && (domain.count < domain.values.size())) {
            domain.values[domain.count++] = period;
        }
    }
    domain.supported = domain.count != 0U;
}

void GnssModule::persistAlignedF9pOutput() noexcept
{
    if (ctx().bootConfig.safeDefaults || !ctx().bootConfig.userNvsReady) {
        return;
    }
    const fw::Result saveRes = ctx().storage.saveKeys(
        std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kOutKeyIndex, 1U>());
    if (fw::isOk(saveRes)) {
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
    } else {
        FW_LOGW(fw::TraceCat::kGnss, "<cfg> persist aligned F9P output failed (%s)",
                fw::toString(saveRes));
    }
}

void GnssModule::restoreLegacyOutputPeriods() noexcept
{
    const auto model = controller().detected().probe.model;
    const auto &receiver = controller().receiver();
    for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
        if ((out_cfg_.mask & (1U << i)) == 0U || data::followsFixRate(out_cfg_, i)) {
            continue;
        }
        // App 语句下标与 OutputMsg 的前缀一致。
        const auto msg = outputCapabilityMsg(static_cast<::gnss::OutputMsg>(i));
        const auto periods = receiver.outputPeriodsMs(msg, model);
        const uint32_t period =
            core::services::GnssService::intervalToPeriodMs(out_cfg_.intervals[i]);
        const auto slower = std::ranges::lower_bound(periods, period);
        if (periods.empty() || (slower != periods.end() && *slower == period)) {
            continue;
        }
        const uint16_t restored = slower == periods.end() ? periods.back() : *slower;
        int8_t interval = 0;
        // 驱动能力表里的值应能由输出配置编码。若未来添加不可编码值，回到关闭。
        if (!settings::periodMsToInterval(static_cast<int32_t>(restored), interval)) {
            interval = 0;
        }
        if (fw::isOk(out_cfg_.applyInterval(i, interval))) {
            FW_LOGW(fw::TraceCat::kGnss, "<cfg> restored GNSS output[%u] from %lu to %lu ms",
                    static_cast<unsigned>(i), static_cast<unsigned long>(period),
                    static_cast<unsigned long>(
                        core::services::GnssService::intervalToPeriodMs(interval)));
        }
    }
    ctx().data.gnssOutCfg.write(out_cfg_);
}

void GnssModule::restoreOutputPeriods() noexcept
{
    if (controller().detected().probe.model == ::gnss::Model::kZedF9p) {
        const bool changed = alignF9pOutputConfig(ensureRoverCfg().rateHz);
        ctx().data.gnssOutCfg.write(out_cfg_);
        if (changed) {
            persistAlignedF9pOutput();
        }
        return;
    }
    restoreLegacyOutputPeriods();
}

::gnss::OutputPlan GnssModule::makePlan() noexcept
{
    uint8_t rateHz = 0U;
    bool gsv = false;
    if (rover_cfg_.has_value()) {
        rateHz = rover_cfg_->rateHz;
        gsv = rover_cfg_->gsv;
    }
    const uint32_t caps = controller().detected().caps;
    auto plan = data::makeOutputPlan(out_cfg_, rateHz, gsv, board::gnssReceiverPort(), caps);
    const auto model = controller().detected().probe.model;
    if (model == ::gnss::Model::kCm680) {
        const uint32_t minPeriod = kMsPerSecond / plan.rateHz;
        for (auto &period : plan.periodMs) {
            if (period != 0U) {
                period = std::max(period, minPeriod);
            }
        }
        constexpr auto kGsa = static_cast<size_t>(::gnss::OutputMsg::kGsa);
        // 自适应 GSA 的默认规则是 1 Hz。显式值仍由驱动校验。
        if (plan.periodMs[kGsa] != 0U && data::followsFixRate(out_cfg_, kGsa)) {
            plan.periodMs[kGsa] = kMsPerSecond;
        }
    } else if (model == ::gnss::Model::kZedF9p) {
        const auto &receiver = controller().receiver();
        constexpr auto kGsa = static_cast<size_t>(::gnss::OutputMsg::kGsa);
        // 自适应 GSA 默认按 1 Hz 输出，避免高频下多系统 GSA 泛洪 SPP。
        if (plan.periodMs[kGsa] != 0U && data::followsFixRate(out_cfg_, kGsa)) {
            plan.periodMs[kGsa] = kMsPerSecond;
        }
        for (size_t i = 0U; i < plan.periodMs.size(); ++i) {
            if (plan.periodMs[i] == 0U) {
                continue;
            }
            // 下标与 OutputMsg 一致（0..kOutputMsgCount-1）。
            const auto msg = outputCapabilityMsg(static_cast<::gnss::OutputMsg>(i));
            const auto periods = receiver.outputPeriodsMs(msg, model);
            if (periods.empty()) {
                continue;
            }
            plan.periodMs[i] = alignF9pPeriodMs(periods, plan.periodMs[i], plan.rateHz);
        }
    }
    return plan;
}

void GnssModule::applyWriteAction(const ::gnss::Action &act, uint32_t nowMs) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int written = uart_write_bytes(port, act.bytes.data(), act.bytes.size());
    if (written > 0) {
        uart_tx_bytes_ += static_cast<uint32_t>(written);
    }
    const bool ok = (written >= 0) && (static_cast<size_t>(written) == act.bytes.size());
    if (ok) {
        recordValsetTx(act.bytes, nowMs, kUbloxConfigTimeoutMs, !isBaudSwitchValset(act.bytes));
    } else {
        if (::gnss::ublox::isValsetFrame(act.bytes)) {
            valset_tracker_.reset();
        }
        controller().maintenanceIoFailed(nowMs);
    }
    // 控制器下发的命令：探测阶段标 <probe>，其余标 <cfg>，方向固定写 TX（ADR-0009 §2.12）；
    // 全可打印 ASCII 保留原文 "..."，二进制 UBX 输出前 16 字节十六进制摘要。
    std::array<char, ::gnss::kMaxCommand + 16U> logText{};
    static_cast<void>(detail::formatGnssTxLog(act.bytes, logText));
    const auto st = controller().state();
    FW_LOGI(fw::TraceCat::kGnss, "%s TX %s", (st == ::gnss::State::kProbing) ? "<probe>" : "<cfg>",
            logText.data());
    const auto curStats = controller().stats();
    if ((st == ::gnss::State::kProbing) && (curStats.probesSent > prev_ctrl_stats_.probesSent)) {
        traceGnss(GnssTrace::kProbeSend,
                  static_cast<uint32_t>(controller().probeReceiver().family()), host_baud_);
        prev_ctrl_stats_.probesSent = curStats.probesSent;
    } else if ((st == ::gnss::State::kConfiguring) &&
               (curStats.stepsSent > prev_ctrl_stats_.stepsSent)) {
        const bool retried = (curStats.stepNacks > prev_ctrl_stats_.stepNacks) ||
                             (curStats.stepTimeouts > prev_ctrl_stats_.stepTimeouts);
        if (retried) {
            ++step_retry_;
        } else if (step_sent_once_) {
            traceGnss(GnssTrace::kStepAck, step_idx_, 0U);
            ++step_idx_;
            step_retry_ = 0U;
        }
        step_sent_once_ = true;
        traceGnss(GnssTrace::kStepSend, step_idx_, step_retry_);
        prev_ctrl_stats_.stepsSent = curStats.stepsSent;
    }
}

void GnssModule::recordValsetTx(std::span<const uint8_t> bytes, uint32_t nowMs, uint32_t timeoutMs,
                                bool expectReply) noexcept
{
    valset_tracker_.onTx(bytes, nowMs, timeoutMs, expectReply);
}

void GnssModule::pollUbloxBoardSn(uint32_t nowMs) noexcept
{
    if (uniqid_polled_ || (controller().detected().probe.model != ::gnss::Model::kZedF9p)) {
        return;
    }
    if (!ensureRtcmFrameBoundary(nowMs)) {
        return;
    }
    uniqid_polled_ = true;
    ::gnss::CmdBuf pollCmd{};
    if (::gnss::ublox::buildPollSecUniqId(pollCmd)) {
        static_cast<void>(writeSeqTx(pollCmd.view(), nowMs, 0U));
    }
}

void GnssModule::resetOnReprobe() noexcept
{
    uniqid_polled_ = false;
    valset_tracker_.reset();
    rtcm_framer_.reset();
    rtcm_frame_stall_armed_ = false;
    ctx().data.gnssBoardSn.write({});
    if (work_mode_.has_value()) {
        auto &wm = *work_mode_;
        wm.onControllerReprobe();
        ctx().data.gnssWorkMode.write(wm.status());
    }
}

void GnssModule::onControllerStateChanged(::gnss::State prev, ::gnss::State cur,
                                          uint32_t nowMs) noexcept
{
    if ((prev == ::gnss::State::kProbing) && (cur == ::gnss::State::kConfiguring)) {
        const auto det = controller().detected();
        const uint32_t baud = (det.baud != 0U) ? det.baud : host_baud_;
        traceGnss(GnssTrace::kProbeHit, static_cast<uint32_t>(det.probe.model), baud);
        FW_LOGI(fw::TraceCat::kGnss, "<probe> detected %s at %lu",
                ::gnss::modelName(det.probe.model), static_cast<unsigned long>(baud));
        step_idx_ = 0U;
        step_retry_ = 0U;
        step_sent_once_ = false;
        detect_pending_ = true;
        restoreSettingsForModel();
        controller().setOutputPlan(makePlan());
    } else if ((cur == ::gnss::State::kRunning) && detect_pending_) {
        // 探测后要经过 kConfiguring → kOutput → kRunning，只看 configuring→running 会漏掉（P3b 起）。
        detect_pending_ = false;
        if (step_sent_once_) {
            traceGnss(GnssTrace::kStepAck, step_idx_, 0U);
        }
        persistProbeHint();
        // 型号与 caps 已知，输出计划按支持的语句位重算一次（ADR-0012 §2.4）。
        controller().setOutputPlan(makePlan());
        pollUbloxBoardSn(nowMs);
    } else if ((prev == ::gnss::State::kRunning) && (cur == ::gnss::State::kProbing)) {
        const auto curStats = controller().stats();
        if (!manual_reprobe_ && (curStats.reprobes > prev_ctrl_stats_.reprobes)) {
            const uint32_t silenceMs = timing_.has_value() ? timing_->silenceMs : 0U;
            traceGnss(GnssTrace::kSilence, silenceMs, 0U);
        }
        manual_reprobe_ = false;
    } else {
        // 其余迁移（断电/失败/读回）没有额外副作用。
    }
    if ((cur == ::gnss::State::kPowerUp) || (cur == ::gnss::State::kProbing)) {
        resetOnReprobe();
    }
}

} // namespace app
