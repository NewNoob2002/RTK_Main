// GNSS 模块实现（ADR-0010 §2.7）：UART 收发、StreamDemux 分流、NMEA 解析、控制器轮询、RTCM 下发与 NVS 绑定。
// 线程：init() 启动前单线程；start()/wait()/poll()/onCommand 仅 kIo 任务；keys/load/save 由 storage 与 kIo 调用。
// 均非 ISR；不分配堆。实时路径只用 GnssTrace；文本诊断经 FW_LOG*（ADR-0009 §2.12）：状态变化 I，逐帧/逐句 D。

#include "modules/gnss/GnssModule.hpp"

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

namespace {

constexpr uint32_t kByteMask = 0xFFU;
constexpr size_t kMaxActionsPerPoll = 16U;
constexpr uint32_t kBaudMin = 1200U;
constexpr uint32_t kBaudMax = 2000000U;
/** 输出/频率变更等待控制器落地的上限（毫秒）。 */
constexpr uint32_t kApplyTimeoutMs = 5000U;
/** 读回行缓冲：UnicoreResp 单帧上限 256 B，正文必小于它（ADR-0012 §2.7）。 */
constexpr size_t kReadbackLineBytes = 256U;

// kGnss 子事件包装：枚举 → uint8_t 载荷、session 恒 0（ADR-0009），参数求值昂贵时调用方先用 fw::traceOn()。
void traceGnssSub(GnssSubTrace ev, uint32_t v0, uint32_t v1) noexcept
{
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(ev), 0U, v0, v1);
}

/** 拒绝原因在文本日志中的短名（explain 为空时的兜底，§2.11）。 */
const char *rejectCauseName(demux::RejectCause cause) noexcept
{
    switch (cause) {
    case demux::RejectCause::kTooLong:
        return "frame too long";
    case demux::RejectCause::kContract:
        return "invalid length field";
    case demux::RejectCause::kAbort:
        return "aborted";
    case demux::RejectCause::kStep:
    default:
        return "rejected";
    }
}

/** `GNSS ` + 帧标识 + 长度文本的整行缓冲。 */
constexpr size_t kLogTagBytes = 48U;
/** 拒绝原因文本缓冲（explain 输出在此缓冲内截断）。 */
constexpr size_t kRejectReasonBytes = 96U;

bool isValidRateHz(uint8_t hz) noexcept
{
    constexpr std::array<uint8_t, 5> kRates{1U, 2U, 5U, 10U, 20U};
    return std::find(kRates.begin(), kRates.end(), hz) != kRates.end();
}

fw::Result toResult(fw::Status s) noexcept
{
    switch (s) {
    case fw::Status::kOk:
        return fw::Result::kOk;
    case fw::Status::kInvalidArg:
        return fw::Result::kInvalidArg;
    case fw::Status::kNoSpace:
        return fw::Result::kNoSpace;
    case fw::Status::kTimeout:
        return fw::Result::kTimeout;
    case fw::Status::kNotFound:
        return fw::Result::kNotFound;
    case fw::Status::kNotSupported:
        return fw::Result::kNotSupported;
    case fw::Status::kBusy:
        return fw::Result::kBusy;
    case fw::Status::kInvalidState:
        return fw::Result::kInvalidState;
    case fw::Status::kCorrupt:
    case fw::Status::kIoError:
    default:
        return fw::Result::kIoError;
    }
}

uint32_t readU32(std::span<const uint8_t> in) noexcept
{
    uint32_t out = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        // in.size() >= 4 已在调用前校验；i < 4 左移不会超出 32 位。
        out |= static_cast<uint32_t>(in[i]) << (8U * i);
    }
    return out;
}

void writeU32(uint32_t value, std::span<uint8_t> out) noexcept
{
    for (size_t i = 0U; i < 4U; ++i) {
        // out.size() >= 4 已在调用前校验；取字节无窄化丢失。
        out[i] = static_cast<uint8_t>((value >> (8U * i)) & kByteMask);
    }
}

void addUniqueDecoder(std::array<const demux::Decoder *, demux::kMaxDecoders> &out, size_t &count,
                      const demux::Decoder *dec) noexcept
{
    if ((dec == nullptr) || (count >= out.size())) {
        return;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (out[i] == dec) {
            return;
        }
    }
    out[count] = dec;
    ++count;
}

} // namespace

