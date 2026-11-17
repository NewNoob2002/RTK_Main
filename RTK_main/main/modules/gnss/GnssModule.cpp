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

using detail::addUniqueDecoder;
using detail::toResult;
using detail::traceGnssSub;

/**
 * @brief 构造子对象并回填未被 NVS 覆盖的默认配置
 * @retval kOk: 子对象已就绪；其他: 保留给上位调用方
 * @note 仅 IO 任务启动期调用；emplace 不分配堆（成员已在 .bss）。
 */
fw::Result GnssModule::initObjects() noexcept
{
    sink_.emplace(this);
    demux_.emplace();
    parser_.emplace();
    controller_.emplace();
    timing_.emplace();
    // 成员默认零初始化以保证 AppContext 落入 .bss；未从 NVS 加载的项在此回填默认配置。
    auto &cfg = ensureRoverCfg();
    if (!rate_loaded_) {
        cfg.rateHz = static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ);
    }
    if (!gsv_loaded_) {
        cfg.gsv = true;
    }
    cfg.port = static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT);
    if (!out_cfg_.isValid()) {
        out_cfg_ = data::GnssOutConfig::defaults();
    }
    return fw::Result::kOk;
}

/**
 * @brief 起 UART、RTCM 管道、解码器表与分流器，并算出候选波特率
 * @retval kOk: 通路就绪；其他: 已回滚并记录失败原因
 * @note 仅启动期单线程调用；UART 安装可阻塞，非 ISR。
 */
fw::Result GnssModule::initPipelines() noexcept
{
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
    initBaudCandidates();
    return fw::Result::kOk;
}

/**
 * @brief 用默认输出计划启动控制器并发布初始状态
 * @retval kOk: 控制器已启动；其他: 已回滚并记录失败原因
 * @note 仅启动期单线程调用；非阻塞、不分配堆。
 */
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
    controller().setOutputPlan(data::makeOutputPlan(
        out_cfg_, cfg.rateHz, cfg.gsv, static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT), 0U));
    if (!controller().init(receivers, std::span<const uint32_t>{bauds_.data(), baud_count_}, cfg,
                           controller().outputPlan(), hint_, clock)) {
        return initFailed(fw::Result::kInvalidState);
    }
    host_baud_ = CONFIG_RTK_GNSS_DEFAULT_BAUD;
    last_state_ = controller().state();
    last_published_state_ = last_state_;
    prev_ctrl_stats_ = controller().stats();
    ctx().data.gnssOutCfg.write(out_cfg_);
    traceGnssSub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
    return fw::Result::kOk;
}

/**
 * @brief 初始化 GNSS 模块（UART、分流、控制器）
 * @retval kOk: 已就绪；其他: 失败原因（已回滚）
 * @note 仅 IO 任务启动期调用；可阻塞（UART 安装），非 ISR。
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
    // Kconfig UART 号限制为 0..2，缓冲区常量均小于 INT_MAX，三处转换安全。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t drvErr = uart_driver_install(port, static_cast<int>(kUartRxBufBytes),
                                                 static_cast<int>(kUartTxBufBytes), 0, nullptr, 0);
    if (drvErr != ESP_OK) {
        return toResult(rtos::fromEspErr(drvErr));
    }
    uart_installed_ = true;
    const uart_config_t uartCfg{
        .baud_rate = CONFIG_RTK_GNSS_DEFAULT_BAUD,
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
    const esp_err_t pinErr = uart_set_pin(port, CONFIG_RTK_GNSS_TX_GPIO, CONFIG_RTK_GNSS_RX_GPIO,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (pinErr != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(pinErr)));
    }
#if CONFIG_RTK_GNSS_POWER_GPIO >= 0
    // Kconfig GPIO 限制为 0..39，枚举转换与 64 位移位安全。
    const auto powerPin = static_cast<gpio_num_t>(CONFIG_RTK_GNSS_POWER_GPIO);
    const uint32_t offLevel = (CONFIG_RTK_GNSS_POWER_ACTIVE_HIGH != 0) ? 0U : 1U;
    const esp_err_t levelErr = gpio_set_level(powerPin, offLevel);
    if (levelErr != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(levelErr)));
    }
    gpio_config_t powerCfg{};
    powerCfg.pin_bit_mask = 1ULL << static_cast<unsigned>(CONFIG_RTK_GNSS_POWER_GPIO);
    powerCfg.mode = GPIO_MODE_OUTPUT;
    powerCfg.pull_up_en = GPIO_PULLUP_DISABLE;
    powerCfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    powerCfg.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t gpioErr = gpio_config(&powerCfg);
    if (gpioErr != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(gpioErr)));
    }
#endif
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

/**
 * @brief  等 UART 数据或命令事件，最长 maxMs
 * @param  maxMs: 最长等待（ms）
 */
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

