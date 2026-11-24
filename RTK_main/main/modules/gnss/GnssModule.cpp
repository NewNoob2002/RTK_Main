// GNSS 模块实现（ADR-0010 §2.7）：UART 收发、StreamDemux 分流、NMEA 解析、控制器轮询、RTCM 下发与 NVS 绑定。
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
#include "modules/storage/StorageModule.hpp"
#include "rtos/EspStatus.hpp"

namespace app {

static_assert(gnss_store::validUserBaud(kGnssTargetBaud),
              "RTK_GNSS_TARGET_BAUD must be a supported baud rate");

using detail::addUniqueDecoder;
using detail::toResult;
using detail::traceGnssSub;

namespace {

enum class RtcmTxGate : uint8_t { kAllow, kHold, kDiscard };

/**
 * @brief 判定当前控制器状态是否允许 RTCM3 下发、暂存或丢弃。
 * @param st: 控制器状态。
 * @param maint: 当前维护操作类型。
 * @param baseBlocksRx: 基站模式是否禁止接收 RTCM。
 * @param finishMidFrame: 是否正在收尾已发出一半的 RTCM3 帧。
 * @retval kAllow 允许写串口；kHold 保留缓冲不发；kDiscard 丢弃管道。
 */
RtcmTxGate classifyRtcmTxGate(::gnss::State st, ::gnss::Maintenance maint, bool baseBlocksRx,
                              bool finishMidFrame) noexcept
{
    if ((st == ::gnss::State::kMaintenance) && (maint != ::gnss::Maintenance::kSave) &&
        !finishMidFrame) {
        return RtcmTxGate::kHold; // 切速与复位期间保留待发数据，避免旧波特率字节混入维护命令。
    }
    const bool allowMidFrameDrain =
        finishMidFrame && ((st == ::gnss::State::kOutput) || (st == ::gnss::State::kReadback));
    const bool allowTx =
        !baseBlocksRx && ((st == ::gnss::State::kRunning) || (st == ::gnss::State::kConfiguring) ||
                          (st == ::gnss::State::kProbing) || (st == ::gnss::State::kMaintenance) ||
                          allowMidFrameDrain);
    return allowTx ? RtcmTxGate::kAllow : RtcmTxGate::kDiscard;
}

} // namespace

fw::Result GnssModule::initObjects() noexcept
{
    sink_.emplace(this);
    demux_.emplace();
    parser_.emplace();
    controller_.emplace();
    timing_.emplace();
    work_mode_.emplace();
    auto &gate = gga_gate_.emplace();
    // 成员默认零初始化以保证 AppContext 落入 .bss；未从 NVS 加载的项在此回填默认配置。
    initBaseDefaults();
    if (!gga_gate_enabled_loaded_) {
        gga_gate_cfg_.enabled = false;
        gga_gate_enabled_loaded_ = true;
    }
    if (!gga_gate_min_sats_loaded_) {
        gga_gate_cfg_.minSats = ::gnss::GgaGateConfig::kDefaultMinSats;
        gga_gate_min_sats_loaded_ = true;
    }
    static_cast<void>(gate.setConfig(gga_gate_cfg_));
    publishGgaGateStatus();
    auto &cfg = ensureRoverCfg();
    if (!rate_loaded_) {
        cfg.rateHz = static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ);
        rate_loaded_ = true;
    }
    ctx().data.gnssRateHz.store(cfg.rateHz, std::memory_order_release);
    if (!gsv_loaded_) {
        cfg.gsv = true;
    }
    cfg.port = board::gnssReceiverPort();
    cfg.useConfiguredPort = true;
    if (!out_cfg_.isValid()) {
        out_cfg_ = data::GnssOutConfig::defaults();
    }
    return fw::Result::kOk;
}