void GnssModule::DemuxSink::onFrame(const demux::Decoder &decoder, uint8_t /*index*/,
                                    std::span<const uint8_t> frame) noexcept
{
    if (owner_ != nullptr) {
        owner_->handleFrame(decoder, frame);
    }
}

void GnssModule::DemuxSink::onReject(const demux::Decoder &decoder, uint8_t /*index*/,
                                     std::span<const uint8_t> prefix, demux::RejectCause cause,
                                     bool last) noexcept
{
    if (owner_ != nullptr) {
        owner_->handleReject(decoder, prefix, cause, last);
    }
}

::gnss::Controller &GnssModule::controller() noexcept
{
    if (!controller_.has_value()) {
        std::abort();
    }
    return *controller_;
}

::gnss::NmeaParser &GnssModule::parser() noexcept
{
    if (!parser_.has_value()) {
        std::abort();
    }
    return *parser_;
}

demux::Demux &GnssModule::streamDemux() noexcept
{
    if (!demux_.has_value()) {
        std::abort();
    }
    return *demux_;
}

fw::Result GnssModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kOk;
    }
    if ((context_ == nullptr) || (demux_buf_.size() < kDemuxBufBytes)) {
        return fw::Result::kInvalidState;
    }
    auto &sink = sink_.emplace(this);
    demux_.emplace();
    parser_.emplace();
    controller_.emplace();
    const auto &timing = timing_.emplace();
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

    const auto uartResult = initUart();
    if (!fw::isOk(uartResult)) {
        return uartResult;
    }

    const fw::Result pipeRes = toResult(ctx().rtcmPipe.init());
    if (!fw::isOk(pipeRes)) {
        return initFailed(pipeRes);
    }

    decoder_count_ = 0U;
    addUniqueDecoder(decoders_, decoder_count_, &demux::nmea::kDecoder);
    addUniqueDecoder(decoders_, decoder_count_, &demux::rtcm3::kDecoder);
    // 驱动私有解码器（UBX、Unicore # 帧、$command 应答等）由各驱动自己声明，顺序即探测优先级。
    const std::span<const ::gnss::Receiver *const> receivers = ::gnss::enabledReceivers();
    for (const ::gnss::Receiver *receiver : receivers) {
        if (receiver == nullptr) {
            continue;
        }
        for (const demux::Decoder *dec : receiver->decoders()) {
            addUniqueDecoder(decoders_, decoder_count_, dec);
        }
    }

    const demux::Error demuxErr = streamDemux().init(
        std::span<const demux::Decoder *const>{decoders_.data(), decoder_count_}, demux_buf_, sink);
    if (demuxErr != demux::Error::kOk) {
        return initFailed(fw::Result::kInvalidState);
    }

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

    // 输出计划是唯一来源（ADR-0012 §2.4）：先按默认值给一份，探测到型号后再按 caps 重算。
    controller().setOutputPlan(data::makeOutputPlan(
        out_cfg_, cfg.rateHz, cfg.gsv, static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT), 0U));
    if (!controller().init(receivers, std::span<const uint32_t>{bauds_.data(), baud_count_}, cfg,
                           controller().outputPlan(), hint_, timing)) {
        return initFailed(fw::Result::kInvalidState);
    }

    host_baud_ = CONFIG_RTK_GNSS_DEFAULT_BAUD;
    last_state_ = controller().state();
    last_published_state_ = last_state_;
    prev_ctrl_stats_ = controller().stats();
    ctx().data.gnssOutCfg.write(out_cfg_);
    traceGnssSub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
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

/**
 * @brief 处理一条应用层指令（kProbe/kReset/kSetRate/kSend/kSetOut/kReadback）
 * @param cmd: 指令（verb 为 GnssVerb 或 GnssSubVerb）
 * @param result: 输出，执行结果；不支持的子命令返回 kNotSupported
 * @note 在 IO 执行器任务里调用，不阻塞；读回子命令只发起请求，结果由 AppData 发布
 */
void GnssModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!initialized_) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t now = rtos::nowMs();
    switch (static_cast<GnssVerb>(cmd.verb)) {
    case GnssVerb::kProbe:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdProbe(now, result);
        return;
    case GnssVerb::kReset:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdReset(now, result);
        return;
    case GnssVerb::kSetRate:
        cmdSetRate(cmd, now, result);
        return;
    case GnssVerb::kSend:
        sendRaw(cmd, result);
        return;
    default:
        break;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetOut) {
        cmdSetOut(cmd, now, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kReadback) {
        startReadback(now, result);
        return;
    }
    result.status = fw::Result::kNotSupported;
}

/**
 * @brief  gnss probe：标记人工重探，清空最近定位并让控制器重新探测
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kOk）
 * @note   仅 kIo 调用，非阻塞。
 */
void GnssModule::cmdProbe(uint32_t nowMs, fw::CommandResult &result) noexcept
{
    // 人工重探：清空最近定位，避免旧定位在重探期间被当成当前值发布。
    manual_reprobe_ = true;
    parser().reset();
    ctx().data.gnssFix.write(::gnss::Fix{});
    controller().requestReprobe(nowMs);
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

/**
 * @brief  gnss reset：清空最近定位后停再启动控制器
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kOk）
 * @note   仅 kIo 调用，非阻塞。
 */
void GnssModule::cmdReset(uint32_t nowMs, fw::CommandResult &result) noexcept
{
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

/**
 * @brief  gnss rate：两阶段提交更新输出频率（ADR-0012 §2.4）
 * @param  cmd: 指令，args[0] 必须是支持的频率
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
 * @note   仅 kIo 调用，非阻塞；NVS 写只在非 kRunning 的直落盘路径发生。
 */
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
    if (controller().state() != ::gnss::State::kRunning) {
        // 与 kSetOut 一致：没有运行中的接收机可确认，直接落盘，下次配置完成时按新频率执行。
        const uint8_t prevRate = ensureRoverCfg().rateHz;
        ensureRoverCfg().rateHz = cmd.args[0];
        const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kConfigKeys);
        if (!fw::isOk(saveRes)) {
            ensureRoverCfg().rateHz = prevRate;
            result.status = saveRes;
            return;
        }
        controller().setOutputPlan(makePlan());
        last_apply_ = GnssApply::kOk;
        ++apply_seq_;
        publishStatus(nowMs);
        result.status = fw::Result::kOk;
        return;
    }
    // 两阶段提交（ADR-0012 §2.4）：先暂存并重配，成功后才写 NVS；失败由 tickPendingApply 回滚。
    pending_prev_rate_ = ensureRoverCfg().rateHz;
    ensureRoverCfg().rateHz = cmd.args[0];
    pending_apply_ = PendingApply::kRate;
    pending_seq_ = controller().stats().outputSeq + 1U;
    pending_deadline_ms_ = nowMs + kApplyTimeoutMs;
    controller().setOutputPlan(makePlan());
    controller().requestReconfigure(nowMs);
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

/**
 * @brief  gnss setout：两阶段提交更新输出表
 * @param  cmd: 指令，args 为 GnssOutConfig 编码块
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
 * @note   仅 kIo 调用，非阻塞。
 */
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
    const data::GnssOutConfig prev = out_cfg_;
    out_cfg_ = next;
    ctx().data.gnssOutCfg.write(out_cfg_);
    traceGnssSub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
    if (controller().state() != ::gnss::State::kRunning) {
        // 未进入 kRunning 时没有可下发的输出阶段：直接落盘，下次配置完成时按新表重算计划。
        const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kConfigKeys);
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
        result.status = fw::Result::kOk;
        return;
    }
    pending_prev_out_ = prev;
    pending_apply_ = PendingApply::kOut;
    pending_seq_ = controller().stats().outputSeq + 1U;
    pending_deadline_ms_ = nowMs + kApplyTimeoutMs;
    controller().setOutputPlan(makePlan());
    static_cast<void>(controller().requestOutput(controller().outputPlan(), nowMs));
    pollController(nowMs);
    publishStatus(nowMs);
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

/**
 * @brief  处理分流器交付的一帧：喂控制器、应用输出，并记 D 级帧日志
 * @param  decoder: 识别出该帧的解码器
 * @param  frame: 完整帧字节
 */