bool GnssModule::poll(uint32_t nowMs) noexcept
{
    if (!initialized_) {
        return false;
    }
    pollController(nowMs);
    tickPendingApply(nowMs);
    const bool txProgress = pumpRtcmPipe();

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

bool GnssModule::pumpRtcmPipe() noexcept
{
    const auto st = controller().state();
    const bool allowTx = (st == ::gnss::State::kRunning) || (st == ::gnss::State::kConfiguring) ||
                         (st == ::gnss::State::kProbing);
    if (!allowTx) {
        if (rtcm_tx_len_ > rtcm_tx_off_) {
            ctx().rtcmPipe.noteDiscarded(rtcm_tx_len_ - rtcm_tx_off_);
            rtcm_tx_len_ = 0U;
            rtcm_tx_off_ = 0U;
        }
        ctx().rtcmPipe.discard();
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
    const auto pending =
        std::span<const uint8_t>{rtcm_tx_buf_}.subspan(rtcm_tx_off_, rtcm_tx_len_ - rtcm_tx_off_);
    const int written = uart_write_bytes(port, pending.data(), pending.size());
    if (written > 0) {
        const auto bytes = static_cast<size_t>(written);
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
    const size_t lost = rtcm_tx_len_ - rtcm_tx_off_;
    rtcm_tx_off_ = 0U;
    rtcm_tx_len_ = 0U;
    ctx().rtcmPipe.noteWriteError(lost);
    traceGnss(GnssTrace::kState, static_cast<uint32_t>(st),
              static_cast<uint32_t>(fw::Result::kIoError));
    FW_LOGW(fw::TraceCat::kGnss, "<rtcm> UART write error (%d), dropped %lu pending bytes", written,
            static_cast<unsigned long>(lost));
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
    // App 可见的支持位 = 驱动声明的语句位 & 本轮未报不支持 & 未失败（ADR-0012 §2.5）。
    if (det.receiver != nullptr) {
        constexpr uint32_t kAppBits = (1U << static_cast<uint32_t>(data::kGnssAppMsgCount)) - 1U;
        const uint32_t declared = det.receiver->outputMask(det.probe.model) & kAppBits;
        st.supportedMask = declared & ~st.ctrl.unsupportedMask & ~st.ctrl.failedMask;
    }
    st.nmea = parser().stats();
    st.uartRxBytes = uart_rx_bytes_;
    st.uartTxBytes = uart_tx_bytes_;
    const auto &dstats = streamDemux().stats();
    st.demuxUnknown = dstats.unknownBytes;
    st.demuxResyncs = dstats.resyncs;
    st.pipe = ctx().rtcmPipe.stats();

    ctx().data.gnssStatus.write(st);
    last_status_ms_ = nowMs;
    last_published_state_ = st.state;
}

/**
 * @brief 登记基础解码器与各驱动私有解码器（顺序即探测优先级）
 * @param receivers: 编译期启用的接收机实例表（可为空表）
 * @note 仅 init() 调用；重复登记同一解码器会被跳过。
 */
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

/**
 * @brief 生成探测用的候选波特率表：默认值优先，其后按从快到慢回退
 * @note 仅 init() 调用；重复值只保留一次，顺序即尝试顺序。
 */
void GnssModule::initBaudCandidates() noexcept
{
    constexpr std::array<uint32_t, kMaxBaudCandidates> kCandidateBauds{{
        CONFIG_RTK_GNSS_DEFAULT_BAUD,
        115200U,
        460800U,
        921600U,
        230400U,
        38400U,
        9600U,
    }};
    baud_count_ = 0U;
    for (const uint32_t baud : kCandidateBauds) {
        const auto candidates = std::span{bauds_}.first(baud_count_);
        if (std::find(candidates.begin(), candidates.end(), baud) == candidates.end()) {
            bauds_[baud_count_] = baud;
            ++baud_count_;
        }
    }
}

} // namespace app