fw::Result GnssModule::initPipelines() noexcept
{
    preferred_baud_ = ctx().bootConfig.gnssBaud;
    initBaudCandidates();
    const fw::Result uartResult = initUart();
    if (!fw::isOk(uartResult)) {
        return uartResult;
    }
    const fw::Result pipeRes = toResult(ctx().rtcmPipe.init());
    if (!fw::isOk(pipeRes)) {
        return initFailed(pipeRes);
    }
    // sink_ 由 initObjects() 建立；在此显式取引用，避免依赖跨函数的 optional 状态。
    if (!sink_.has_value()) {
        return initFailed(fw::Result::kInvalidState);
    }
    DemuxSink &sink = *sink_;
    const std::span<const ::gnss::Receiver *const> receivers = ::gnss::enabledReceivers();
    registerDecoders(receivers);
    const demux::Error demuxErr = streamDemux().init(
        std::span<const demux::Decoder *const>{decoders_.data(), decoder_count_}, demux_buf_, sink);
    if (demuxErr != demux::Error::kOk) {
        return initFailed(fw::Result::kInvalidState);
    }
    return fw::Result::kOk;
}

fw::Result GnssModule::startController() noexcept
{
    const auto &cfg = ensureRoverCfg();
    // timing_ 由 initObjects() 建立；显式取引用避免依赖跨函数的 optional 状态。
    if (!timing_.has_value()) {
        return initFailed(fw::Result::kInvalidState);
    }
    const ::gnss::Timing &clock = *timing_;
    const std::span<const ::gnss::Receiver *const> receivers = ::gnss::enabledReceivers();
    // 输出计划是唯一来源（ADR-0012 §2.4）：先按默认值给一份，探测到型号后再按 caps 重算。
    controller().setOutputPlan(data::makeOutputPlan(out_cfg_, cfg.rateHz, cfg.gsv, cfg.port, 0U));
    if (hint_.baud == 0U) {
        hint_.baud = preferred_baud_;
    }
    if (!controller().init(receivers, std::span<const uint32_t>{bauds_.data(), baud_count_}, cfg,
                           controller().outputPlan(), hint_, clock)) {
        return initFailed(fw::Result::kInvalidState);
    }
    boot_baud_pending_ = true;
    boot_baud_in_flight_ = false;
    boot_baud_save_in_flight_ = false;
    baud_fail_count_ = 0U;
    ctx().data.gnssBaudFailCount.store(0U, std::memory_order_relaxed);
    // initUart 已按同一候选首项配置端口。
    host_baud_ = bauds_.front();
    last_state_ = controller().state();
    last_published_state_ = last_state_;
    prev_ctrl_stats_ = controller().stats();
    ctx().data.gnssOutCfg.write(out_cfg_);
    traceGnssSub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
    return fw::Result::kOk;
}

/**
 * @brief 初始化 UART、分流器与控制器
 * @retval kOk 已初始化或重复调用
 * @retval kInvalidState 上下文、外供存储或控制对象状态不合法
 * @retval kInvalidArg UART/GPIO 或管道参数不合法
 * @retval kNoSpace 驱动或管道创建资源不足
 * @retval kIoError IDF 操作失败；清理失败时优先返回清理错误
 * @note 仅启动前单线程调用；UART 安装可阻塞，非 ISR。
 */
