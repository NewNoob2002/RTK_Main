// GNSS 模块实现（ADR-0010 §2.7）：UART 收发、StreamDemux 分流、NMEA 解析、控制器轮询、RTCM 下发与 NVS 绑定。
// 线程：init() 启动前单线程；start()/wait()/poll()/on_command 仅 kIo 任务；keys/load/save 由 storage 与 kIo 调用。
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

void trace_gnss_sub(GnssSubTrace ev, uint32_t v0, uint32_t v1) noexcept
{
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(ev), 0U, v0, v1);
}

/** 拒绝原因在文本日志中的短名（explain 为空时的兜底，§2.11）。 */
const char *reject_cause_name(demux::RejectCause cause) noexcept
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
/** NMEA 五位消息名（如 GNGGA）在 '$' 与 talker 之后。 */
constexpr size_t kNmeaTagOffset = 1U;
constexpr size_t kNmeaTagLen = 6U;
/** `$command,` 之后的命令名起点。 */
constexpr size_t kCommandTagOffset = 9U;
/** RTCM3 消息号占 12 位，起点为长度字段第 1 字节。 */
constexpr size_t kRtcmTagOffset = 3U;
constexpr uint32_t kByteLowNibbleBits = 4U;
constexpr uint32_t kRtcmMsgMask = 0x0FFFU;
/** UBX class/id 位于同步字之后。 */
constexpr size_t kUbxClassOffset = 2U;

bool is_valid_rate_hz(uint8_t hz) noexcept
{
    constexpr std::array<uint8_t, 5> kRates{1U, 2U, 5U, 10U, 20U};
    return std::find(kRates.begin(), kRates.end(), hz) != kRates.end();
}

fw::Result to_result(fw::Status s) noexcept
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

uint32_t read_u32(std::span<const uint8_t> in) noexcept
{
    uint32_t out = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        // in.size() >= 4 已在调用前校验；i < 4 左移不会超出 32 位。
        out |= static_cast<uint32_t>(in[i]) << (8U * i);
    }
    return out;
}

void write_u32(uint32_t value, std::span<uint8_t> out) noexcept
{
    for (size_t i = 0U; i < 4U; ++i) {
        // out.size() >= 4 已在调用前校验；取字节无窄化丢失。
        out[i] = static_cast<uint8_t>((value >> (8U * i)) & kByteMask);
    }
}

