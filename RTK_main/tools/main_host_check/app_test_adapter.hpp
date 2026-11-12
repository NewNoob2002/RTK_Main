// 原有场景的接线适配：调用真实模块，旧测试的断言字段只在此处由主题装配。
#pragma once
// 保留基线白盒场景对会话/预读状态的检查；不额外编译此 cpp，避免重复定义。
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../main/modules/bt_link/bt_link_module.cpp"
#include "../../main/modules/rtcm/rtcm_module.hpp"
#include "../../main/modules/storage/storage_module.hpp"
#include "rtk/os/static_queue.hpp"

namespace rtk::app {
namespace {
constinit AppContext s_test_app{};
}
AppContext &app_context() noexcept
{
    return s_test_app;
}

// 仅测试接线可访问模块私有状态，保留原场景的逐字段断言。
struct ModuleTestAccess {
    static auto &manager() noexcept
    {
        return app_context().bt_link.link_manager_;
    }
    static auto &alarm() noexcept
    {
        return app_context().bt_link.alarm_;
    }
    static auto &service() noexcept
    {
        return app_context().bt_link.link_service();
    }
    static auto &sessions() noexcept
    {
        return app_context().bt_link.sessions_;
    }
    static auto &last_tick_ms() noexcept
    {
        return app_context().bt_link.last_tick_ms_;
    }
    static auto &whitelist_pending() noexcept
    {
        return app_context().storage.pending_;
    }
    static auto &whitelist_attempt_ms() noexcept
    {
        return app_context().storage.attempt_ms_;
    }
    static void handle_event(const rtk_bt_spp_evt_t &evt, uint32_t now_ms) noexcept
    {
        app_context().bt_link.handle_event(evt, now_ms);
    }
    static void drain_rx(uint32_t now_ms) noexcept
    {
        app_context().bt_link.drain_rx(now_ms);
    }
    static void collect_whitelist_dirty() noexcept
    {
        app_context().bt_link.collect_whitelist_dirty();
    }
    static void apply_rtcm_reset() noexcept
    {
        app_context().bt_link.apply_rtcm_reset();
    }
    static void publish_link(uint32_t now_ms) noexcept
    {
        app_context().bt_link.publish_link(now_ms);
    }
    static void flush_rtcm() noexcept
    {
        auto &bt = app_context().bt_link;
        app_context().data.rtcm_batch.write(
            {std::span<const uint8_t>{bt.rtcm_batch_}.first(bt.rtcm_size_), bt.rtcm_rejected_});
        static_cast<void>(app_context().rtcm.poll(os::now_ms()));
        bt.rtcm_size_ = 0;
        bt.rtcm_rejected_ = 0;
    }
};
namespace {
// 旧白盒断言的名字仅限本测试 TU，产品没有这些二次暴露。
auto &s_link_manager = ModuleTestAccess::manager();
auto &s_alarm = ModuleTestAccess::alarm();
struct LinkServiceProbe {
    core::LinkService *operator->() const noexcept
    {
        return &ModuleTestAccess::service();
    }
};
constinit LinkServiceProbe s_service{};
auto &s_sessions = ModuleTestAccess::sessions();
void handle_event(const rtk_bt_spp_evt_t &evt, uint32_t now_ms) noexcept
{
    ModuleTestAccess::handle_event(evt, now_ms);
}
void drain_rx(uint32_t now_ms) noexcept
{
    ModuleTestAccess::drain_rx(now_ms);
}
void collect_whitelist_dirty() noexcept
{
    ModuleTestAccess::collect_whitelist_dirty();
}
void mark_whitelist_dirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept
{
    app_context().storage.mark_whitelist_dirty(list, now_ms);
}
using WhitelistRaw = std::array<std::array<uint8_t, core::kAddrLen>, kMaxAlarms>;
constexpr uint32_t kWhitelistRetryMs = 1000;
enum class CommandId { kAlarmSet, kAlarmClear, kPair, kWhitelistClear, kSaveSn, kRtcmReset };
enum class CommandStatus { kOk, kInvalidArg, kStoreError };
struct Command {
    CommandId id{};
    uint8_t state = 0;
    uint8_t level = 0;
    uint32_t seq = 0;
};
struct CommandResult {
    uint32_t seq = 0;
    CommandStatus status{};
};
os::StaticQueue<Command, 4> s_command_queue{};
os::StaticQueue<CommandResult, 1> s_result_queue{};
bool &s_whitelist_pending = ModuleTestAccess::whitelist_pending();
uint32_t &s_whitelist_attempt_ms = ModuleTestAccess::whitelist_attempt_ms();
struct StatusSnapshot {
    bool whitelist_pending = false;
    esp_err_t whitelist_error = ESP_OK;
    bool bt_ready = false;
    bool link_running = false;
    core::Policy policy = policy_from_config();
    size_t session_count = 0;
    std::array<SessionSnapshot, kMaxSessions> sessions{};
    uint32_t rtcm_frames = 0;
    uint32_t rtcm_bytes = 0;
    uint32_t rtcm_crc32 = 0;
    uint16_t rtcm_last_msg = 0;
    uint32_t rtcm_rejected_role = 0;
    uint32_t demux_unknown_bytes = 0;
    uint32_t demux_product_rejects = 0;
    uint32_t demux_rtcm_rejects = 0;
    uint32_t parser_crc_errors = 0;
    uint32_t parser_length_errors = 0;
    uint32_t rx_dropped = 0;
};
StatusSnapshot s_snapshot{};
void flush_rtcm()
{
    ModuleTestAccess::flush_rtcm();
}
struct RtcmProbe {
    const RtcmStats &stats() const
    {
        flush_rtcm();
        return app_context().rtcm.stats();
    }
} s_rtcm;
void publish_snapshot(uint32_t now_ms)
{
    ModuleTestAccess::apply_rtcm_reset();
    ModuleTestAccess::publish_link(now_ms);
    flush_rtcm();
    const auto rtcm = app_context().data.rtcm_stats.read();
    const auto demux = app_context().data.demux_stats.read();
    const auto store = app_context().data.store_status.read();
    const auto sessions = app_context().data.sessions.read();
    const auto stats = app_context().data.link_stats.read();
    s_snapshot.whitelist_pending = store.whitelist_pending;
    s_snapshot.whitelist_error = store.whitelist_error;
    s_snapshot.session_count = sessions.count;
    s_snapshot.sessions = sessions.entries;
    s_snapshot.rtcm_frames = rtcm.frames;
    s_snapshot.rtcm_bytes = rtcm.bytes;
    s_snapshot.rtcm_crc32 = rtcm.crc32;
    s_snapshot.rtcm_last_msg = rtcm.last_msg;
    s_snapshot.rtcm_rejected_role = rtcm.rejected_role;
    s_snapshot.demux_unknown_bytes = demux.unknown_bytes;
    s_snapshot.demux_product_rejects = demux.product_rejects;
    s_snapshot.demux_rtcm_rejects = demux.rtcm_rejects;
    s_snapshot.parser_crc_errors = stats.parser_crc_errors;
    s_snapshot.parser_length_errors = stats.parser_length_errors;
    s_snapshot.rx_dropped = stats.rx_dropped;
}
void note_bt_failure()
{
    s_snapshot.bt_ready = false;
}
void read_snapshot(StatusSnapshot &out)
{
    out = s_snapshot;
}
void persist_whitelist(uint32_t now_ms, bool force = false)
{
    if (force) {
        static_cast<void>(app_context().storage.flush_whitelist(now_ms));
    } else {
        static_cast<void>(app_context().storage.poll(now_ms));
    }
}
void process_commands(uint32_t /*now_ms*/)
{
    Command queued{};
    while (s_command_queue.receive(queued, 0)) {
        fw::Command command{};
        command.seq = queued.seq;
        fw::CommandResult result{};
        if (queued.id == CommandId::kRtcmReset) {
            command.verb = verb(RtcmVerb::kReset);
            app_context().rtcm.on_command(command, result);
            ModuleTestAccess::apply_rtcm_reset();
        } else {
            switch (queued.id) {
            case CommandId::kAlarmSet:
                command.verb = verb(LinkVerb::kAlarmSet);
                command.arg_len = 2;
                command.args[0] = queued.state;
                command.args[1] = queued.level;
                break;
            case CommandId::kAlarmClear:
                command.verb = verb(LinkVerb::kAlarmClear);
                break;
            case CommandId::kPair:
                command.verb = verb(LinkVerb::kPair);
                break;
            case CommandId::kWhitelistClear:
                command.verb = verb(LinkVerb::kWhitelistClear);
                break;
            default:
                command.verb = 0xFF;
                break;
            }
            app_context().bt_link.on_command(command, result);
        }
        const CommandResult response{queued.seq, result.status == fw::Result::kOk        ? CommandStatus::kOk
                                                 : result.status == fw::Result::kIoError ? CommandStatus::kStoreError
                                                                                         : CommandStatus::kInvalidArg};
        if (!s_result_queue.send(response, 0)) {
            CommandResult stale{};
            static_cast<void>(s_result_queue.receive(stale, 0));
            static_cast<void>(s_result_queue.send(response, 0));
        }
    }
}
void link_round(uint32_t &last_tick_ms)
{
    bool pending = false;
    for (const auto &entry : s_sessions) {
        pending = pending || (entry.used && entry.demux_ready &&
                              (entry.drain_pending || entry.sink.pending() || entry.rx_offset < entry.rx_size));
    }
    app_context().bt_link.wait(pending ? 0U : 20U);
    const uint32_t begin = os::now_ms();
    process_commands(begin);
    ModuleTestAccess::last_tick_ms() = last_tick_ms;
    static_cast<void>(app_context().bt_link.poll(begin));
    last_tick_ms = ModuleTestAccess::last_tick_ms();
    flush_rtcm();
    static_cast<void>(app_context().storage.poll(os::now_ms()));
    publish_snapshot(os::now_ms());
}
} // namespace
} // namespace rtk::app