fw::Result GnssModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kOk;
    }
    if ((context_ == nullptr) || (demux_buf_.size() < kDemuxBufBytes)) {
        return fw::Result::kInvalidState;
    }
    fw::Result step = initObjects();
    if (fw::isOk(step)) {
        step = initPipelines();
    }
    if (fw::isOk(step)) {
        step = startController();
    }
    if (!fw::isOk(step)) {
        return step;
    }
    initialized_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::initUart() noexcept
{
    const auto &pins = board::current().gnss;
    if (pins.uart != CONFIG_RTK_GNSS_UART_NUM) {
        return fw::Result::kInvalidState;
    }
    // UART 号已验证，缓冲区常量均小于 INT_MAX，三处转换安全。
    const auto port = static_cast<uart_port_t>(pins.uart);
    const esp_err_t drvErr = uart_driver_install(port, static_cast<int>(kUartRxBufBytes),
                                                 static_cast<int>(kUartTxBufBytes), 0, nullptr, 0);
    if (drvErr != ESP_OK) {
        return toResult(rtos::fromEspErr(drvErr));
    }
    uart_installed_ = true;
    const uart_config_t uartCfg{
        // 早期校验后的首选值最高 921600，转换为 IDF 的 int 无损。
        .baud_rate = static_cast<int>(bauds_.front()),
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0U,
        .rx_glitch_filt_thresh = 0U,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    const esp_err_t cfgErr = uart_param_config(port, &uartCfg);
    if (cfgErr != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(cfgErr)));
    }
    const esp_err_t pinErr =
        uart_set_pin(port, pins.txGpio, pins.rxGpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (pinErr != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(pinErr)));
    }
    if (pins.powerGpio < 0) {
        return fw::Result::kOk;
    }
    // 配置表中的电源脚是已核实的输出 GPIO，枚举与位掩码转换安全。
    const auto powerPin = static_cast<gpio_num_t>(pins.powerGpio);
    const uint32_t offLevel = pins.powerActiveHigh ? 0U : 1U;
    const esp_err_t levelErr = gpio_set_level(powerPin, offLevel);
    if (levelErr != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(levelErr)));
    }
    gpio_config_t powerCfg{};
    powerCfg.pin_bit_mask = 1ULL << static_cast<unsigned>(pins.powerGpio);
    powerCfg.mode = GPIO_MODE_OUTPUT;
    powerCfg.pull_up_en = GPIO_PULLUP_DISABLE;
    powerCfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    powerCfg.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t gpioErr = gpio_config(&powerCfg);
    if (gpioErr != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(gpioErr)));
    }
    return fw::Result::kOk;
}

fw::Status GnssModule::stopBoot() noexcept
{
    if (!uart_installed_) {
        return fw::Status::kOk;
    }
    // Kconfig UART 号限制为 0..2；与安装时同一端口。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t err = uart_driver_delete(port);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    uart_installed_ = false;
    initialized_ = false;
    return fw::Status::kOk;
}

fw::Result GnssModule::initFailed(fw::Result original) noexcept
{
    const auto cleanup = stopBoot();
    return fw::isOk(cleanup) ? original : toResult(cleanup);
}

fw::Result GnssModule::start() noexcept
{
    if (!initialized_) {
        return fw::Result::kInvalidState;
    }
    const uint32_t now = rtos::nowMs();
    controller().start(now);
    pollController(now);
    publishStatus(now);
    return fw::Result::kOk;
}

void GnssModule::wait(uint32_t maxMs) noexcept
{
    if (!initialized_) {
        vTaskDelay(pdMS_TO_TICKS(maxMs));
        return;
    }
    std::array<uint8_t, kUartReadChunkBytes> buf{};
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int n = uart_read_bytes(port, buf.data(), buf.size(), pdMS_TO_TICKS(maxMs));
    if (n <= 0) {
        last_rx_bytes_ = 0U;
        return;
    }
    const auto count = static_cast<size_t>(n);
    last_rx_bytes_ = count;
    uart_rx_bytes_ += static_cast<uint32_t>(count);
    const demux::Error err = streamDemux().feed(std::span<const uint8_t>{buf.data(), count});
    if (err != demux::Error::kOk) {
        traceGnss(GnssTrace::kState, static_cast<uint32_t>(controller().state()),
                  static_cast<uint32_t>(err));
    }
    const auto &dstats = streamDemux().stats();
    if (dstats.unknownBytes > prev_unknown_bytes_) {
        FW_LOGD(fw::TraceCat::kGnss, "<rx> %lu unknown bytes dropped",
                static_cast<unsigned long>(dstats.unknownBytes - prev_unknown_bytes_));
        prev_unknown_bytes_ = dstats.unknownBytes;
    }
}