void GnssModule::handleFrame(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept
{
    // 文本日志不经中断路径：本函数只由 kIo 任务的 feed() 调用（ADR-0009 §2.11）。
    std::array<char, kLogTagBytes> tag{};
    controller().receiver().frameTag(decoder, frame, tag);
    FW_LOGD(fw::TraceCat::kGnss, "<rx> %s, %lu bytes", tag.data(),
            static_cast<unsigned long>(frame.size()));
    fw::tlogHex(fw::TraceCat::kGnss, frame);

    const uint32_t now = rtos::nowMs();
    // 读回窗口内先把这一行取走（只在 kIo 任务里写交接缓冲），再交给状态机计数与计时。
    if (controller().state() == ::gnss::State::kReadback) {
        appendReadbackLine(decoder, frame);
    }
    controller().onFrame(decoder, frame, now);
    if (&decoder == &demux::nmea::kDecoder) {
        const auto res = parser().feed(frame, now);
        if (res == ::gnss::NmeaResult::kFix) {
            ctx().data.gnssFix.write(parser().fix());
        } else if (res == ::gnss::NmeaResult::kSatsReady) {
            // GGA 触发卫星轮发布时已在同一次 feed() 中更新 fix_（G-A §4.1），须同步刷新定位槽。
            ctx().data.gnssFix.write(parser().fix());
            ctx().data.gnssSats.write(parser().sats());
        }
    }

    // ADR-0011 §5：6 类可订阅语句同步更新 6 KiB 最新帧缓存，并按会话掩码与周期写入 App typed FanoutRing。
    {
        // 分类交给驱动（ADR-0012 §2.6）：基类覆盖 5 类 NMEA，Unicore 另加 #SATSINFOA/#HEADINGA。
        const ::gnss::OutputMsg cls = controller().receiver().classifyFrame(decoder, frame);
        const auto idx = static_cast<size_t>(static_cast<uint8_t>(cls));
        if ((cls != ::gnss::OutputMsg::kNone) && (idx < data::kGnssAppMsgCount)) {
            const auto typeIdx = static_cast<uint8_t>(idx);
            if (!ctx().data.gnssLatest.update(typeIdx, frame)) {
                traceGnssSub(GnssSubTrace::kAppDrop, 2U, typeIdx);
            }
            uint32_t activeMask = 0U;
            std::array<uint32_t, data::kGnssAppMsgCount> periodsMs{};
            ctx().data.gnssAppSub.readSubscription(activeMask, periodsMs);
            if ((activeMask & (1U << typeIdx)) == 0U) {
                // 无订阅者：FanoutRing::write 只计 noSubscriber，不写入环形缓冲；闸门复位，重新订阅后首句立即推送。
                static_cast<void>(ctx().data.gnssAppRing.write(typeIdx, frame));
                push_gates_[typeIdx].reset();
            } else {
                const uint32_t period = periodsMs[typeIdx];
                auto &gate = push_gates_[typeIdx];
                if (!gate.due(now, period)) {
                    ctx().data.gnssAppSub.addSkipped(typeIdx);
                } else {
                    const fw::Result wres = ctx().data.gnssAppRing.write(typeIdx, frame);
                    if (fw::isOk(wres)) {
                        gate.onPushed(now, period);
                    } else if (wres == fw::Result::kInvalidArg) {
                        ctx().data.gnssAppSub.addRingOversize(typeIdx);
                        traceGnssSub(GnssSubTrace::kAppDrop, 0U, typeIdx);
                    }
                }
            }
        }
    }
}

/**
 * @brief  分流器拒收字节：记 D 级日志与可选 hex 转储
 * @param  decoder: 拒收的解码器
 * @param  prefix: 被拒收的字节
 * @param  cause: 拒收原因
 * @param  last: 是否为本次重同步的最后一个拒收
 */
void GnssModule::handleReject(const demux::Decoder &decoder, std::span<const uint8_t> prefix,
                              demux::RejectCause cause, bool last) noexcept
{
    // 只在“该起点已无其它候选”时输出一行，避免多解码器并行尝试时刷屏（ADR-0009 §2.11）。
    if (!last || prefix.empty()) {
        return;
    }
    std::array<char, kRejectReasonBytes> reason{};
    const std::size_t len = (decoder.explain != nullptr) ? decoder.explain(prefix, reason) : 0U;
    const char *text = (len != 0U) ? reason.data() : rejectCauseName(cause);
    FW_LOGD(fw::TraceCat::kGnss, "<rx> %s rejected %lu bytes: %s", decoder.name,
            static_cast<unsigned long>(prefix.size()), text);
    fw::tlogHex(fw::TraceCat::kGnss, prefix);
}

/**
 * @brief 读回期间把一帧转成一行文本追加到 gnssReadbackText（ADR-0012 §2.7）。
 *
 * 交接缓冲是唯一的跳任务载体：kIo 在读回进行中逐行追加，控制台看到 gnssReadback.seq 变化后按 len 读取，
 * 读写不重叠（见 AppData.hpp 该成员的注释），因此不需要锁。
 * @param decoder: 该帧的解码器。
 * @param frame: 整帧字节。
 * @note 仅 kIo 调用，非阻塞，不分配。
 */
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

/**
 * @brief 发起一次配置读回，并把被拒原因就地发布（ADR-0012 §2.7）。
 * @param nowMs: 当前时间。
 * @param result: 写入命令结果（kOk / kBusy / kNotSupported）。
 * @note 仅 kIo 调用；被拒时立即发布结论，控制台不必等到超时。
 */
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

/**
 * @brief 发布一次读回结论，seq 严格递增（ADR-0012 §2.7）。
 *
 * 失败路径按卡要求“预支” stats+1，成功路径用 stats 原值，两者可能撞车；取 max(上一发布+1) 保证控制台
 * “等 seq 变化”的判定不会漏掉任何一次结论。
 * @param lines: 命中行数。
 * @param len: 文本有效字节。
 * @param supported: 驱动是否支持读回。
 * @param truncated: 是否有行因容量不足被丢弃。
 * @param busy: 是否因状态不对被拒。
 * @note 仅 kIo 调用。
 */
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

/**
 * @brief 跑一轮控制器状态机并把动作落到 UART，收尾时检测读回窗口结束
 * @param nowMs: 单调时间（毫秒）
 * @note 在模块任务里调用，最多 kMaxActionsPerPoll 次动作，不阻塞
 */
void GnssModule::pollController(uint32_t nowMs) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    for (size_t iter = 0U; iter < kMaxActionsPerPoll; ++iter) {
        const ::gnss::Action act = controller().poll(nowMs);
        if (act.kind == ::gnss::Action::Kind::kNone) {
            break;
        }
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
            const fw::Result err = applyBaud(act.baud);
            if (fw::isOk(err)) {
                traceGnss(GnssTrace::kBaud, act.baud, oldBaud);
                FW_LOGI(fw::TraceCat::kGnss, "<uart> baud %lu (was %lu)",
                        static_cast<unsigned long>(act.baud), static_cast<unsigned long>(oldBaud));
            }
            break;
        }
        case ::gnss::Action::Kind::kWrite: {
            const int written = uart_write_bytes(port, act.bytes.data(), act.bytes.size());
            if (written > 0) {
                uart_tx_bytes_ += static_cast<uint32_t>(written);
            }
            // 控制器下发的命令原文（去掉行尾 CR/LF）；探测阶段标 <probe>，其余标 <cfg>，
            // 方向固定写 TX（MCU → 接收机）：旧格式 "model <- cmd" 容易被读成接收（ADR-0009 §2.12）。
            std::size_t cmdLen = act.bytes.size();
            while ((cmdLen > 0U) &&
                   ((act.bytes[cmdLen - 1U] == '\r') || (act.bytes[cmdLen - 1U] == '\n'))) {
                --cmdLen;
            }
            // IDF action 的字节数有固定上限；逐字节转成 char 后保留命令原文供诊断。
            std::array<char, ::gnss::kMaxCommand> cmdText{};
            const size_t textLen = std::min(cmdLen, cmdText.size());
            for (size_t i = 0U; i < textLen; ++i) {
                cmdText[i] = std::bit_cast<char>(act.bytes[i]);
            }
            const auto st = controller().state();
            // textLen 不超过固定命令数组，远小于 INT_MAX。
            FW_LOGI(fw::TraceCat::kGnss, "%s TX \"%.*s\"",
                    (st == ::gnss::State::kProbing) ? "<probe>" : "<cfg>",
                    static_cast<int>(textLen), cmdText.data());
            const auto curStats = controller().stats();
            if ((st == ::gnss::State::kProbing) &&
                (curStats.probesSent > prev_ctrl_stats_.probesSent)) {
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
            break;
        }
        case ::gnss::Action::Kind::kNone:
        default:
            break;
        }
    }

    updateControllerStatus();
    // 读回收尾（ADR-0012 §2.7）：控制器已回 kRunning 并推进 readbackSeq，这里发布一次结论。
    if (controller().stats().readbackSeq != readback_base_seq_) {
        readback_base_seq_ = controller().stats().readbackSeq;
        FW_LOGI(fw::TraceCat::kGnss, "<cfg> Readback done, %u lines%s",
                static_cast<unsigned>(readback_lines_), readback_truncated_ ? " (truncated)" : "");
        publishReadback(readback_lines_, readback_len_, true, readback_truncated_, false);
    }
}

