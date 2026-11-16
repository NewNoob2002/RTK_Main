// GNSS 模块（kIo 执行器）：串口接收分流、NMEA 解析、芯片探测/配置状态机、RTCM 管道写回与 gnss 命令组（ADR-0010 §2.7）。
// 线程模型：init() 启动前单线程；start()/wait()/poll()/on_command 仅 kIo 执行器任务调用；keys/load/save 由 storage
// 在启动期与 kIo 写回时调用。 均非 ISR；不分配堆（demux 缓冲、控制器、NMEA 解析器与命令表均为静态存储）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "cli/Console.hpp"
#include "data/GnssOutput.hpp"
#include "demux/Demux.hpp"
#include "fw/Command.hpp"
#include "fw/Result.hpp"
#include "gnss/Controller.hpp"
#include "gnss/NmeaParser.hpp"

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

    constexpr GnssModule() noexcept = default;
    constexpr explicit GnssModule(AppContext &context) noexcept : context_(&context) {}
    void bind_context(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~GnssModule() = default;
    GnssModule(const GnssModule &) = delete;
    GnssModule &operator=(const GnssModule &) = delete;
    GnssModule(GnssModule &&) = delete;
    GnssModule &operator=(GnssModule &&) = delete;

    /** 仅启动绑定回滚、消费者尚未启动时调用；清除 demux 外供引用，非 ISR。 */
    void unbind_storage() noexcept
    {
        demux_buf_ = {};
    }

    /** 启动前单线程：绑定外供 4 KiB demux 环缓冲。 */
    [[nodiscard]] fw::Status bind_storage(std::span<uint8_t> demux_buf) noexcept
    {
        if (initialized_) {
            return fw::Status::kInvalidState;
        }
        if ((demux_buf.data() == nullptr) || (demux_buf.size() < kDemuxBufBytes)) {
            return fw::Status::kInvalidArg;
        }
        demux_buf_ = demux_buf.first(kDemuxBufBytes);
        return fw::Status::kOk;
    }

    /** 启动前单线程：安装 UART 驱动、配置电源 GPIO、初始化 RTCM 管道、Demux 与控制器。幂等。 */
    [[nodiscard]] fw::Result init() noexcept;

    /** 启动失败且 IO 已确认停稳后调用；检查 UART 删除结果，封存存储保留至重启，非 ISR。 */
    [[nodiscard]] fw::Status stop_boot() noexcept;

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
    static fw::Result load_out(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_out(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;

private:
    class DemuxSink final : public demux::Sink {
    public:
        constexpr DemuxSink() noexcept = default;
        constexpr explicit DemuxSink(GnssModule *owner) noexcept : owner_(owner) {}
        void bind(GnssModule *owner) noexcept
        {
            owner_ = owner;
        }
        void on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> frame) noexcept override;
        void on_reject(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> prefix,
                       demux::RejectCause cause, bool last) noexcept override;

    private:
        GnssModule *owner_ = nullptr;
    };
    static_assert(std::is_trivially_destructible_v<DemuxSink>, "DemuxSink must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<demux::Demux>, "Demux must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<::gnss::NmeaParser>, "NmeaParser must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<::gnss::Controller>, "Controller must be trivially destructible");

    static_assert(std::is_trivially_destructible_v<::gnss::RoverConfig>, "RoverConfig must be trivially destructible");
    ::gnss::RoverConfig &ensure_rover_cfg() noexcept
    {
        if (!rover_cfg_.has_value()) {
            rover_cfg_.emplace();
        }
        return *rover_cfg_;
    }

    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    void handle_frame(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept;
    void handle_reject(const demux::Decoder &decoder, std::span<const uint8_t> prefix, demux::RejectCause cause,
                       bool last) noexcept;
    void poll_controller(uint32_t now_ms) noexcept;
    [[nodiscard]] bool pump_rtcm_pipe() noexcept;
    void publish_status(uint32_t now_ms) noexcept;
    void persist_detected_hint() noexcept;
    void apply_out_to_receiver() noexcept;
    [[nodiscard]] fw::Result apply_power(bool on) noexcept;
    [[nodiscard]] fw::Result apply_baud(uint32_t baud) noexcept;

    // 仅 init 已原地构造后使用；运行期不 reset，缺失表示内部生命周期约束被破坏。
    [[nodiscard]] ::gnss::Controller &controller() noexcept;
    [[nodiscard]] ::gnss::NmeaParser &parser() noexcept;
    [[nodiscard]] demux::Demux &stream_demux() noexcept;
    [[nodiscard]] fw::Result init_uart() noexcept;
    void send_raw(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    void update_controller_status() noexcept;
    [[nodiscard]] fw::Result init_failed(fw::Result original) noexcept;
    AppContext *context_ = nullptr;
    std::optional<DemuxSink> sink_{};
    std::optional<demux::Demux> demux_{};
    std::optional<::gnss::NmeaParser> parser_{};
    std::optional<::gnss::Controller> controller_{};
    ::gnss::ControllerStats prev_ctrl_stats_{};
    /** 上一次已上报的未知字节计数，用于按增量输出 `GNSS N unknown bytes dropped`（§2.11）。 */
    uint32_t prev_unknown_bytes_ = 0U;
    ::gnss::Hint hint_{};
    std::optional<::gnss::Timing> timing_{};
    std::optional<::gnss::RoverConfig> rover_cfg_{};
    data::GnssOutConfig out_cfg_{};
    std::array<data::GnssPushGate, data::kGnssAppMsgCount> push_gates_{};
    std::array<const demux::Decoder *, demux::kMaxDecoders> decoders_{};
    size_t decoder_count_ = 0U;
    std::span<uint8_t> demux_buf_{};
    std::array<uint8_t, kRtcmTxChunkBytes> rtcm_tx_buf_{};
    size_t rtcm_tx_len_ = 0U;
    size_t rtcm_tx_off_ = 0U;
    std::array<uint32_t, kMaxBaudCandidates> bauds_{};
    size_t baud_count_ = 0U;

    uint32_t host_baud_ = 0U;
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
    bool rate_loaded_ = false;
    bool gsv_loaded_ = false;
    bool initialized_ = false;
    bool uart_installed_ = false;
};

/** 返回 gnss 顶层命令表（仅在 app_main 启动控制台时调用）。 */
[[nodiscard]] std::span<const cli::Cmd> gnss_cmds() noexcept;
/** 返回 gnss 子命令动词表，供顶层 help 索引（仅在 app_main 启动控制台时调用）。 */
[[nodiscard]] std::span<const cli::Verb> gnss_verbs() noexcept;

} // namespace app
