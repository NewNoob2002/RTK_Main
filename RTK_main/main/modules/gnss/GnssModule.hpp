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
#include "def/GnssDef.hpp"
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
    /**
     * @brief  gnss probe：标记人工重探，清空最近定位并让控制器重新探测
     * @param  now_ms: 当前时间
     * @param  result: 写入命令结果（kOk；参数校验在 on_command）
     * @note   仅 kIo 调用，非阻塞。
     */
    void cmd_probe(uint32_t now_ms, fw::CommandResult &result) noexcept;
    /**
     * @brief  gnss reset：清空最近定位后停再启动控制器
     * @param  now_ms: 当前时间
     * @param  result: 写入命令结果（kOk；参数校验在 on_command）
     * @note   仅 kIo 调用，非阻塞。
     */
    void cmd_reset(uint32_t now_ms, fw::CommandResult &result) noexcept;
    /**
     * @brief  gnss rate：两阶段提交更新输出频率（ADR-0012 §2.4）
     * @param  cmd: 指令，args[0] 必须是支持的频率
     * @param  now_ms: 当前时间
     * @param  result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
     * @note   仅 kIo 调用，非阻塞；NVS 写只在非 kRunning 的直落盘路径发生。
     */
    void cmd_set_rate(const fw::Command &cmd, uint32_t now_ms, fw::CommandResult &result) noexcept;
    /**
     * @brief  gnss setout：两阶段提交更新输出表
     * @param  cmd: 指令，args 为 GnssOutConfig 编码块
     * @param  now_ms: 当前时间
     * @param  result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
     * @note   仅 kIo 调用，非阻塞。
     */
    void cmd_set_out(const fw::Command &cmd, uint32_t now_ms, fw::CommandResult &result) noexcept;

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
        void onFrame(const demux::Decoder &decoder, uint8_t index,
                     std::span<const uint8_t> frame) noexcept override;
        void onReject(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> prefix,
                      demux::RejectCause cause, bool last) noexcept override;

    private:
        GnssModule *owner_ = nullptr;
    };
    static_assert(std::is_trivially_destructible_v<DemuxSink>,
                  "DemuxSink must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<demux::Demux>,
                  "Demux must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<::gnss::NmeaParser>,
                  "NmeaParser must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<::gnss::Controller>,
                  "Controller must be trivially destructible");

    static_assert(std::is_trivially_destructible_v<::gnss::RoverConfig>,
                  "RoverConfig must be trivially destructible");
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
    void handle_reject(const demux::Decoder &decoder, std::span<const uint8_t> prefix,
                       demux::RejectCause cause, bool last) noexcept;
    void poll_controller(uint32_t now_ms) noexcept;
    /**
     * @brief  读回期间把一帧转成一行文本追加到 gnss_readback_text（ADR-0012 §2.7）
     * @param  decoder: 该帧的解码器
     * @param  frame: 整帧字节
     * @note   仅 kIo 调用；命中即计数，容量不足时丢弃该行并置 truncated。
     */
    void append_readback_line(const demux::Decoder &decoder,
                              std::span<const uint8_t> frame) noexcept;
    /**
     * @brief  发起一次配置读回，并把被拒原因就地发布
     * @param  now_ms: 当前时间
     * @param  result: 写入命令结果（kOk / kBusy / kNotSupported）
     * @note   仅 kIo 调用；成功时清空上一轮文本与计数。
     */
    void start_readback(uint32_t now_ms, fw::CommandResult &result) noexcept;
    /**
     * @brief  发布一次读回结论（seq 严格递增，保证控制台一定能看到变化）
     * @param  lines: 命中行数
     * @param  len: 文本有效字节
     * @param  supported: 驱动是否支持读回
     * @param  truncated: 是否有行因容量不足被丢弃
     * @param  busy: 是否因状态不对被拒
     * @note   仅 kIo 调用；文本本体已在 gnss_readback_text 中。
     */
    void publish_readback(uint16_t lines, uint16_t len, bool supported, bool truncated,
                          bool busy) noexcept;
    /**
     * @brief  把 RTCM 管道中的待发字节写到 GNSS UART
     * @retval true 管道仍有积压
     * @retval false 已排空或写失败（失败时丢弃积压并记 W 级日志）
     * @note   仅 kIo 调用，非阻塞。
     */
    [[nodiscard]] bool pump_rtcm_pipe() noexcept;
    void publish_status(uint32_t now_ms) noexcept;
    void persist_detected_hint() noexcept;
    [[nodiscard]] fw::Result apply_power(bool on) noexcept;
    [[nodiscard]] fw::Result apply_baud(uint32_t baud) noexcept;

    // 仅 init 已原地构造后使用；运行期不 reset，缺失表示内部生命周期约束被破坏。
    [[nodiscard]] ::gnss::Controller &controller() noexcept;
    [[nodiscard]] ::gnss::NmeaParser &parser() noexcept;
    [[nodiscard]] demux::Demux &stream_demux() noexcept;
    [[nodiscard]] fw::Result init_uart() noexcept;
    void send_raw(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /** 由当前配置与接收机能力生成输出计划（纯计算，不碰串口）。 */
    [[nodiscard]] ::gnss::OutputPlan make_plan() noexcept;
    /** 输出/频率变更的落地检查：成功写 NVS，失败回滚并重下发旧计划（ADR-0012 §2.4）。 */
    void tick_pending_apply(uint32_t now_ms) noexcept;
    void update_controller_status() noexcept;
    [[nodiscard]] fw::Result init_failed(fw::Result original) noexcept;
    AppContext *context_ = nullptr;
    std::optional<DemuxSink> sink_{};
    std::optional<demux::Demux> demux_{};
    std::optional<::gnss::NmeaParser> parser_{};
    std::optional<::gnss::Controller> controller_{};
    ::gnss::ControllerStats prev_ctrl_stats_{};
    /** 读回状态（ADR-0012 §2.7）：文本在 gnss_readback_text，这里只留长度/计数与 seq 账本。 */
    uint16_t readback_len_ = 0U;
    uint16_t readback_lines_ = 0U;
    bool readback_truncated_ = false;
    /** 当前这轮读回开始时的 stats 值：与 stats 不等即表示刚收尾。 */
    uint32_t readback_base_seq_ = 0U;
    /** 已发布的 seq：失败路径会“预支”到 stats+1，发布时取 max 以免与控制台记录相撞。 */
    uint32_t readback_pub_seq_ = 0U;
    /** 上一次已上报的未知字节计数，用于按增量输出 `GNSS N unknown bytes dropped`（§2.11）。 */
    uint32_t prev_unknown_bytes_ = 0U;
    ::gnss::Hint hint_{};
    std::optional<::gnss::Timing> timing_{};
    std::optional<::gnss::RoverConfig> rover_cfg_{};
    data::GnssOutConfig out_cfg_{};
    /** 待落地的变更：等控制器 output_seq 前进后再决定写 NVS 还是回滚。 */
    enum class PendingApply : uint8_t {
        kNone = 0,
        kOut = 1,
        kRate = 2,
    };
    PendingApply pending_apply_ = PendingApply::kNone;
    GnssApply last_apply_ = GnssApply::kNone;
    uint32_t apply_seq_ = 0U;
    data::GnssOutConfig pending_prev_out_{};
    uint8_t pending_prev_rate_ = 0U;
    uint32_t pending_seq_ = 0U;
    uint32_t pending_deadline_ms_ = 0U;
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