/**
 * @brief  把控制器状态变化与探测结果记为 I 级日志并更新快照
 */
void GnssModule::updateControllerStatus() noexcept
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
        traceGnss(GnssTrace::kState, static_cast<uint32_t>(curState),
                  static_cast<uint32_t>(last_state_));
        FW_LOGI(fw::TraceCat::kGnss, "<sm> state %s -> %s", ::gnss::stateName(last_state_),
                ::gnss::stateName(curState));
        if ((last_state_ == ::gnss::State::kProbing) && (curState == ::gnss::State::kConfiguring)) {
            const auto det = controller().detected();
            const uint32_t baud = (det.baud != 0U) ? det.baud : host_baud_;
            traceGnss(GnssTrace::kProbeHit, static_cast<uint32_t>(det.probe.model), baud);
            FW_LOGI(fw::TraceCat::kGnss, "<probe> detected %s at %lu",
                    ::gnss::modelName(det.probe.model), static_cast<unsigned long>(baud));
            step_idx_ = 0U;
            step_retry_ = 0U;
            step_sent_once_ = false;
        } else if ((last_state_ == ::gnss::State::kConfiguring) &&
                   (curState == ::gnss::State::kRunning)) {
            if (step_sent_once_) {
                traceGnss(GnssTrace::kStepAck, step_idx_, 0U);
            }
            persistDetectedHint();
            // 型号与 caps 已知，输出计划按支持的语句位重算一次（ADR-0012 §2.4）。
            controller().setOutputPlan(makePlan());
        } else if ((last_state_ == ::gnss::State::kRunning) &&
                   (curState == ::gnss::State::kProbing)) {
            if (!manual_reprobe_ && (curStats.reprobes > prev_ctrl_stats_.reprobes)) {
                const uint32_t silenceMs = timing_.has_value() ? timing_->silenceMs : 0U;
                traceGnss(GnssTrace::kSilence, silenceMs, 0U);
            }
            manual_reprobe_ = false;
        }
        last_state_ = curState;
    }
    prev_ctrl_stats_ = curStats;
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