void GnssModule::publishPendingNvs() noexcept
{
    const bool baudPending = baud_deferred_ || baud_save_in_flight_ ||
                             (active_maintenance_ == ::gnss::Maintenance::kBaud &&
                              (controller().state() == ::gnss::State::kMaintenance ||
                               controller().stats().maintenanceSeq != completed_maintenance_seq_));
    const bool pendingNvs = (pending_apply_ != PendingApply::kNone) || (ppp_ticket_seq_ != 0U) ||
                            (sat_ticket_seq_ != 0U) || gga_gate_save_pending_ || baudPending;
    ctx().data.gnssPendingNvs.store(pendingNvs, std::memory_order_release);
}

/** @brief 推进 GNSS 状态机与数据通路。 @param nowMs: 单调时钟。 @retval true UART 或管道可能仍有积压。 */
bool GnssModule::poll(uint32_t nowMs) noexcept
{
    if (!initialized_) {
        return false;
    }
    std::span<const uint32_t> timedOutKeys{};
    if (valset_tracker_.pollTimeout(nowMs, timedOutKeys)) {
        logValsetIssue("timeout", timedOutKeys);
    }
    tickGgaGateSave(nowMs, false);
    const bool seqBusy = pollPppAndSat(nowMs);
    const bool ctrlReady = !seqBusy && ensureRtcmFrameBoundary(nowMs);
    if (ctrlReady) {
        pollController(nowMs);
        tickPendingApply(nowMs);
        tickMaintenance(nowMs);
        pollWorkMode(nowMs);
    }
    if (ctrlReady && !uniqid_polled_ && (controller().state() == ::gnss::State::kRunning)) {
        pollUbloxBoardSn(nowMs);
    }
    publishPendingNvs();
    const bool ctrlNeedsBoundary = (controller().state() != ::gnss::State::kRunning) ||
                                   (pending_apply_ != PendingApply::kNone);
    const bool stopAtBoundary = (sat_seq_.needsTx() || ppp_seq_.needsTx() || ctrlNeedsBoundary) &&
                                !rtcm_framer_.atFrameBoundary();
    const bool txProgress = pumpRtcmPipe(stopAtBoundary);

    const auto curState = controller().state();
    if ((curState != last_published_state_) || ((nowMs - last_status_ms_) >= kStatusPeriodMs)) {
        publishStatus(nowMs);
    }

    const bool hadFullRx = (last_rx_bytes_ == kUartReadChunkBytes);
    last_rx_bytes_ = 0U;
    const bool allowTx = (curState == ::gnss::State::kRunning) ||
                         (curState == ::gnss::State::kConfiguring) ||
                         (curState == ::gnss::State::kProbing);
    const bool hasMoreTx = (rtcm_tx_len_ > rtcm_tx_off_) || (ctx().rtcmPipe.available() > 0U);
    return hadFullRx || (allowTx && txProgress && hasMoreTx);
}

bool GnssModule::ensureRtcmFrameBoundary(uint32_t nowMs) noexcept
{
    constexpr uint32_t kRtcmFrameStallMs = 200U;
    while (!rtcm_framer_.atFrameBoundary()) {
        const bool progressed = pumpRtcmPipe(true);
        if (rtcm_framer_.atFrameBoundary()) {
            rtcm_frame_stall_armed_ = false;
            return true;
        }
        if (progressed) {
            rtcm_frame_stall_armed_ = false;
            continue;
        }
        if ((rtcm_tx_len_ > rtcm_tx_off_) || (ctx().rtcmPipe.available() > 0U)) {
            rtcm_frame_stall_armed_ = false;
            return false;
        }
        if (!rtcm_frame_stall_armed_) {
            rtcm_frame_stall_armed_ = true;
            rtcm_frame_stall_ms_ = nowMs;
            return false;
        }
        if ((nowMs - rtcm_frame_stall_ms_) >= kRtcmFrameStallMs) {
            rtcm_framer_.reset();
            rtcm_frame_stall_armed_ = false;
            return true;
        }
        return false;
    }
    rtcm_frame_stall_armed_ = false;
    return true;
}

