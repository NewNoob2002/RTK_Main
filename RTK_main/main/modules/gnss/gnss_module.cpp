// GNSS 模块实现（ADR-0010 §2.7）：UART 收发、StreamDemux 分流、NMEA 解析、控制器轮询、RTCM 下发与 NVS 绑定。
// 线程：init() 启动前单线程；start()/wait()/poll()/on_command 仅 kIo 任务；keys/load/save 由 storage 与 kIo 调用。
// 均非 ISR；不分配堆。实时路径只用 GnssTrace；文本诊断经 fw::tlog()（ADR-0009 §2.11）。

#include "modules/gnss/gnss_module.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_context.hpp"
#include "def/gnss_sub_def.hpp"
#include "demux/decoders/nmea.hpp"
#include "demux/decoders/rtcm3.hpp"
#include "demux/decoders/unicore_hash.hpp"
#include "fw/status.hpp"
#include "fw/tlog.hpp"
#include "gnss/dialect.hpp"
#include "modules/gnss/gnss_keys.hpp"
#include "modules/storage/storage_module.hpp"
#include "rtos/esp_status.hpp"

namespace app {

namespace {

constexpr uint32_t kByteMask = 0xFFU;
constexpr size_t kMaxActionsPerPoll = 16U;
constexpr uint32_t kBaudMin = 1200U;
constexpr uint32_t kBaudMax = 2000000U;
constexpr uint32_t kMsPerSec = 1000U;
constexpr uint32_t kCentiDiv = 10U;
constexpr size_t kOutCmdLineBytes = 64U;
constexpr size_t kOutPeriodTextBytes = 16U;

void trace_gnss_sub(GnssSubTrace ev, uint32_t v0, uint32_t v1) noexcept
{
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(ev), 0U, v0, v1);
}

void format_period_sec(int8_t interval, std::span<char> out) noexcept
{
    const uint32_t period_ms = core::services::GnssService::interval_to_period_ms(interval);
    if ((period_ms % kMsPerSec) == 0U) {
        (void)std::snprintf(out.data(), out.size(), "%lu", static_cast<unsigned long>(period_ms / kMsPerSec));
    } else {
        const uint32_t whole = period_ms / kMsPerSec;
        const uint32_t centi = (period_ms % kMsPerSec) / kCentiDiv;
        (void)std::snprintf(out.data(), out.size(), "%lu.%02lu", static_cast<unsigned long>(whole),
                            static_cast<unsigned long>(centi));
    }
}

bool out_cfg_differs_from_default(const data::GnssOutConfig &cfg) noexcept
{
    const auto def = data::GnssOutConfig::defaults();
    return (cfg.mask != def.mask) || (cfg.intervals != def.intervals);
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

/** 文本日志里可打印 ASCII 的下限（跳过空格，避免把分隔符带进名字）。 */
constexpr uint8_t kTagPrintableMin = 0x21U;
/** 文本日志里可打印 ASCII 的上限。 */
constexpr uint8_t kTagPrintableMax = 0x7EU;
constexpr uint8_t kCommaByte = static_cast<uint8_t>(',');
/** 帧标识缓冲区：`UNICORE_CMD ` + 命令名足够，24 字节。 */
constexpr size_t kFrameTagBytes = 24U;
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

/** 把 frame[start, size) 的 ASCII 名字拷进 out（最多 out.size()-1 字节），返回写入长度。 */
size_t copy_word(const std::span<const uint8_t> frame, size_t start, std::span<char> out) noexcept
{
    size_t len = 0U;
    for (size_t i = start; (i < frame.size()) && (len + 1U < out.size()); ++i) {
        const uint8_t byte = frame[i];
        if ((byte == kCommaByte) || (byte < kTagPrintableMin) || (byte > kTagPrintableMax)) {
            break;
        }
        out[len] = static_cast<char>(byte);
        ++len;
    }
    out[len] = '\0';
    return len;
}

/** 生成一句帧标识，如 `NMEA GNGGA`、`UNICORE VERSIONA`、`RTCM3 1005`、`UBX 0x01/0x07`（§2.11）。 */
void describe_frame(const demux::Decoder &decoder, std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    const std::string_view name{decoder.name};
    std::array<char, kFrameTagBytes> word{};
    if ((name == "NMEA") && (frame.size() > kNmeaTagLen)) {
        // GGA/GSA/... 五位消息名紧跟 '$' 与两位 talker，可直接展示。
        (void)copy_word(frame, kNmeaTagOffset, word);
        std::snprintf(out.data(), out.size(), "NMEA %s", word.data());
        return;
    }
    if ((name == "UnicoreHash") && (frame.size() > 2U)) {
        (void)copy_word(frame, 1U, word);
        std::snprintf(out.data(), out.size(), "UNICORE %s", word.data());
        return;
    }
    if ((name == "UnicoreResp") && (frame.size() > kCommandTagOffset)) {
        // "$command,<cmd>,response: ..."：命令名在第二个逗号之前。
        (void)copy_word(frame, kCommandTagOffset, word);
        std::snprintf(out.data(), out.size(), "UNICORE_CMD %s", word.data());
        return;
    }
    if ((name == "RTCM3") && (frame.size() > kRtcmTagOffset + 1U)) {
        const uint32_t msg = (static_cast<uint32_t>(frame[kRtcmTagOffset]) << kByteLowNibbleBits) |
                             (static_cast<uint32_t>(frame[kRtcmTagOffset + 1U]) >> kByteLowNibbleBits);
        std::snprintf(out.data(), out.size(), "RTCM3 %lu", static_cast<unsigned long>(msg & kRtcmMsgMask));
        return;
    }
    if ((name == "UBX") && (frame.size() > kUbxClassOffset + 1U)) {
        std::snprintf(out.data(), out.size(), "UBX 0x%02x/0x%02x", static_cast<unsigned>(frame[kUbxClassOffset]),
                      static_cast<unsigned>(frame[kUbxClassOffset + 1U]));
        return;
    }
    std::snprintf(out.data(), out.size(), "%s", decoder.name);
}

bool is_valid_rate_hz(uint8_t hz) noexcept
{
    return (hz == 1U) || (hz == 2U) || (hz == 5U) || (hz == 10U) || (hz == 20U);
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

::gnss::Family infer_probe_family(std::span<const uint8_t> bytes) noexcept
{
    if ((bytes.size() >= 2U) && (bytes[0] == 0xB5U) && (bytes[1] == 0x62U)) {
        return ::gnss::Family::kUblox;
    }
    if ((bytes.size() >= 3U) && (bytes[0] == 'L') && (bytes[1] == 'O') && (bytes[2] == 'G')) {
        return ::gnss::Family::kComnav;
    }
    if ((bytes.size() >= 3U) && (bytes[0] == 'V') && (bytes[1] == 'E') && (bytes[2] == 'R')) {
        return ::gnss::Family::kUnicore;
    }
    return ::gnss::Family::kUnknown;
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

fw::Result GnssModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kOk;
    }

    // CONFIG_RTK_GNSS_UART_NUM 在 Kconfig 中限制为 0..2，转换为 uart_port_t 安全。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t drv_err =
        uart_driver_install(port, static_cast<int>(kUartRxBufBytes), static_cast<int>(kUartTxBufBytes), 0, nullptr, 0);
    if (drv_err != ESP_OK) {
        return to_result(rtos::from_esp_err(drv_err));
    }

    uart_config_t uart_cfg{};
    uart_cfg.baud_rate = CONFIG_RTK_GNSS_DEFAULT_BAUD;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;
    const esp_err_t cfg_err = uart_param_config(port, &uart_cfg);
    if (cfg_err != ESP_OK) {
        (void)uart_driver_delete(port);
        return to_result(rtos::from_esp_err(cfg_err));
    }

    const esp_err_t pin_err =
        uart_set_pin(port, CONFIG_RTK_GNSS_TX_GPIO, CONFIG_RTK_GNSS_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (pin_err != ESP_OK) {
        (void)uart_driver_delete(port);
        return to_result(rtos::from_esp_err(pin_err));
    }

#if CONFIG_RTK_GNSS_POWER_GPIO >= 0
    // CONFIG_RTK_GNSS_POWER_GPIO 非负且来自 Kconfig（0..39），转换为 gpio_num_t 与移位均安全。
    const auto power_pin = static_cast<gpio_num_t>(CONFIG_RTK_GNSS_POWER_GPIO);
    const uint32_t off_level = (CONFIG_RTK_GNSS_POWER_ACTIVE_HIGH != 0) ? 0U : 1U;
    const esp_err_t level_err = gpio_set_level(power_pin, off_level);
    if (level_err != ESP_OK) {
        (void)uart_driver_delete(port);
        return to_result(rtos::from_esp_err(level_err));
    }
    gpio_config_t power_cfg{};
    power_cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(CONFIG_RTK_GNSS_POWER_GPIO);
    power_cfg.mode = GPIO_MODE_OUTPUT;
    power_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    power_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_cfg.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t gpio_err = gpio_config(&power_cfg);
    if (gpio_err != ESP_OK) {
        (void)uart_driver_delete(port);
        return to_result(rtos::from_esp_err(gpio_err));
    }
#endif

    const fw::Result pipe_res = to_result(context_.rtcm_pipe.init());
    if (!fw::is_ok(pipe_res)) {
        (void)uart_driver_delete(port);
        return pipe_res;
    }

    decoder_count_ = 0U;
    add_unique_decoder(decoders_, decoder_count_, &demux::nmea::kDecoder);
    add_unique_decoder(decoders_, decoder_count_, &demux::rtcm3::kDecoder);
    const auto dialects = ::gnss::enabled_dialects();
    for (const ::gnss::Dialect *dialect : dialects) {
        if (dialect == nullptr) {
            continue;
        }
        for (const demux::Decoder *dec : dialect->decoders) {
            add_unique_decoder(decoders_, decoder_count_, dec);
        }
    }

    const demux::Error demux_err =
        demux_.init(std::span<const demux::Decoder *const>{decoders_.data(), decoder_count_}, demux_buf_, sink_);
    if (demux_err != demux::Error::kOk) {
        (void)uart_driver_delete(port);
        return fw::Result::kInvalidState;
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
        const uint32_t *const begin = bauds_.data();
        const uint32_t *const end = bauds_.data() + baud_count_;
        if (std::find(begin, end, baud) == end) {
            bauds_[baud_count_] = baud;
            ++baud_count_;
        }
    }

    if (!controller_.init(dialects, std::span<const uint32_t>{bauds_.data(), baud_count_}, rover_cfg_, hint_,
                          timing_)) {
        (void)uart_driver_delete(port);
        return fw::Result::kInvalidState;
    }

    host_baud_ = CONFIG_RTK_GNSS_DEFAULT_BAUD;
    last_state_ = controller_.state();
    last_published_state_ = last_state_;
    prev_ctrl_stats_ = controller_.stats();
    context_.data.gnss_out_cfg.write(out_cfg_);
    trace_gnss_sub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
    initialized_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::start() noexcept
{
    if (!initialized_) {
        return fw::Result::kInvalidState;
    }
    const uint32_t now = rtos::now_ms();
    controller_.start(now);
    poll_controller(now);
    publish_status(now);
    return fw::Result::kOk;
}

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
    const demux::Error err = demux_.feed(std::span<const uint8_t>{buf.data(), count});
    if (err != demux::Error::kOk) {
        trace_gnss(GnssTrace::kState, static_cast<uint32_t>(controller_.state()), static_cast<uint32_t>(err));
    }
    const auto &dstats = demux_.stats();
    if (dstats.unknown_bytes > prev_unknown_bytes_) {
        fw::tlog(fw::TraceCat::kGnss, "%lu unknown bytes dropped",
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
    const bool tx_progress = pump_rtcm_pipe();

    const auto cur_state = controller_.state();
    if ((cur_state != last_published_state_) || ((now_ms - last_status_ms_) >= kStatusPeriodMs)) {
        publish_status(now_ms);
    }

    const bool had_full_rx = (last_rx_bytes_ == kUartReadChunkBytes);
    last_rx_bytes_ = 0U;
    const bool allow_tx = (cur_state == ::gnss::State::kRunning) || (cur_state == ::gnss::State::kConfiguring) ||
                          (cur_state == ::gnss::State::kProbing);
    const bool has_more_tx = (rtcm_tx_len_ > rtcm_tx_off_) || (context_.rtcm_pipe.available() > 0U);
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
        parser_.reset();
        context_.data.gnss_fix.write(::gnss::Fix{});
        controller_.request_reprobe(now);
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
        parser_.reset();
        context_.data.gnss_fix.write(::gnss::Fix{});
        controller_.stop(now);
        poll_controller(now);
        controller_.start(now);
        poll_controller(now);
        publish_status(now);
        result.status = fw::Result::kOk;
        return;

    case GnssVerb::kSetRate: {
        if ((cmd.arg_len != 1U) || !is_valid_rate_hz(cmd.args[0])) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        rover_cfg_.rate_hz = cmd.args[0];
        const fw::Result save_res = context_.storage.save_keys(gnss_store::kConfigKeys);
        if (!fw::is_ok(save_res)) {
            result.status = save_res;
            return;
        }
        controller_.request_reconfigure(now);
        poll_controller(now);
        publish_status(now);
        result.status = fw::Result::kOk;
        return;
    }

    case GnssVerb::kSend: {
        const auto st = controller_.state();
        if ((st != ::gnss::State::kRunning) && (st != ::gnss::State::kFailed)) {
            result.status = fw::Result::kInvalidState;
            return;
        }
        if ((cmd.arg_len == 0U) || (cmd.arg_len > cmd.args.size())) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        std::array<char, fw::kCommandArgBytes + 2U> line{};
        const auto len = static_cast<size_t>(cmd.arg_len);
        std::memcpy(line.data(), cmd.args.data(), len);
        line[len] = '\r';
        line[len + 1U] = '\n';
        const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
        const int written = uart_write_bytes(port, line.data(), len + 2U);
        if (written < 0) {
            result.status = fw::Result::kIoError;
            return;
        }
        uart_tx_bytes_ += static_cast<uint32_t>(written);
        result.status = fw::Result::kOk;
        return;
    }

    default: {
        if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetOut) {
            data::GnssOutConfig next{};
            if ((cmd.arg_len != data::GnssOutConfig::kBlobBytes) ||
                !fw::is_ok(data::GnssOutConfig::decode(std::span<const uint8_t>{cmd.args.data(), cmd.arg_len}, next))) {
                result.status = fw::Result::kInvalidArg;
                return;
            }
            out_cfg_ = next;
            context_.data.gnss_out_cfg.write(out_cfg_);
            const fw::Result save_res = context_.storage.save_keys(gnss_store::kConfigKeys);
            if (!fw::is_ok(save_res)) {
                result.status = save_res;
                return;
            }
            if (controller_.state() == ::gnss::State::kRunning) {
                apply_out_to_receiver();
            }
            trace_gnss_sub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
            result.status = fw::Result::kOk;
            return;
        }
        result.status = fw::Result::kNotSupported;
        return;
    }
    }
}

void GnssModule::handle_frame(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept
{
    // 文本日志不经中断路径：本函数只由 kIo 任务的 feed() 调用（ADR-0009 §2.11）。
    std::array<char, kLogTagBytes> tag{};
    describe_frame(decoder, frame, tag);
    fw::tlog(fw::TraceCat::kGnss, "%s, %lu bytes", tag.data(), static_cast<unsigned long>(frame.size()));
    fw::tlog_hex(fw::TraceCat::kGnss, frame);

    const uint32_t now = rtos::now_ms();
    controller_.on_frame(decoder, frame, now);
    if (&decoder == &demux::nmea::kDecoder) {
        const auto res = parser_.feed(frame, now);
        if (res == ::gnss::NmeaResult::kFix) {
            context_.data.gnss_fix.write(parser_.fix());
        } else if (res == ::gnss::NmeaResult::kSatsReady) {
            // GGA 触发卫星轮发布时已在同一次 feed() 中更新 fix_（G-A §4.1），须同步刷新定位槽。
            context_.data.gnss_fix.write(parser_.fix());
            context_.data.gnss_sats.write(parser_.sats());
        }
    }

    // ADR-0011 §5：6 类可订阅语句同步更新 6 KiB 最新帧缓存，并按会话掩码与周期写入 App typed FanoutRing。
    if ((&decoder == &demux::nmea::kDecoder) || (&decoder == &demux::unicore_hash::kDecoder)) {
        const auto type_opt = data::classify_gnss_sentence(frame);
        if (type_opt.has_value()) {
            const uint8_t type_idx = *type_opt;
            if (!context_.data.gnss_latest.update(type_idx, frame)) {
                trace_gnss_sub(GnssSubTrace::kAppDrop, 2U, type_idx);
            }
            uint32_t active_mask = 0U;
            std::array<uint32_t, data::kGnssAppMsgCount> periods_ms{};
            context_.data.gnss_app_sub.read_subscription(active_mask, periods_ms);
            if ((active_mask & (1U << type_idx)) == 0U) {
                // 无订阅者：FanoutRing::write 只计 no_subscriber，不写入环形缓冲；闸门复位，重新订阅后首句立即推送。
                static_cast<void>(context_.data.gnss_app_ring.write(type_idx, frame));
                push_gates_[type_idx].reset();
            } else {
                const uint32_t period = periods_ms[type_idx];
                auto &gate = push_gates_[type_idx];
                if (!gate.due(now, period)) {
                    context_.data.gnss_app_sub.add_skipped(type_idx);
                } else {
                    const fw::Result wres = context_.data.gnss_app_ring.write(type_idx, frame);
                    if (fw::is_ok(wres)) {
                        gate.on_pushed(now, period);
                    } else if (wres == fw::Result::kInvalidArg) {
                        context_.data.gnss_app_sub.add_ring_oversize(type_idx);
                        trace_gnss_sub(GnssSubTrace::kAppDrop, 0U, type_idx);
                    }
                }
            }
        }
    }
}

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
    fw::tlog(fw::TraceCat::kGnss, "%s rejected %lu bytes: %s", decoder.name, static_cast<unsigned long>(prefix.size()),
             text);
    fw::tlog_hex(fw::TraceCat::kGnss, prefix);
}

void GnssModule::poll_controller(uint32_t now_ms) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    for (size_t iter = 0U; iter < kMaxActionsPerPoll; ++iter) {
        const ::gnss::Action act = controller_.poll(now_ms);
        if (act.kind == ::gnss::Action::Kind::kNone) {
            break;
        }
        switch (act.kind) {
        case ::gnss::Action::Kind::kPower: {
            const fw::Result err = apply_power(act.on);
            if (!fw::is_ok(err)) {
                trace_gnss(GnssTrace::kState, static_cast<uint32_t>(controller_.state()), static_cast<uint32_t>(err));
            }
            break;
        }
        case ::gnss::Action::Kind::kSetBaud: {
            const uint32_t old_baud = host_baud_;
            const fw::Result err = apply_baud(act.baud);
            if (fw::is_ok(err)) {
                trace_gnss(GnssTrace::kBaud, act.baud, old_baud);
                fw::tlog(fw::TraceCat::kGnss, "baud %lu (was %lu)", static_cast<unsigned long>(act.baud),
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
            fw::tlog(fw::TraceCat::kGnss, "%s <- \"%.*s\"", ::gnss::model_name(controller_.detected().probe.model),
                     static_cast<int>(cmd_len), reinterpret_cast<const char *>(act.bytes.data()));
            const auto st = controller_.state();
            const auto cur_stats = controller_.stats();
            if ((st == ::gnss::State::kProbing) && (cur_stats.probes_sent > prev_ctrl_stats_.probes_sent)) {
                trace_gnss(GnssTrace::kProbeSend, static_cast<uint32_t>(infer_probe_family(act.bytes)), host_baud_);
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

    const auto cur_stats = controller_.stats();
    if (cur_stats.step_nacks > prev_ctrl_stats_.step_nacks) {
        trace_gnss(GnssTrace::kStepNack, step_idx_, 0U);
    }
    if (cur_stats.step_timeouts > prev_ctrl_stats_.step_timeouts) {
        trace_gnss(GnssTrace::kStepTimeout, step_idx_, step_retry_ + 1U);
    }

    const auto cur_state = controller_.state();
    if (cur_state != last_state_) {
        trace_gnss(GnssTrace::kState, static_cast<uint32_t>(cur_state), static_cast<uint32_t>(last_state_));
        fw::tlog(fw::TraceCat::kGnss, "state %s -> %s", ::gnss::state_name(last_state_), ::gnss::state_name(cur_state));
        if ((last_state_ == ::gnss::State::kProbing) && (cur_state == ::gnss::State::kConfiguring)) {
            const auto det = controller_.detected();
            const uint32_t baud = (det.baud != 0U) ? det.baud : host_baud_;
            trace_gnss(GnssTrace::kProbeHit, static_cast<uint32_t>(det.probe.model), baud);
            fw::tlog(fw::TraceCat::kGnss, "detected %s at %lu", ::gnss::model_name(det.probe.model),
                     static_cast<unsigned long>(baud));
            step_idx_ = 0U;
            step_retry_ = 0U;
            step_sent_once_ = false;
        } else if ((last_state_ == ::gnss::State::kConfiguring) && (cur_state == ::gnss::State::kRunning)) {
            if (step_sent_once_) {
                trace_gnss(GnssTrace::kStepAck, step_idx_, 0U);
            }
            persist_detected_hint();
            if (out_cfg_differs_from_default(out_cfg_)) {
                apply_out_to_receiver();
            }
        } else if ((last_state_ == ::gnss::State::kRunning) && (cur_state == ::gnss::State::kProbing)) {
            if (!manual_reprobe_ && (cur_stats.reprobes > prev_ctrl_stats_.reprobes)) {
                trace_gnss(GnssTrace::kSilence, timing_.silence_ms, 0U);
            }
            manual_reprobe_ = false;
        }
        last_state_ = cur_state;
    }
    prev_ctrl_stats_ = cur_stats;
}

bool GnssModule::pump_rtcm_pipe() noexcept
{
    const auto st = controller_.state();
    const bool allow_tx =
        (st == ::gnss::State::kRunning) || (st == ::gnss::State::kConfiguring) || (st == ::gnss::State::kProbing);
    if (!allow_tx) {
        if (rtcm_tx_len_ > rtcm_tx_off_) {
            context_.rtcm_pipe.note_discarded(rtcm_tx_len_ - rtcm_tx_off_);
            rtcm_tx_len_ = 0U;
            rtcm_tx_off_ = 0U;
        }
        context_.rtcm_pipe.discard();
        return false;
    }
    if (rtcm_tx_off_ >= rtcm_tx_len_) {
        rtcm_tx_len_ = context_.rtcm_pipe.read(rtcm_tx_buf_);
        rtcm_tx_off_ = 0U;
    }
    if (rtcm_tx_off_ >= rtcm_tx_len_) {
        context_.rtcm_pipe.set_pending(0U, false);
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
        context_.rtcm_pipe.note_written(bytes);
        context_.rtcm_pipe.set_pending(rtcm_tx_len_ - rtcm_tx_off_, false);
        return true;
    }
    if (written == 0) {
        // 驱动 TX 环暂满：保留块与偏移，下轮继续，不丢块也不重读管道。
        context_.rtcm_pipe.set_pending(rtcm_tx_len_ - rtcm_tx_off_, true);
        return false;
    }
    const size_t lost = rtcm_tx_len_ - rtcm_tx_off_;
    rtcm_tx_off_ = 0U;
    rtcm_tx_len_ = 0U;
    context_.rtcm_pipe.note_write_error(lost);
    trace_gnss(GnssTrace::kState, static_cast<uint32_t>(st), static_cast<uint32_t>(fw::Result::kIoError));
    fw::tlog(fw::TraceCat::kGnss, "RTCM UART write error (%d), dropped %lu pending bytes", written,
             static_cast<unsigned long>(lost));
    return false;
}

void GnssModule::publish_status(uint32_t now_ms) noexcept
{
    GnssStatus st{};
    st.enabled = initialized_;
    st.state = controller_.state();
    const auto &det = controller_.detected();
    if (det.dialect != nullptr) {
        st.family = det.dialect->family;
        st.model = det.probe.model;
        st.fw_version = det.probe.fw_version;
        st.caps = det.caps;
    }
    st.host_baud = host_baud_;
    st.ctrl = controller_.stats();
    st.nmea = parser_.stats();
    st.uart_rx_bytes = uart_rx_bytes_;
    st.uart_tx_bytes = uart_tx_bytes_;
    const auto &dstats = demux_.stats();
    st.demux_unknown = dstats.unknown_bytes;
    st.demux_resyncs = dstats.resyncs;
    st.pipe = context_.rtcm_pipe.stats();

    context_.data.gnss_status.write(st);
    last_status_ms_ = now_ms;
    last_published_state_ = st.state;
}

void GnssModule::persist_detected_hint() noexcept
{
    const auto &det = controller_.detected();
    if (det.dialect == nullptr) {
        return;
    }
    const ::gnss::Family det_family = det.dialect->family;
    const ::gnss::Model det_model = det.probe.model;
    const uint32_t det_baud = (det.baud != 0U) ? det.baud : host_baud_;
    if ((det_family == hint_.family) && (det_model == hint_.model) && (det_baud == hint_.baud)) {
        return;
    }
    hint_.family = det_family;
    hint_.model = det_model;
    hint_.baud = det_baud;
    const fw::Result save_res = context_.storage.save_keys(gnss_store::kCacheKeys);
    if (!fw::is_ok(save_res)) {
        // StorageModule::save_keys 内部已发 kStore trace；此处保留 hint_ 内存值避免每轮重复写盘。
    }
}

void GnssModule::apply_out_to_receiver() noexcept
{
    const auto &det = controller_.detected();
    if (det.dialect == nullptr) {
        return;
    }
    const auto fam = det.dialect->family;
    if ((fam != ::gnss::Family::kUnicore) && (fam != ::gnss::Family::kComnav)) {
        return;
    }
    static constexpr std::array<const char *, data::kGnssAppMsgCount> kUnicoreCmdNames = {
        "GPGGA", "SATSINFOA", "HEADINGA", "GPGST", "GPRMC", "GPGSA",
    };
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
        if ((i == 2U) && ((det.caps & ::gnss::cap::kHeading) == 0U)) {
            continue;
        }
        if ((fam == ::gnss::Family::kComnav) && ((i == 1U) || (i == 2U))) {
            continue;
        }
        std::array<char, kOutCmdLineBytes> line{};
        int len = 0;
        const bool enabled = (out_cfg_.mask & (1U << i)) != 0U;
        if (fam == ::gnss::Family::kUnicore) {
            if (!enabled) {
                len = std::snprintf(line.data(), line.size(), "UNLOG COM%d %s\r\n", CONFIG_RTK_GNSS_PORT,
                                    kUnicoreCmdNames[i]);
            } else {
                std::array<char, kOutPeriodTextBytes> period{};
                format_period_sec(out_cfg_.intervals[i], period);
                len = std::snprintf(line.data(), line.size(), "%s COM%d %s\r\n", kUnicoreCmdNames[i],
                                    CONFIG_RTK_GNSS_PORT, period.data());
            }
        } else {
            if (!enabled) {
                len = std::snprintf(line.data(), line.size(), "UNLOG COM%d %s\r\n", CONFIG_RTK_GNSS_PORT,
                                    kUnicoreCmdNames[i]);
            } else {
                std::array<char, kOutPeriodTextBytes> period{};
                format_period_sec(out_cfg_.intervals[i], period);
                len = std::snprintf(line.data(), line.size(), "LOG COM%d %s ONTIME %s\r\n", CONFIG_RTK_GNSS_PORT,
                                    kUnicoreCmdNames[i], period.data());
            }
        }
        if (len > 0) {
            const int written = uart_write_bytes(port, line.data(), static_cast<size_t>(len));
            if (written > 0) {
                uart_tx_bytes_ += static_cast<uint32_t>(written);
            }
        }
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
    if (in[0] > static_cast<uint8_t>(::gnss::Family::kComnav)) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.hint_.family = static_cast<::gnss::Family>(in[0]);
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
    if (in[0] > static_cast<uint8_t>(::gnss::Model::kK803)) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.hint_.model = static_cast<::gnss::Model>(in[0]);
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
    app_context().gnss.rover_cfg_.rate_hz = in[0];
    return fw::Result::kOk;
}

fw::Result GnssModule::save_rate(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = app_context().gnss.rover_cfg_.rate_hz;
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_gsv(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || (in[0] > 1U)) {
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.rover_cfg_.gsv = (in[0] != 0U);
    return fw::Result::kOk;
}

fw::Result GnssModule::save_gsv(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = app_context().gnss.rover_cfg_.gsv ? 1U : 0U;
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::load_out(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    data::GnssOutConfig parsed{};
    if (!fw::is_ok(data::GnssOutConfig::decode(in, parsed))) {
        trace_gnss_sub(GnssSubTrace::kOutCfg, app_context().gnss.out_cfg_.mask, 1U);
        return fw::Result::kInvalidArg;
    }
    app_context().gnss.out_cfg_ = parsed;
    return fw::Result::kOk;
}

fw::Result GnssModule::save_out(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    return app_context().gnss.out_cfg_.encode(out, len);
}

} // namespace app
