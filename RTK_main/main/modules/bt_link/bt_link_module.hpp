// 链路模块独占会话、协议服务与 RX 调度状态。
// init 仅启动前；其余操作仅 kLink，start_tx 仅 app_main；均非 ISR。
#pragma once

#include <array>
#include <optional>
#include <span>

#include "app_config.hpp"
#include "def/rtcm_def.hpp"
#if RTK_GNSS_ENABLED
#include "gnss/types.hpp"
#endif
#include "modules/bt_link/async_tx.hpp"
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
    /** app_main 在执行器 start 前调用，非 ISR；静态创建 TX 任务并返回状态。 */
    [[nodiscard]] fw::Status start_tx() noexcept;

private:
    friend struct ModuleTestAccess;
    using SessionDemux = demux::StaticDemux<proto::demux_frame::kDecoder, demux::rtcm3::kDecoder>;
    static constexpr uint8_t kProductIndex = SessionDemux::index_of<proto::demux_frame::kDecoder>();
    static constexpr uint8_t kRtcmIndex = SessionDemux::index_of<demux::rtcm3::kDecoder>();
    static constexpr size_t kRxChunkBytes = 64;
    class SessionSink final : public demux::Sink {
    public:
        constexpr SessionSink() noexcept = default;
        // 仅 kLink，非 ISR；OPEN/CLOSE 时复位元数据，owner 覆盖本会话生命周期。
        void bind(BtLinkModule &owner, core::SessionHandle handle) noexcept;
        void on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> wire) noexcept override;
        [[nodiscard]] bool dispatch_one() noexcept;
        [[nodiscard]] bool pending() const noexcept
        {
            return cursor_ < count_;
        }
        [[nodiscard]] bool healthy() const noexcept
        {
            return healthy_;
        }

    private:
        struct PendingFrame {
            uint16_t size = 0;
            uint8_t index = 0;
        };
        static constexpr size_t kMinWireBytes = 6;
        std::array<uint8_t, SessionDemux::kBufSize> bytes_{};
        std::array<PendingFrame, SessionDemux::kBufSize / kMinWireBytes> frames_{};
        BtLinkModule *owner_ = nullptr;
        core::SessionHandle handle_ = 0;
        size_t count_ = 0;
        size_t cursor_ = 0;
        size_t size_ = 0;
        size_t offset_ = 0;
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
    void drain_rx(uint32_t round_start_ms) noexcept;
    [[nodiscard]] bool drain_session(SessionEntry &entry, size_t index, size_t &budget,
                                     uint32_t round_start_ms) noexcept;
    [[nodiscard]] bool feed_one(SessionEntry &entry, size_t &budget, uint32_t round_start_ms) noexcept;
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
    AsyncTxTransport async_tx_{transport_};
    // optional 在 init 按依赖次序原地构造，生命周期覆盖整个运行期，不分配堆。
    std::optional<core::services::DeviceService> device_service_{};
    std::optional<core::services::ConfigService> config_service_{};
    std::optional<core::services::AlarmService> alarm_service_{};
    std::optional<core::services::PairService> pair_service_{};
    core::CommandRouter router_{};
    std::optional<core::LinkService> service_{};
    std::array<SessionEntry, kMaxSessions> sessions_{};
    size_t rx_cursor_ = 0;
    std::array<uint8_t, kRtcmBatchBytes> rtcm_batch_{};
    size_t rtcm_size_ = 0;
    uint32_t rtcm_rejected_ = 0;
#if RTK_GNSS_ENABLED
    uint32_t gnss_fix_version_ = 0;
    uint32_t gnss_status_version_ = 0;
    ::gnss::Fix cached_gnss_fix_{};
#endif
};
/** 链路模块的只读控制台命令表（bt/alarm）；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> link_cmds() noexcept;
} // namespace app