void GnssModule::discardRtcmPipe() noexcept
{
    if (rtcm_tx_len_ > rtcm_tx_off_) {
        ctx().rtcmPipe.noteDiscarded(rtcm_tx_len_ - rtcm_tx_off_);
        rtcm_tx_len_ = 0U;
        rtcm_tx_off_ = 0U;
    }
    ctx().rtcmPipe.discard();
    rtcm_framer_.reset();
    rtcm_frame_stall_armed_ = false;
}

void GnssModule::handleRtcmWriteError(int written, ::gnss::State st) noexcept
{
    const size_t lost = rtcm_tx_len_ - rtcm_tx_off_;
    rtcm_tx_off_ = 0U;
    rtcm_tx_len_ = 0U;
    rtcm_framer_.reset();
    rtcm_frame_stall_armed_ = false;
    ctx().rtcmPipe.noteWriteError(lost);
    traceGnss(GnssTrace::kState, static_cast<uint32_t>(st),
              static_cast<uint32_t>(fw::Result::kIoError));
    FW_LOGW(fw::TraceCat::kGnss, "<rtcm> UART write error (%d), dropped %lu pending bytes", written,
            static_cast<unsigned long>(lost));
}

bool GnssModule::pumpRtcmPipe(bool stopAtFrameBoundary) noexcept
{
    const auto st = controller().state();
    const bool finishMidFrame = stopAtFrameBoundary && !rtcm_framer_.atFrameBoundary();
    const bool baseBlocksRx = work_mode_.has_value() && work_mode_->isBaseActive();
    const RtcmTxGate gate =
        classifyRtcmTxGate(st, active_maintenance_, baseBlocksRx, finishMidFrame);
    if (gate == RtcmTxGate::kHold) {
        return false;
    }
    if (gate == RtcmTxGate::kDiscard) {
        discardRtcmPipe();
        return false;
    }
    if (stopAtFrameBoundary && rtcm_framer_.atFrameBoundary()) {
        return false;
    }
    if (rtcm_tx_off_ >= rtcm_tx_len_) {
        rtcm_tx_len_ = ctx().rtcmPipe.read(rtcm_tx_buf_);
        rtcm_tx_off_ = 0U;
    }
    if (rtcm_tx_off_ >= rtcm_tx_len_) {
        ctx().rtcmPipe.setPending(0U, false);
        return false;
    }
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    auto pending =
        std::span<const uint8_t>{rtcm_tx_buf_}.subspan(rtcm_tx_off_, rtcm_tx_len_ - rtcm_tx_off_);
    if (stopAtFrameBoundary) {
        const size_t finishBytes = rtcm_framer_.bytesToFinishCurrentFrame(pending);
        if (finishBytes == 0U) {
            return false;
        }
        pending = pending.first(finishBytes);
    }
    const int written = uart_write_bytes(port, pending.data(), pending.size());
    if (written > 0) {
        const auto bytes = static_cast<size_t>(written);
        rtcm_framer_.onBytesWritten(pending.first(bytes));
        rtcm_frame_stall_armed_ = false;
        rtcm_tx_off_ += bytes;
        if (rtcm_tx_off_ >= rtcm_tx_len_) {
            rtcm_tx_off_ = 0U;
            rtcm_tx_len_ = 0U;
        }
        uart_tx_bytes_ += static_cast<uint32_t>(bytes);
        ctx().rtcmPipe.noteWritten(bytes);
        ctx().rtcmPipe.setPending(rtcm_tx_len_ - rtcm_tx_off_, false);
        return true;
    }
    if (written == 0) {
        // 驱动 TX 环暂满：保留块与偏移，下轮继续，不丢块也不重读管道。
        ctx().rtcmPipe.setPending(rtcm_tx_len_ - rtcm_tx_off_, true);
        return false;
    }
    handleRtcmWriteError(written, st);
    return false;
}