void add_unique_decoder(std::array<const demux::Decoder *, demux::kMaxDecoders> &out, size_t &count,
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

void GnssModule::DemuxSink::on_frame(const demux::Decoder &decoder, uint8_t /*index*/,
                                     std::span<const uint8_t> frame) noexcept
{
    if (owner_ != nullptr) {
        owner_->handle_frame(decoder, frame);
    }
}

void GnssModule::DemuxSink::on_reject(const demux::Decoder &decoder, uint8_t /*index*/, std::span<const uint8_t> prefix,
                                      demux::RejectCause cause, bool last) noexcept
{
    if (owner_ != nullptr) {
        owner_->handle_reject(decoder, prefix, cause, last);
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

demux::Demux &GnssModule::stream_demux() noexcept
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
    auto &cfg = ensure_rover_cfg();
    if (!rate_loaded_) {
        cfg.rate_hz = static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ);
    }
    if (!gsv_loaded_) {
        cfg.gsv = true;
    }
    cfg.port = static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT);
    if (!out_cfg_.is_valid()) {
        out_cfg_ = data::GnssOutConfig::defaults();
    }

    const auto uart_result = init_uart();
    if (!fw::is_ok(uart_result)) {
        return uart_result;
    }

    const fw::Result pipe_res = to_result(ctx().rtcm_pipe.init());
    if (!fw::is_ok(pipe_res)) {
        return init_failed(pipe_res);
    }

    decoder_count_ = 0U;
    add_unique_decoder(decoders_, decoder_count_, &demux::nmea::kDecoder);
    add_unique_decoder(decoders_, decoder_count_, &demux::rtcm3::kDecoder);
    // 驱动私有解码器（UBX、Unicore # 帧、$command 应答等）由各驱动自己声明，顺序即探测优先级。
    const std::span<const ::gnss::Receiver *const> receivers = ::gnss::enabled_receivers();
    for (const ::gnss::Receiver *receiver : receivers) {
        if (receiver == nullptr) {
            continue;
        }
        for (const demux::Decoder *dec : receiver->decoders()) {
            add_unique_decoder(decoders_, decoder_count_, dec);
        }
    }

    const demux::Error demux_err =
        stream_demux().init(std::span<const demux::Decoder *const>{decoders_.data(), decoder_count_}, demux_buf_, sink);
    if (demux_err != demux::Error::kOk) {
        return init_failed(fw::Result::kInvalidState);
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
    controller().set_output_plan(
        data::make_output_plan(out_cfg_, cfg.rate_hz, cfg.gsv, static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT), 0U));
    if (!controller().init(receivers, std::span<const uint32_t>{bauds_.data(), baud_count_}, cfg,
                           controller().output_plan(), hint_, timing)) {
        return init_failed(fw::Result::kInvalidState);
    }

    host_baud_ = CONFIG_RTK_GNSS_DEFAULT_BAUD;
    last_state_ = controller().state();
    last_published_state_ = last_state_;
    prev_ctrl_stats_ = controller().stats();
    ctx().data.gnss_out_cfg.write(out_cfg_);
    trace_gnss_sub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
    initialized_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::init_uart() noexcept
{
    // Kconfig UART 号限制为 0..2，缓冲区常量均小于 INT_MAX，三处转换安全。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t drv_err =
        uart_driver_install(port, static_cast<int>(kUartRxBufBytes), static_cast<int>(kUartTxBufBytes), 0, nullptr, 0);
    if (drv_err != ESP_OK) {
        return to_result(rtos::from_esp_err(drv_err));
    }
    uart_installed_ = true;
    const uart_config_t uart_cfg{
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
    const esp_err_t cfg_err = uart_param_config(port, &uart_cfg);
    if (cfg_err != ESP_OK) {
        return init_failed(to_result(rtos::from_esp_err(cfg_err)));
    }
    const esp_err_t pin_err =
        uart_set_pin(port, CONFIG_RTK_GNSS_TX_GPIO, CONFIG_RTK_GNSS_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (pin_err != ESP_OK) {
        return init_failed(to_result(rtos::from_esp_err(pin_err)));
    }
#if CONFIG_RTK_GNSS_POWER_GPIO >= 0
    // Kconfig GPIO 限制为 0..39，枚举转换与 64 位移位安全。
    const auto power_pin = static_cast<gpio_num_t>(CONFIG_RTK_GNSS_POWER_GPIO);
    const uint32_t off_level = (CONFIG_RTK_GNSS_POWER_ACTIVE_HIGH != 0) ? 0U : 1U;
    const esp_err_t level_err = gpio_set_level(power_pin, off_level);
    if (level_err != ESP_OK) {
        return init_failed(to_result(rtos::from_esp_err(level_err)));
    }
    gpio_config_t power_cfg{};
    power_cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(CONFIG_RTK_GNSS_POWER_GPIO);
    power_cfg.mode = GPIO_MODE_OUTPUT;
    power_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    power_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_cfg.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t gpio_err = gpio_config(&power_cfg);
    if (gpio_err != ESP_OK) {
        return init_failed(to_result(rtos::from_esp_err(gpio_err)));
    }
#endif
    return fw::Result::kOk;
}

fw::Status GnssModule::stop_boot() noexcept
{
    if (!uart_installed_) {
        return fw::Status::kOk;
    }
    // Kconfig UART 号限制为 0..2；与安装时同一端口。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t err = uart_driver_delete(port);
    if (err != ESP_OK) {
        return rtos::from_esp_err(err);
    }
    uart_installed_ = false;
    initialized_ = false;
    return fw::Status::kOk;
}

fw::Result GnssModule::init_failed(fw::Result original) noexcept
{
    const auto cleanup = stop_boot();
    return fw::is_ok(cleanup) ? original : to_result(cleanup);
}

fw::Result GnssModule::start() noexcept
{
    if (!initialized_) {
        return fw::Result::kInvalidState;
    }
    const uint32_t now = rtos::now_ms();
    controller().start(now);
    poll_controller(now);
    publish_status(now);
    return fw::Result::kOk;
}

/**
 * @brief  等 UART 数据或命令事件，最长 max_ms
 * @param  max_ms: 最长等待（ms）
 */
void GnssModule::wait(uint32_t max_ms) noexcept
{
    if (!initialized_) {
        vTaskDelay(pdMS_TO_TICKS(max_ms));
        return;
    }
    std::array<uint8_t, kUartReadChunkBytes> buf{};
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int n = uart_read_bytes(port, buf.data(), buf.size(), pdMS_TO_TICKS(max_ms));
    if (n <= 0) {
        last_rx_bytes_ = 0U;
        return;
    }
    const auto count = static_cast<size_t>(n);
    last_rx_bytes_ = count;
    uart_rx_bytes_ += static_cast<uint32_t>(count);
    const demux::Error err = stream_demux().feed(std::span<const uint8_t>{buf.data(), count});
    if (err != demux::Error::kOk) {
        trace_gnss(GnssTrace::kState, static_cast<uint32_t>(controller().state()), static_cast<uint32_t>(err));
    }
    const auto &dstats = stream_demux().stats();
    if (dstats.unknown_bytes > prev_unknown_bytes_) {
        FW_LOGD(fw::TraceCat::kGnss, "%lu unknown bytes dropped",
                static_cast<unsigned long>(dstats.unknown_bytes - prev_unknown_bytes_));
        prev_unknown_bytes_ = dstats.unknown_bytes;
    }
}

bool GnssModule::poll(uint32_t now_ms) noexcept
{
    if (!initialized_) {
        return false;
    }
    poll_controller(now_ms);
    tick_pending_apply(now_ms);
    const bool tx_progress = pump_rtcm_pipe();

    const auto cur_state = controller().state();
    if ((cur_state != last_published_state_) || ((now_ms - last_status_ms_) >= kStatusPeriodMs)) {
        publish_status(now_ms);
    }

    const bool had_full_rx = (last_rx_bytes_ == kUartReadChunkBytes);
    last_rx_bytes_ = 0U;
    const bool allow_tx = (cur_state == ::gnss::State::kRunning) || (cur_state == ::gnss::State::kConfiguring) ||
                          (cur_state == ::gnss::State::kProbing);
    const bool has_more_tx = (rtcm_tx_len_ > rtcm_tx_off_) || (ctx().rtcm_pipe.available() > 0U);
    return had_full_rx || (allow_tx && tx_progress && has_more_tx);
}

void GnssModule::on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!initialized_) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t now = rtos::now_ms();
    switch (static_cast<GnssVerb>(cmd.verb)) {
    case GnssVerb::kProbe:
        if (cmd.arg_len != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        manual_reprobe_ = true;
        parser().reset();
        ctx().data.gnss_fix.write(::gnss::Fix{});
        controller().request_reprobe(now);
        poll_controller(now);
        publish_status(now);
        result.status = fw::Result::kOk;
        return;

    case GnssVerb::kReset:
        if (cmd.arg_len != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        manual_reprobe_ = true;
        parser().reset();
        ctx().data.gnss_fix.write(::gnss::Fix{});
        controller().stop(now);
        poll_controller(now);
        controller().start(now);
        poll_controller(now);
        publish_status(now);
        result.status = fw::Result::kOk;
        return;

    case GnssVerb::kSetRate: {
        if ((cmd.arg_len != 1U) || !is_valid_rate_hz(cmd.args[0])) {
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
            const uint8_t prev_rate = ensure_rover_cfg().rate_hz;
            ensure_rover_cfg().rate_hz = cmd.args[0];
            const fw::Result save_res = ctx().storage.save_keys(gnss_store::kConfigKeys);
            if (!fw::is_ok(save_res)) {
                ensure_rover_cfg().rate_hz = prev_rate;
                result.status = save_res;
                return;
            }
            controller().set_output_plan(make_plan());
            last_apply_ = GnssApply::kOk;
            ++apply_seq_;
            publish_status(now);
            result.status = fw::Result::kOk;
            return;
        }
        // 两阶段提交（ADR-0012 §2.4）：先暂存并重配，成功后才写 NVS；失败由 tick_pending_apply 回滚。
        pending_prev_rate_ = ensure_rover_cfg().rate_hz;
        ensure_rover_cfg().rate_hz = cmd.args[0];
        pending_apply_ = PendingApply::kRate;
        pending_seq_ = controller().stats().output_seq + 1U;
        pending_deadline_ms_ = now + kApplyTimeoutMs;
        controller().set_output_plan(make_plan());
        controller().request_reconfigure(now);
        poll_controller(now);
        publish_status(now);
        result.status = fw::Result::kOk;
        return;
    }

    case GnssVerb::kSend:
        send_raw(cmd, result);
        return;

    default: {
        if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetOut) {
            data::GnssOutConfig next{};
            if ((cmd.arg_len != data::GnssOutConfig::kBlobBytes) ||
                !fw::is_ok(data::GnssOutConfig::decode(std::span<const uint8_t>{cmd.args.data(), cmd.arg_len}, next))) {
                result.status = fw::Result::kInvalidArg;
                return;
            }
            if (pending_apply_ != PendingApply::kNone) {
                result.status = fw::Result::kBusy;
                return;
            }
            const data::GnssOutConfig prev = out_cfg_;
            out_cfg_ = next;
            ctx().data.gnss_out_cfg.write(out_cfg_);
            trace_gnss_sub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
            if (controller().state() != ::gnss::State::kRunning) {
                // 未进入 kRunning 时没有可下发的输出阶段：直接落盘，下次配置完成时按新表重算计划。
                const fw::Result save_res = ctx().storage.save_keys(gnss_store::kConfigKeys);
                if (!fw::is_ok(save_res)) {
                    out_cfg_ = prev;
                    ctx().data.gnss_out_cfg.write(out_cfg_);
                    result.status = save_res;
                    return;
                }
                controller().set_output_plan(make_plan());
                last_apply_ = GnssApply::kOk;
                ++apply_seq_;
                publish_status(now);
                result.status = fw::Result::kOk;
                return;
            }
            pending_prev_out_ = prev;
            pending_apply_ = PendingApply::kOut;
            pending_seq_ = controller().stats().output_seq + 1U;
            pending_deadline_ms_ = now + kApplyTimeoutMs;
            controller().set_output_plan(make_plan());
            static_cast<void>(controller().request_output(controller().output_plan(), now));
            poll_controller(now);
            publish_status(now);
            result.status = fw::Result::kOk;
            return;
        }
        result.status = fw::Result::kNotSupported;
        return;
    }
    }
}

void GnssModule::send_raw(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto st = controller().state();
    if ((st != ::gnss::State::kRunning) && (st != ::gnss::State::kFailed)) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    if ((cmd.arg_len == 0U) || (cmd.arg_len > cmd.args.size())) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    std::array<char, fw::kCommandArgBytes + 2U> line{};
    // arg_len 已检查不超过固定 args；size_t 足以表示它。
    const auto len = static_cast<size_t>(cmd.arg_len);
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
void GnssModule::handle_frame(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept
{
    // 文本日志不经中断路径：本函数只由 kIo 任务的 feed() 调用（ADR-0009 §2.11）。
    std::array<char, kLogTagBytes> tag{};
    controller().receiver().frame_tag(decoder, frame, tag);
    FW_LOGD(fw::TraceCat::kGnss, "%s, %lu bytes", tag.data(), static_cast<unsigned long>(frame.size()));
    fw::tlog_hex(fw::TraceCat::kGnss, frame);

    const uint32_t now = rtos::now_ms();
    controller().on_frame(decoder, frame, now);
    if (&decoder == &demux::nmea::kDecoder) {
        const auto res = parser().feed(frame, now);
        if (res == ::gnss::NmeaResult::kFix) {
            ctx().data.gnss_fix.write(parser().fix());
        } else if (res == ::gnss::NmeaResult::kSatsReady) {
            // GGA 触发卫星轮发布时已在同一次 feed() 中更新 fix_（G-A §4.1），须同步刷新定位槽。
            ctx().data.gnss_fix.write(parser().fix());
            ctx().data.gnss_sats.write(parser().sats());
        }
    }

    // ADR-0011 §5：6 类可订阅语句同步更新 6 KiB 最新帧缓存，并按会话掩码与周期写入 App typed FanoutRing。
    {
        // 分类交给驱动（ADR-0012 §2.6）：基类覆盖 5 类 NMEA，Unicore 另加 #SATSINFOA/#HEADINGA。
        const ::gnss::OutputMsg cls = controller().receiver().classify_frame(decoder, frame);
        const auto idx = static_cast<size_t>(static_cast<uint8_t>(cls));
        if ((cls != ::gnss::OutputMsg::kNone) && (idx < data::kGnssAppMsgCount)) {
            const auto type_idx = static_cast<uint8_t>(idx);
            if (!ctx().data.gnss_latest.update(type_idx, frame)) {
                trace_gnss_sub(GnssSubTrace::kAppDrop, 2U, type_idx);
            }
            uint32_t active_mask = 0U;
            std::array<uint32_t, data::kGnssAppMsgCount> periods_ms{};
            ctx().data.gnss_app_sub.read_subscription(active_mask, periods_ms);
            if ((active_mask & (1U << type_idx)) == 0U) {
                // 无订阅者：FanoutRing::write 只计 no_subscriber，不写入环形缓冲；闸门复位，重新订阅后首句立即推送。
                static_cast<void>(ctx().data.gnss_app_ring.write(type_idx, frame));
                push_gates_[type_idx].reset();
            } else {
                const uint32_t period = periods_ms[type_idx];
                auto &gate = push_gates_[type_idx];
                if (!gate.due(now, period)) {
                    ctx().data.gnss_app_sub.add_skipped(type_idx);
                } else {
                    const fw::Result wres = ctx().data.gnss_app_ring.write(type_idx, frame);
                    if (fw::is_ok(wres)) {
                        gate.on_pushed(now, period);
                    } else if (wres == fw::Result::kInvalidArg) {
                        ctx().data.gnss_app_sub.add_ring_oversize(type_idx);
                        trace_gnss_sub(GnssSubTrace::kAppDrop, 0U, type_idx);
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
void GnssModule::handle_reject(const demux::Decoder &decoder, std::span<const uint8_t> prefix, demux::RejectCause cause,
                               bool last) noexcept
{
    // 只在“该起点已无其它候选”时输出一行，避免多解码器并行尝试时刷屏（ADR-0009 §2.11）。
    if (!last || prefix.empty()) {
        return;
    }
    std::array<char, kRejectReasonBytes> reason{};
    const std::size_t len = (decoder.explain != nullptr) ? decoder.explain(prefix, reason) : 0U;
    const char *text = (len != 0U) ? reason.data() : reject_cause_name(cause);
    FW_LOGD(fw::TraceCat::kGnss, "%s rejected %lu bytes: %s", decoder.name, static_cast<unsigned long>(prefix.size()),
            text);
    fw::tlog_hex(fw::TraceCat::kGnss, prefix);
}

/**
 * @brief  推进 GNSS 控制器状态机并执行其动作（发命令、改波特率、上电）
 * @param  now_ms: 当前时间（ms）
 */
void GnssModule::poll_controller(uint32_t now_ms) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    for (size_t iter = 0U; iter < kMaxActionsPerPoll; ++iter) {
        const ::gnss::Action act = controller().poll(now_ms);
        if (act.kind == ::gnss::Action::Kind::kNone) {
            break;
        }
        switch (act.kind) {
        case ::gnss::Action::Kind::kPower: {
            const fw::Result err = apply_power(act.on);
            if (!fw::is_ok(err)) {
                trace_gnss(GnssTrace::kState, static_cast<uint32_t>(controller().state()), static_cast<uint32_t>(err));
            }
            break;
        }
        case ::gnss::Action::Kind::kSetBaud: {
            const uint32_t old_baud = host_baud_;
            const fw::Result err = apply_baud(act.baud);
            if (fw::is_ok(err)) {
                trace_gnss(GnssTrace::kBaud, act.baud, old_baud);
                FW_LOGI(fw::TraceCat::kGnss, "baud %lu (was %lu)", static_cast<unsigned long>(act.baud),
                        static_cast<unsigned long>(old_baud));
            }
            break;
        }
        case ::gnss::Action::Kind::kWrite: {
            const int written = uart_write_bytes(port, act.bytes.data(), act.bytes.size());
            if (written > 0) {
                uart_tx_bytes_ += static_cast<uint32_t>(written);
            }
            // 控制器下发的命令原文（去掉行尾 CR/LF），模型名用于区分探测/配置阶段（§2.11）。
            std::size_t cmd_len = act.bytes.size();
            while ((cmd_len > 0U) && ((act.bytes[cmd_len - 1U] == '\r') || (act.bytes[cmd_len - 1U] == '\n'))) {
                --cmd_len;
            }
            // IDF action 的字节数有固定上限；逐字节转成 char 后保留命令原文供诊断。
            std::array<char, ::gnss::kMaxCommand> cmd_text{};
            const size_t text_len = std::min(cmd_len, cmd_text.size());
            for (size_t i = 0U; i < text_len; ++i) {
                cmd_text[i] = std::bit_cast<char>(act.bytes[i]);
            }
            // text_len 不超过固定命令数组，远小于 INT_MAX。
            FW_LOGI(fw::TraceCat::kGnss, "%s <- \"%.*s\"", ::gnss::model_name(controller().detected().probe.model),
                    static_cast<int>(text_len), cmd_text.data());
            const auto st = controller().state();
            const auto cur_stats = controller().stats();
            if ((st == ::gnss::State::kProbing) && (cur_stats.probes_sent > prev_ctrl_stats_.probes_sent)) {
                trace_gnss(GnssTrace::kProbeSend, static_cast<uint32_t>(controller().probe_receiver().family()),
                           host_baud_);
                prev_ctrl_stats_.probes_sent = cur_stats.probes_sent;
            } else if ((st == ::gnss::State::kConfiguring) && (cur_stats.steps_sent > prev_ctrl_stats_.steps_sent)) {
                const bool retried = (cur_stats.step_nacks > prev_ctrl_stats_.step_nacks) ||
                                     (cur_stats.step_timeouts > prev_ctrl_stats_.step_timeouts);
                if (retried) {
                    ++step_retry_;
                } else if (step_sent_once_) {
                    trace_gnss(GnssTrace::kStepAck, step_idx_, 0U);
                    ++step_idx_;
                    step_retry_ = 0U;
                }
                step_sent_once_ = true;
                trace_gnss(GnssTrace::kStepSend, step_idx_, step_retry_);
                prev_ctrl_stats_.steps_sent = cur_stats.steps_sent;
            }
            break;
        }
        case ::gnss::Action::Kind::kNone:
        default:
            break;
        }
    }

    update_controller_status();
}

/**
 * @brief  把控制器状态变化与探测结果记为 I 级日志并更新快照
 */
void GnssModule::update_controller_status() noexcept
{
    const auto cur_stats = controller().stats();
    if (cur_stats.step_nacks > prev_ctrl_stats_.step_nacks) {
        trace_gnss(GnssTrace::kStepNack, step_idx_, 0U);
    }
    if (cur_stats.step_timeouts > prev_ctrl_stats_.step_timeouts) {
        trace_gnss(GnssTrace::kStepTimeout, step_idx_, step_retry_ + 1U);
    }

    const auto cur_state = controller().state();
    if (cur_state != last_state_) {
        trace_gnss(GnssTrace::kState, static_cast<uint32_t>(cur_state), static_cast<uint32_t>(last_state_));
        FW_LOGI(fw::TraceCat::kGnss, "state %s -> %s", ::gnss::state_name(last_state_), ::gnss::state_name(cur_state));
        if ((last_state_ == ::gnss::State::kProbing) && (cur_state == ::gnss::State::kConfiguring)) {
            const auto det = controller().detected();
            const uint32_t baud = (det.baud != 0U) ? det.baud : host_baud_;
            trace_gnss(GnssTrace::kProbeHit, static_cast<uint32_t>(det.probe.model), baud);
            FW_LOGI(fw::TraceCat::kGnss, "detected %s at %lu", ::gnss::model_name(det.probe.model),
                    static_cast<unsigned long>(baud));
            step_idx_ = 0U;
            step_retry_ = 0U;
            step_sent_once_ = false;
        } else if ((last_state_ == ::gnss::State::kConfiguring) && (cur_state == ::gnss::State::kRunning)) {
            if (step_sent_once_) {
                trace_gnss(GnssTrace::kStepAck, step_idx_, 0U);
            }
            persist_detected_hint();
            // 型号与 caps 已知，输出计划按支持的语句位重算一次（ADR-0012 §2.4）。
            controller().set_output_plan(make_plan());
        } else if ((last_state_ == ::gnss::State::kRunning) && (cur_state == ::gnss::State::kProbing)) {
            if (!manual_reprobe_ && (cur_stats.reprobes > prev_ctrl_stats_.reprobes)) {
                const uint32_t silence_ms = timing_.has_value() ? timing_->silence_ms : 0U;
                trace_gnss(GnssTrace::kSilence, silence_ms, 0U);
            }
            manual_reprobe_ = false;
        }
        last_state_ = cur_state;
    }
    prev_ctrl_stats_ = cur_stats;
}

bool GnssModule::pump_rtcm_pipe() noexcept
{
    const auto st = controller().state();
    const bool allow_tx =
        (st == ::gnss::State::kRunning) || (st == ::gnss::State::kConfiguring) || (st == ::gnss::State::kProbing);
    if (!allow_tx) {
        if (rtcm_tx_len_ > rtcm_tx_off_) {
            ctx().rtcm_pipe.note_discarded(rtcm_tx_len_ - rtcm_tx_off_);
            rtcm_tx_len_ = 0U;
            rtcm_tx_off_ = 0U;
        }
        ctx().rtcm_pipe.discard();
        return false;
    }
    if (rtcm_tx_off_ >= rtcm_tx_len_) {
        rtcm_tx_len_ = ctx().rtcm_pipe.read(rtcm_tx_buf_);
        rtcm_tx_off_ = 0U;
    }
    if (rtcm_tx_off_ >= rtcm_tx_len_) {
        ctx().rtcm_pipe.set_pending(0U, false);
        return false;
    }
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const auto pending = std::span<const uint8_t>{rtcm_tx_buf_}.subspan(rtcm_tx_off_, rtcm_tx_len_ - rtcm_tx_off_);
    const int written = uart_write_bytes(port, pending.data(), pending.size());
    if (written > 0) {
        const auto bytes = static_cast<size_t>(written);
        rtcm_tx_off_ += bytes;
        if (rtcm_tx_off_ >= rtcm_tx_len_) {
            rtcm_tx_off_ = 0U;
            rtcm_tx_len_ = 0U;
        }
        uart_tx_bytes_ += static_cast<uint32_t>(bytes);
        ctx().rtcm_pipe.note_written(bytes);
        ctx().rtcm_pipe.set_pending(rtcm_tx_len_ - rtcm_tx_off_, false);
        return true;
    }
    if (written == 0) {
        // 驱动 TX 环暂满：保留块与偏移，下轮继续，不丢块也不重读管道。
        ctx().rtcm_pipe.set_pending(rtcm_tx_len_ - rtcm_tx_off_, true);
        return false;
    }
    const size_t lost = rtcm_tx_len_ - rtcm_tx_off_;
    rtcm_tx_off_ = 0U;
    rtcm_tx_len_ = 0U;
    ctx().rtcm_pipe.note_write_error(lost);
    trace_gnss(GnssTrace::kState, static_cast<uint32_t>(st), static_cast<uint32_t>(fw::Result::kIoError));
    FW_LOGW(fw::TraceCat::kGnss, "RTCM UART write error (%d), dropped %lu pending bytes", written,
            static_cast<unsigned long>(lost));
    return false;
}

::gnss::OutputPlan GnssModule::make_plan() noexcept
{
    uint8_t rate_hz = 0U;
    bool gsv = false;
    if (rover_cfg_.has_value()) {
        rate_hz = rover_cfg_->rate_hz;
        gsv = rover_cfg_->gsv;
    }
    const uint32_t caps = controller().detected().caps;
    return data::make_output_plan(out_cfg_, rate_hz, gsv, static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT), caps);
}

void GnssModule::tick_pending_apply(uint32_t now_ms) noexcept
{
    if (pending_apply_ == PendingApply::kNone) {
        return;
    }
    const auto stats = controller().stats();
    const bool landed = stats.output_seq >= pending_seq_;
    // 超时按有符号差判断，毫秒计数回绕时仍成立。
    const bool timeout = static_cast<int32_t>(now_ms - pending_deadline_ms_) >= 0;
    if (!landed && !timeout) {
        return;
    }
    if (landed && (stats.failed_mask == 0U)) {
        const fw::Result save_res = ctx().storage.save_keys(gnss_store::kConfigKeys);
        last_apply_ = fw::is_ok(save_res) ? GnssApply::kOk : GnssApply::kFailed;
    } else {
        // 有语句失败或超时：NVS 保持旧值，界面与计划一起回滚到变更前。
        if (pending_apply_ == PendingApply::kOut) {
            out_cfg_ = pending_prev_out_;
            ctx().data.gnss_out_cfg.write(out_cfg_);
        } else {
            ensure_rover_cfg().rate_hz = pending_prev_rate_;
        }
        last_apply_ = GnssApply::kRestored;
        controller().set_output_plan(make_plan());
        static_cast<void>(controller().request_output(controller().output_plan(), now_ms));
    }
    pending_apply_ = PendingApply::kNone;
    ++apply_seq_;
    publish_status(now_ms);
}

void GnssModule::publish_status(uint32_t now_ms) noexcept
{
    GnssStatus st{};
    st.enabled = initialized_;
    st.state = controller().state();
    const auto &det = controller().detected();
    if (det.receiver != nullptr) {
        st.family = det.receiver->family();
        st.model = det.probe.model;
        st.fw_version = det.probe.fw_version;
        st.caps = det.caps;
    }
    st.host_baud = host_baud_;
    st.ctrl = controller().stats();
    st.output_seq = st.ctrl.output_seq;
    st.applied_mask = st.ctrl.applied_mask;
    st.unsupported_mask = st.ctrl.unsupported_mask;
    st.failed_mask = st.ctrl.failed_mask;
    st.last_apply = last_apply_;
    st.apply_seq = apply_seq_;
    static_assert(std::tuple_size_v<decltype(st.rx_period_ms)> == data::kGnssAppMsgCount);
    const auto &plan = controller().output_plan();
    for (size_t i = 0U; i < st.rx_period_ms.size(); ++i) {
        st.rx_period_ms[i] = plan.period_ms[i];
    }
    // App 可见的支持位 = 驱动声明的语句位 & 本轮未报不支持 & 未失败（ADR-0012 §2.5）。
    if (det.receiver != nullptr) {
        constexpr uint32_t kAppBits = (1U << static_cast<uint32_t>(data::kGnssAppMsgCount)) - 1U;
        const uint32_t declared = det.receiver->output_mask(det.probe.model) & kAppBits;
        st.supported_mask = declared & ~st.ctrl.unsupported_mask & ~st.ctrl.failed_mask;
    }
    st.nmea = parser().stats();
    st.uart_rx_bytes = uart_rx_bytes_;
    st.uart_tx_bytes = uart_tx_bytes_;
    const auto &dstats = stream_demux().stats();
    st.demux_unknown = dstats.unknown_bytes;
    st.demux_resyncs = dstats.resyncs;
    st.pipe = ctx().rtcm_pipe.stats();

    ctx().data.gnss_status.write(st);
    last_status_ms_ = now_ms;
    last_published_state_ = st.state;
}

void GnssModule::persist_detected_hint() noexcept
{
    const auto &det = controller().detected();
    if (det.receiver == nullptr) {
        return;
    }
    const ::gnss::Family det_family = det.receiver->family();
    const ::gnss::Model det_model = det.probe.model;
    const uint32_t det_baud = (det.baud != 0U) ? det.baud : host_baud_;
    if ((det_family == hint_.family) && (det_model == hint_.model) && (det_baud == hint_.baud)) {
        return;
    }
    hint_.family = det_family;
    hint_.model = det_model;
    hint_.baud = det_baud;
    const fw::Result save_res = ctx().storage.save_keys(gnss_store::kCacheKeys);
    if (!fw::is_ok(save_res)) {
        // StorageModule::save_keys 内部已发 kStore trace；此处保留 hint_ 内存值避免每轮重复写盘。
    }
}

fw::Result GnssModule::apply_power(bool on) noexcept
{
#if CONFIG_RTK_GNSS_POWER_GPIO >= 0
    const auto power_pin = static_cast<gpio_num_t>(CONFIG_RTK_GNSS_POWER_GPIO);
    const bool active_high = (CONFIG_RTK_GNSS_POWER_ACTIVE_HIGH != 0);
    const uint32_t level = (on == active_high) ? 1U : 0U;
    return to_result(rtos::from_esp_err(gpio_set_level(power_pin, level)));
#else
    static_cast<void>(on);
    return fw::Result::kOk;
#endif
}

fw::Result GnssModule::apply_baud(uint32_t baud) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t baud_err = uart_set_baudrate(port, baud);
    if (baud_err != ESP_OK) {
        return to_result(rtos::from_esp_err(baud_err));
    }
    const esp_err_t flush_err = uart_flush_input(port);
    if (flush_err != ESP_OK) {
        return to_result(rtos::from_esp_err(flush_err));
    }
    host_baud_ = baud;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_family(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty()) {
        return fw::Result::kInvalidArg;
    }
    // 家族号越界（旧格式或损坏）由 family_from_u8() 报告，避免手工维护上限。
    const std::optional<::gnss::Family> family = ::gnss::family_from_u8(in[0]);
    if (!family.has_value()) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.hint_.family = *family;
    return fw::Result::kOk;
}

fw::Result GnssModule::save_family(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = static_cast<uint8_t>(app_context().gnss.hint_.family);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_model(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty()) {
        return fw::Result::kInvalidArg;
    }
    const std::optional<::gnss::Model> model = ::gnss::model_from_u8(in[0]);
    if (!model.has_value()) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.hint_.model = *model;
    return fw::Result::kOk;
}

fw::Result GnssModule::save_model(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = static_cast<uint8_t>(app_context().gnss.hint_.model);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_baud(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() < 4U) {
        return fw::Result::kInvalidArg;
    }
    const uint32_t baud = read_u32(in);
    if ((baud != 0U) && ((baud < kBaudMin) || (baud > kBaudMax))) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.hint_.baud = baud;
    return fw::Result::kOk;
}

fw::Result GnssModule::save_baud(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < 4U) {
        return fw::Result::kInvalidArg;
    }
    write_u32(app_context().gnss.hint_.baud, out);
    len = 4U;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_rate(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || !is_valid_rate_hz(in[0])) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.ensure_rover_cfg().rate_hz = in[0];
    app_context().gnss.rate_loaded_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::save_rate(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    const auto &gnss = app_context().gnss;
    out[0] = (gnss.rate_loaded_ && gnss.rover_cfg_.has_value()) ? gnss.rover_cfg_->rate_hz
                                                                : static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_gsv(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || (in[0] > 1U)) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.ensure_rover_cfg().gsv = (in[0] != 0U);
    app_context().gnss.gsv_loaded_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::save_gsv(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    const auto &gnss = app_context().gnss;
    out[0] = (gnss.gsv_loaded_ && gnss.rover_cfg_.has_value()) ? (gnss.rover_cfg_->gsv ? 1U : 0U) : 1U;
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_out(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    data::GnssOutConfig parsed{};
    if (!fw::is_ok(data::GnssOutConfig::decode(in, parsed))) {
        auto &gnss = app_context().gnss;
        if (!gnss.out_cfg_.is_valid()) {
            gnss.out_cfg_ = data::GnssOutConfig::defaults();
        }
        trace_gnss_sub(GnssSubTrace::kOutCfg, gnss.out_cfg_.mask, 1U);
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.out_cfg_ = parsed;
    return fw::Result::kOk;
}

fw::Result GnssModule::save_out(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    const auto &cfg = app_context().gnss.out_cfg_;
    const auto effective = cfg.is_valid() ? cfg : data::GnssOutConfig::defaults();
    return effective.encode(out, len);
}

} // namespace app
