// 链路模块独占会话、协议服务与 RX 调度状态。
// init 仅启动前；其余操作仅 kLink；均非 ISR。
#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <span>

#include "app_config.hpp"
#include "def/rtcm_def.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if RTK_GNSS_ENABLED
#include "gnss/types.hpp"
#endif
#include "demux/decoders/rtcm3.hpp"
#include "demux/static_demux.hpp"
#include "spp/spp_transport.hpp"
#include "cli/console.hpp"
#include "core/alarm_source.hpp"
#include "core/command_router.hpp"
#include "core/link_manager.hpp"
#include "core/link_service.hpp"
#include "core/services/alarm_service.hpp"
#include "core/services/config_service.hpp"
#include "core/services/device_service.hpp"
#include "core/services/pair_service.hpp"
#include "proto/demux_frame.hpp"
#include "spp_server.h"

namespace app {
struct AppContext;
struct ModuleTestAccess;
/** 链路模块（kLink 执行器）：SPP 事件、分流器、路由与统计的唯一所有者；仅链路任务使用。 */
class BtLinkModule final {
public:
    constexpr explicit BtLinkModule(AppContext &context) noexcept : context_(context) {}
    ~BtLinkModule() = default;
    BtLinkModule(const BtLinkModule &) = delete;
    BtLinkModule &operator=(const BtLinkModule &) = delete;
    BtLinkModule(BtLinkModule &&) = delete;
    BtLinkModule &operator=(BtLinkModule &&) = delete;
    /** 启动前单线程，非 ISR；构造服务且不分配堆，失败返回 kInvalidState。 */
    [[nodiscard]] fw::Result init() noexcept;
    /** init 成功后仅 kLink 调用，非 ISR、非阻塞；发布快照，返回是否仍有 RX 积压。 */
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    /** init 成功后仅 kLink 调用，非 ISR；白名单清除可同步写 NVS，结果写入 result。 */
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /** 仅 kLink，非 ISR；至多等待 max_ms，先处理生命周期事件。 */
    void wait(uint32_t max_ms) noexcept;
    /** 仅 kLink 或启动前，非 ISR；返回直推 RTCM 累计统计快照。 */
    [[nodiscard]] const RtcmLinkSnapshot &rtcm_snapshot() const noexcept
    {
        return rtcm_snapshot_;
    }

private:
    friend struct ModuleTestAccess;
    using SessionDemux = demux::StaticDemux<proto::demux_frame::kDecoder, demux::rtcm3::kDecoder>;
    static constexpr uint8_t kProductIndex = SessionDemux::index_of<proto::demux_frame::kDecoder>();
    static constexpr uint8_t kRtcmIndex = SessionDemux::index_of<demux::rtcm3::kDecoder>();
    static constexpr size_t kRxChunkBytes = kLinkRxBufBytes;
    static constexpr size_t kRxBudgetBytes = kLinkRxBufBytes;
    class SessionSink final : public demux::Sink {
    public:
        constexpr SessionSink() noexcept = default;
        // 仅 kLink，非 ISR；OPEN/CLOSE 时复位元数据，owner 覆盖本会话生命周期。
        void bind(BtLinkModule &owner, core::SessionHandle handle) noexcept;
        void on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> wire) noexcept override;
        [[nodiscard]] bool healthy() const noexcept
        {
            return healthy_;
        }

    private:
        BtLinkModule *owner_ = nullptr;
        core::SessionHandle handle_ = 0;
        bool healthy_ = true;
    };
    struct SessionEntry {
        bool used = false;
        core::SessionHandle handle = 0;
        core::BtAddr addr{};
        bool drain_pending = false;
        std::array<uint8_t, kRxChunkBytes> rx{};
        size_t rx_offset = 0;
        size_t rx_size = 0;
        SessionSink sink{};
        SessionDemux demux{};
        bool demux_ready = false;
        // 分流器累计统计的上报基线，仅 kLink 读写；会话复用时归零。
        uint32_t seen_resyncs = 0;
        uint32_t seen_unknown = 0;
    };

    [[nodiscard]] SessionEntry *session_find(core::SessionHandle handle) noexcept;
    void session_forget(core::SessionHandle handle) noexcept;
    void session_record(core::SessionHandle handle, const core::BtAddr &addr) noexcept;
    [[nodiscard]] core::LinkService &link_service() noexcept;
    void collect_whitelist_dirty() noexcept;
    void handle_event(const spp_server_evt_t &evt, uint32_t now_ms) noexcept;
    void handle_open(const spp_server_evt_t &evt, uint32_t now_ms) noexcept;
    void handle_close(core::SessionHandle handle) noexcept;
    void handle_data(core::SessionHandle handle) noexcept;
    void handle_rtcm_frame(core::SessionHandle handle, std::span<const uint8_t> wire) noexcept;
    [[nodiscard]] bool poll_and_check_ready(uint32_t now_ms) noexcept;
    static void on_rx_wake(void *ctx) noexcept;
    void drain_rx(uint32_t round_start_ms) noexcept;
    [[nodiscard]] bool drain_session(SessionEntry &entry, size_t index, size_t &budget,
                                     uint32_t round_start_ms) noexcept;
    [[nodiscard]] bool feed_one(SessionEntry &entry, size_t &budget, uint32_t round_start_ms) noexcept;
    void sync_stream() noexcept;
    void publish_link(uint32_t now_ms) noexcept;
    void apply_rtcm_reset() noexcept;
#if RTK_GNSS_ENABLED
    void sync_gnss_info(uint32_t now_ms) noexcept;
#endif
    void set_alarm(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    void clear_whitelist(fw::CommandResult &result) noexcept;
    void edit_whitelist(const fw::Command &cmd, bool add, fw::CommandResult &result) noexcept;
    void persist_whitelist(fw::CommandResult &result) noexcept;

    AppContext &context_;
    uint32_t last_tick_ms_ = 0;
    uint32_t reset_version_ = 0;
    uint32_t round_start_ms_ = 0;
    DemuxStats retired_demux_{};
    core::LinkManager link_manager_{};
    core::AlarmSource alarm_{};
    spp::SppTransport transport_{};
    core::SessionHandle stream_handle_ = 0;
    // optional 在 init 按依赖次序原地构造，生命周期覆盖整个运行期，不分配堆。
    std::optional<core::services::DeviceService> device_service_{};
    std::optional<core::services::ConfigService> config_service_{};
    std::optional<core::services::AlarmService> alarm_service_{};
    std::optional<core::services::PairService> pair_service_{};
    core::CommandRouter router_{};
    std::optional<core::LinkService> service_{};
    std::array<SessionEntry, kMaxSessions> sessions_{};
    size_t rx_cursor_ = 0;
    RtcmLinkSnapshot rtcm_snapshot_{};
    std::atomic<TaskHandle_t> wake_task_{nullptr};
#if RTK_GNSS_ENABLED
    uint32_t gnss_fix_version_ = 0;
    uint32_t gnss_status_version_ = 0;
    ::gnss::Fix cached_gnss_fix_{};
#endif
};
/** 链路模块的只读控制台命令表（bt/alarm）；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> link_cmds() noexcept;
} // namespace app