void GnssModule::publishStatus(uint32_t nowMs) noexcept
{
    GnssStatus st{};
    st.enabled = initialized_;
    st.state = controller().state();
    const auto &det = controller().detected();
    if (det.receiver != nullptr) {
        st.family = det.receiver->family();
        st.model = det.probe.model;
        st.fwVersion = det.probe.fwVersion;
        st.caps = det.caps;
    }
    st.hostBaud = host_baud_;
    st.ctrl = controller().stats();
    // 自动保存可能先于控制台的下一次轮询完成，保留上次手动操作的结果。
    st.ctrl.maintenanceSeq = console_maintenance_seq_;
    st.ctrl.maintenance = console_maintenance_;
    st.ctrl.maintenanceResult = console_maintenance_result_;
    st.outputSeq = st.ctrl.outputSeq;
    st.appliedMask = st.ctrl.appliedMask;
    st.unsupportedMask = st.ctrl.unsupportedMask;
    st.failedMask = st.ctrl.failedMask;
    st.lastApply = last_apply_;
    st.applySeq = apply_seq_;
    static_assert(std::tuple_size_v<decltype(st.rxPeriodMs)> == data::kGnssAppMsgCount);
    const auto &plan = controller().outputPlan();
    for (size_t i = 0U; i < st.rxPeriodMs.size(); ++i) {
        st.rxPeriodMs[i] = plan.periodMs[i];
    }
    // App 可见的支持位 = 驱动声明的语句位（含 bit 6 GSV，供默认 NMEA 直传取交集）& 未报不支持 & 未失败（ADR-0012 §2.5 / P1 Rev B）。
    if (det.receiver != nullptr) {
        constexpr uint32_t kOutBits = (1U << static_cast<uint32_t>(::gnss::kOutputMsgCount)) - 1U;
        const uint32_t declared = det.receiver->outputMask(det.probe.model) & kOutBits;
        st.supportedMask = declared & ~st.ctrl.unsupportedMask & ~st.ctrl.failedMask;
    }
    st.nmea = parser().stats();
    st.uartRxBytes = uart_rx_bytes_;
    st.uartTxBytes = uart_tx_bytes_;
    const auto &dstats = streamDemux().stats();
    st.demuxUnknown = dstats.unknownBytes;
    st.demuxResyncs = dstats.resyncs;
    st.pipe = ctx().rtcmPipe.stats();

    ctx().data.gnssRateHz.store(ensureRoverCfg().rateHz, std::memory_order_release);
    ctx().data.gnssStatus.write(st);
    publishCaps();
    last_status_ms_ = nowMs;
    last_published_state_ = st.state;
}

void GnssModule::registerDecoders(std::span<const ::gnss::Receiver *const> receivers) noexcept
{
    decoder_count_ = 0U;
    addUniqueDecoder(decoders_, decoder_count_, &demux::nmea::kDecoder);
    addUniqueDecoder(decoders_, decoder_count_, &demux::rtcm3::kDecoder);
    // 驱动私有解码器（UBX、Unicore # 帧、$command 应答等）由各驱动自己声明，顺序即探测优先级。
    for (const ::gnss::Receiver *receiver : receivers) {
        if (receiver == nullptr) {
            continue;
        }
        for (const demux::Decoder *dec : receiver->decoders()) {
            addUniqueDecoder(decoders_, decoder_count_, dec);
        }
    }
}

void GnssModule::initBaudCandidates() noexcept
{
    const std::array<uint32_t, kMaxBaudCandidates> candidatesInOrder{
        preferred_baud_ == 0U ? CONFIG_RTK_GNSS_DEFAULT_BAUD : preferred_baud_,
        115200U,
        460800U,
        921600U,
        230400U,
        38400U,
        9600U,
        19200U,
        57600U,
    };
    baud_count_ = 0U;
    for (const uint32_t baud : candidatesInOrder) {
        const auto candidates = std::span{bauds_}.first(baud_count_);
        if (std::find(candidates.begin(), candidates.end(), baud) == candidates.end()) {
            bauds_[baud_count_] = baud;
            ++baud_count_;
        }
    }
}

} // namespace app