::gnss::OutputPlan GnssModule::makePlan() noexcept
{
    uint8_t rateHz = 0U;
    bool gsv = false;
    if (rover_cfg_.has_value()) {
        rateHz = rover_cfg_->rateHz;
        gsv = rover_cfg_->gsv;
    }
    const uint32_t caps = controller().detected().caps;
    return data::makeOutputPlan(out_cfg_, rateHz, gsv, static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT),
                                caps);
}

void GnssModule::tickPendingApply(uint32_t nowMs) noexcept
{
    if (pending_apply_ == PendingApply::kNone) {
        return;
    }
    const auto stats = controller().stats();
    const bool landed = stats.outputSeq >= pending_seq_;
    // 超时按有符号差判断，毫秒计数回绕时仍成立。
    const bool timeout = static_cast<int32_t>(nowMs - pending_deadline_ms_) >= 0;
    if (!landed && !timeout) {
        return;
    }
    if (landed && (stats.failedMask == 0U)) {
        const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kConfigKeys);
        last_apply_ = fw::isOk(saveRes) ? GnssApply::kOk : GnssApply::kFailed;
    } else {
        // 有语句失败或超时：NVS 保持旧值，界面与计划一起回滚到变更前。
        if (pending_apply_ == PendingApply::kOut) {
            out_cfg_ = pending_prev_out_;
            ctx().data.gnssOutCfg.write(out_cfg_);
        } else {
            ensureRoverCfg().rateHz = pending_prev_rate_;
        }
        last_apply_ = GnssApply::kRestored;
        controller().setOutputPlan(makePlan());
        static_cast<void>(controller().requestOutput(controller().outputPlan(), nowMs));
    }
    pending_apply_ = PendingApply::kNone;
    ++apply_seq_;
    publishStatus(nowMs);
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

