// GNSS 模块（kIo 执行器）：串口接收分流、NMEA 解析、芯片探测/配置状态机、RTCM 管道写回与 gnss 命令组（ADR-0010 §2.7）。
// 线程模型：init() 启动前单线程；start()/wait()/poll()/on_command 仅 kIo 执行器任务调用；keys/load/save 由 storage
// 在启动期与 kIo 写回时调用。 均非 ISR；不分配堆（demux 缓冲、控制器、NMEA 解析器与命令表均为静态存储）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "cli/console.hpp"
#include "def/gnss_def.hpp"
#include "demux/demux.hpp"
#include "fw/command.hpp"
#include "fw/result.hpp"
#include "fw/store_key.hpp"
#include "gnss/controller.hpp"
#include "gnss/nmea_parser.hpp"
#include "gnss/types.hpp"

namespace app {
struct AppContext;

/**
 * GNSS 模块：拥有 UART 收发、StreamDemux、NmeaParser、Controller 与 NVS 提示/配置状态。
 * 串口读阻塞放在 wait(max_ms) 中作为 kIo 执行器的 idle 等待；poll(now_ms) 非阻塞驱动状态机与 RTCM 下发。
 */
class GnssModule final {
public:
    static constexpr size_t kDemuxBufBytes = 4096U;
    static constexpr size_t kUartReadChunkBytes = 512U;
    static constexpr size_t kUartRxBufBytes = 4096U;
    static constexpr size_t kUartTxBufBytes = 1024U;
    static constexpr size_t kRtcmTxChunkBytes = 512U;
    static constexpr size_t kMaxBaudCandidates = 7U;
    static constexpr uint32_t kStatusPeriodMs = 500U;

    constexpr explicit GnssModule(AppContext &context) noexcept : context_(context) {}
    ~GnssModule() = default;
    GnssModule(const GnssModule &) = delete;
    GnssModule &operator=(const GnssModule &) = delete;
    GnssModule(GnssModule &&) = delete;
    GnssModule &operator=(GnssModule &&) = delete;

    /** 启动前单线程：安装 UART 驱动、配置电源 GPIO、初始化 RTCM 管道、Demux 与控制器。幂等。 */
    [[nodiscard]] fw::Result init() noexcept;

    /** kIo 任务首次 poll 前调用一次：启动 GNSS 上电探测流程并发布初始状态。 */
    [[nodiscard]] fw::Result start() noexcept;

    /** kIo 空闲等待钩子：带超时读 GNSS UART 并送入 StreamDemux，触发帧回调。仅 kIo 调用，非 ISR。 */
    void wait(uint32_t max_ms) noexcept;

    /** 仅 kIo 调用，非阻塞：执行控制器动作、排空 RTCM 管道到 UART、定期发布状态快照与探测落盘。 */
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;

    /** 仅 kIo 调用：处理 gnss probe/reset/rate/send 命令。 */
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    // 适配 fw::StoreBinding：表为 constexpr，通过 app_context().gnss 定位单例实例。
    static fw::Result load_family(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_family(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    static fw::Result load_model(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_model(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    static fw::Result load_baud(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_baud(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    static fw::Result load_rate(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_rate(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    static fw::Result load_gsv(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_gsv(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;

private:
    class DemuxSink final : public demux::Sink {
    public:
        constexpr explicit DemuxSink(GnssModule *owner) noexcept : owner_(owner) {}
        void on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> frame) noexcept override;
        void on_reject(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> prefix,
                       demux::RejectCause cause, bool last) noexcept override;

    private:
        GnssModule *owner_ = nullptr;
    };

    void handle_frame(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept;
    void handle_reject(const demux::Decoder &decoder, std::span<const uint8_t> prefix, demux::RejectCause cause,
                       bool last) noexcept;
    void poll_controller(uint32_t now_ms) noexcept;
    [[nodiscard]] bool pump_rtcm_pipe() noexcept;
    void publish_status(uint32_t now_ms) noexcept;
    void persist_detected_hint() noexcept;
    [[nodiscard]] fw::Result apply_power(bool on) noexcept;
    [[nodiscard]] fw::Result apply_baud(uint32_t baud) noexcept;

    AppContext &context_;
    DemuxSink sink_{this};
    demux::Demux demux_{};
    ::gnss::NmeaParser parser_{};
    ::gnss::Controller controller_{};
    ::gnss::ControllerStats prev_ctrl_stats_{};
    /** 上一次已上报的未知字节计数，用于按增量输出 `GNSS N unknown bytes dropped`（§2.11）。 */
    uint32_t prev_unknown_bytes_ = 0U;
    ::gnss::Hint hint_{};
    ::gnss::Timing timing_{};
    ::gnss::RoverConfig rover_cfg_{
        .rate_hz = static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ),
        .gsv = true,
        .port = static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT),
        .port_baud = 0U,
    };
    std::array<const demux::Decoder *, demux::kMaxDecoders> decoders_{};
    size_t decoder_count_ = 0U;
    std::array<uint8_t, kDemuxBufBytes> demux_buf_{};
    std::array<uint8_t, kRtcmTxChunkBytes> rtcm_tx_buf_{};
    size_t rtcm_tx_len_ = 0U;
    size_t rtcm_tx_off_ = 0U;
    std::array<uint32_t, kMaxBaudCandidates> bauds_{};
    size_t baud_count_ = 0U;

    uint32_t host_baud_ = CONFIG_RTK_GNSS_DEFAULT_BAUD;
    uint32_t uart_rx_bytes_ = 0U;
    uint32_t uart_tx_bytes_ = 0U;
    size_t last_rx_bytes_ = 0U;
    uint32_t last_status_ms_ = 0U;
    ::gnss::State last_state_ = ::gnss::State::kOff;
    ::gnss::State last_published_state_ = ::gnss::State::kOff;
    uint32_t step_idx_ = 0U;
    uint32_t step_retry_ = 0U;
    bool step_sent_once_ = false;
    bool manual_reprobe_ = false;
    bool initialized_ = false;
};

/** 返回 gnss 顶层命令表（仅在 app_main 启动控制台时调用）。 */
[[nodiscard]] std::span<const cli::Cmd> gnss_cmds() noexcept;
/** 返回 gnss 子命令动词表，供顶层 help 索引（仅在 app_main 启动控制台时调用）。 */
[[nodiscard]] std::span<const cli::Verb> gnss_verbs() noexcept;

} // namespace app
