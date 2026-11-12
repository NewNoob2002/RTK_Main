// 原有场景的接线适配：调用真实模块，旧测试的断言字段只在此处由主题装配。
// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
// 保留基线白盒场景对会话/预读状态的检查；不额外编译此 cpp，避免重复定义。
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../main/bt_link_module.cpp"
#include "../../main/rtcm_module.hpp"
#include "../../main/storage_module.hpp"
#include "rtk/os/static_queue.hpp"

namespace rtk::app {
namespace {
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
bool &s_whitelist_pending = s_storage_module.pending;
uint32_t &s_whitelist_attempt_ms = s_storage_module.attempt_ms;
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
    data::kRtcmBatch.write({std::span<const uint8_t>{s_rtcm_batch}.first(s_rtcm_size), s_rtcm_rejected});
    static_cast<void>(rtcm_poll(&s_rtcm_module, os::now_ms()));
    s_rtcm_size = 0;
    s_rtcm_rejected = 0;
}
struct RtcmProbe {
    const RtcmStats &stats() const
    {
        flush_rtcm();
        return s_rtcm_module.forwarder.stats();
    }
} s_rtcm;
void publish_snapshot(uint32_t now_ms)
{
    apply_rtcm_reset(s_bt_link_module);
    publish_link(now_ms);
    flush_rtcm();
    const auto rtcm = data::kRtcmStats.read();
    const auto demux = data::kDemuxStats.read();
    const auto store = data::kStoreStatus.read();
    const auto sessions = data::kSessions.read();
    const auto stats = data::kLinkStats.read();
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
        static_cast<void>(flush_whitelist(now_ms));
    } else {
        static_cast<void>(storage_poll(&s_storage_module, now_ms));
    }
}
void process_commands(uint32_t /*now_ms*/)
{
    Command queued{};
    while (s_command_queue.receive(queued, 0)) {
        ::app::Command command{};
        command.seq = queued.seq;
        ::app::CommandResult result{};
        if (queued.id == CommandId::kRtcmReset) {
            command.verb = verb(RtcmVerb::kReset);
            rtcm_command(&s_rtcm_module, command, result);
            apply_rtcm_reset(s_bt_link_module);
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
            bt_link_command(&s_bt_link_module, command, result);
        }
        const CommandResult response{queued.seq, result.status == ::app::Result::kOk ? CommandStatus::kOk
                                                 : result.status == ::app::Result::kIoError
                                                     ? CommandStatus::kStoreError
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
    bt_link_wait(nullptr, pending ? 0U : 20U);
    const uint32_t begin = os::now_ms();
    process_commands(begin);
    s_bt_link_module.last_tick_ms = last_tick_ms;
    static_cast<void>(bt_link_poll(&s_bt_link_module, begin));
    last_tick_ms = s_bt_link_module.last_tick_ms;
    flush_rtcm();
    static_cast<void>(storage_poll(&s_storage_module, os::now_ms()));
    publish_snapshot(os::now_ms());
}
} // namespace
} // namespace rtk::app