void GnssModule::persistDetectedHint() noexcept
{
    const auto &det = controller().detected();
    if (det.receiver == nullptr) {
        return;
    }
    const ::gnss::Family detFamily = det.receiver->family();
    const ::gnss::Model detModel = det.probe.model;
    const uint32_t detBaud = (det.baud != 0U) ? det.baud : host_baud_;
    if ((detFamily == hint_.family) && (detModel == hint_.model) && (detBaud == hint_.baud)) {
        return;
    }
    hint_.family = detFamily;
    hint_.model = detModel;
    hint_.baud = detBaud;
    const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kCacheKeys);
    if (!fw::isOk(saveRes)) {
        // StorageModule::saveKeys 内部已发 kStore trace；此处保留 hint_ 内存值避免每轮重复写盘。
    }
}

fw::Result GnssModule::applyPower(bool on) noexcept
{
#if CONFIG_RTK_GNSS_POWER_GPIO >= 0
    const auto powerPin = static_cast<gpio_num_t>(CONFIG_RTK_GNSS_POWER_GPIO);
    const bool activeHigh = (CONFIG_RTK_GNSS_POWER_ACTIVE_HIGH != 0);
    const uint32_t level = (on == activeHigh) ? 1U : 0U;
    return toResult(rtos::fromEspErr(gpio_set_level(powerPin, level)));
#else
    static_cast<void>(on);
    return fw::Result::kOk;
#endif
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

fw::Result GnssModule::loadFamily(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty()) {
        return fw::Result::kInvalidArg;
    }
    // 家族号越界（旧格式或损坏）由 familyFromU8() 报告，避免手工维护上限。
    const std::optional<::gnss::Family> family = ::gnss::familyFromU8(in[0]);
    if (!family.has_value()) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.hint_.family = *family;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveFamily(const void * /*obj*/, std::span<uint8_t> out,
                                  size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = static_cast<uint8_t>(appContext().gnss.hint_.family);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadModel(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty()) {
        return fw::Result::kInvalidArg;
    }
    const std::optional<::gnss::Model> model = ::gnss::modelFromU8(in[0]);
    if (!model.has_value()) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.hint_.model = *model;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveModel(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = static_cast<uint8_t>(appContext().gnss.hint_.model);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadBaud(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() < 4U) {
        return fw::Result::kInvalidArg;
    }
    const uint32_t baud = readU32(in);
    if ((baud != 0U) && ((baud < kBaudMin) || (baud > kBaudMax))) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.hint_.baud = baud;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveBaud(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < 4U) {
        return fw::Result::kInvalidArg;
    }
    writeU32(appContext().gnss.hint_.baud, out);
    len = 4U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadRate(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || !isValidRateHz(in[0])) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.ensureRoverCfg().rateHz = in[0];
    appContext().gnss.rate_loaded_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveRate(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    const auto &gnss = appContext().gnss;
    out[0] = (gnss.rate_loaded_ && gnss.rover_cfg_.has_value())
                 ? gnss.rover_cfg_->rateHz
                 : static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadGsv(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || (in[0] > 1U)) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.ensureRoverCfg().gsv = (in[0] != 0U);
    appContext().gnss.gsv_loaded_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveGsv(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    const auto &gnss = appContext().gnss;
    out[0] =
        (gnss.gsv_loaded_ && gnss.rover_cfg_.has_value()) ? (gnss.rover_cfg_->gsv ? 1U : 0U) : 1U;
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadOut(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    data::GnssOutConfig parsed{};
    if (!fw::isOk(data::GnssOutConfig::decode(in, parsed))) {
        auto &gnss = appContext().gnss;
        if (!gnss.out_cfg_.isValid()) {
            gnss.out_cfg_ = data::GnssOutConfig::defaults();
        }
        traceGnssSub(GnssSubTrace::kOutCfg, gnss.out_cfg_.mask, 1U);
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.out_cfg_ = parsed;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveOut(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    const auto &cfg = appContext().gnss.out_cfg_;
    const auto effective = cfg.isValid() ? cfg : data::GnssOutConfig::defaults();
    return effective.encode(out, len);
}

} // namespace app
