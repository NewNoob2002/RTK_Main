// 系统模块控制台命令表：status（只读 DataCenter 各主题 Slot，不进入链路执行器）。

#include "support/console_support.hpp"
#include "modules/system/system_module.hpp"
#include "app_context.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <span>

#include "esp_err.h"

namespace app {

namespace {

void print_sessions(const Sessions &sessions) noexcept
{
    // 会话数由发布者限制为 kMaxSessions，转换到 unsigned 无损。
    printf("sessions: %u\n", static_cast<unsigned>(sessions.count));
    for (size_t idx = 0U; idx < sessions.count; idx++) {
        const SessionSnapshot &session = sessions.entries[idx];
        printf("  handle=0x%08" PRIx32 " role=%s mac=", session.handle, role_name(session.role));
        print_mac(session.addr);
        printf("\n");
    }
}

void print_whitelist(const LinkStatus &link, const StoreStatus &store) noexcept
{
    // 白名单数量不超过 kMaxAlarms，转换到 unsigned 无损。
    printf("whitelist: %u pending=%s store=%s\n", static_cast<unsigned>(link.whitelist_count),
           store.whitelist_pending ? "yes" : "no", esp_err_to_name(store.whitelist_error));
    for (size_t idx = 0U; idx < link.whitelist_count; idx++) {
        printf("  ");
        print_mac(link.whitelist[idx]);
        printf("\n");
    }
}

int cmd_status(void * /*ctx*/, int /*argc*/, char ** /*argv*/) noexcept
{
    /* 保持各主题的读取次序，再按原顺序输出。 */
    const auto &data = app_context().data;
    const auto system = data.system_status.read();
    const auto link = data.link_status.read();
    const auto sessions = data.sessions.read();
    const auto store = data.store_status.read();
    const auto alarm = data.alarm_state.read();
    const auto stats = data.link_stats.read();
    const auto rtcm = data.rtcm_stats.read();
    const auto demux = data.demux_stats.read();
    const auto heartbeat = data.heartbeat.read();
    const auto exec = data.exec_stats.read();

    printf("link: %s\n", system.link_running ? "running" : "not running");
    printf("bt: %s\n", system.bt_ready ? "ready" : "init failed");
    printf("policy: %s\n", policy_name(link.policy));

    print_sessions(sessions);
    print_whitelist(link, store);

    printf("pairing: %s\n", link.pairing ? "open" : "closed");
    // 报警字段均为 uint8_t，扩展为 unsigned 无损。
    printf("alarm: state=%u level=%u override=%s\n", static_cast<unsigned>(alarm.state),
           static_cast<unsigned>(alarm.level), alarm.override ? "yes" : "no");
    printf("stats: frames_rx=%" PRIu32 " replies_tx=%" PRIu32 " write_errors=%" PRIu32 " pushes_tx=%" PRIu32
           " disconnects=%" PRIu32 " rx_dropped=%" PRIu32 "\n",
           stats.frames_rx, stats.replies_tx, stats.write_errors, stats.pushes_tx, stats.disconnects, stats.rx_dropped);
    printf("parser: crc_errors=%" PRIu32 " length_errors=%" PRIu32 "\n", stats.parser_crc_errors,
           stats.parser_length_errors);
    printf("tx: sent=%" PRIu32 " failed=%" PRIu32 " queue_full=%" PRIu32 " max_depth=%" PRIu32 " stack_free=%" PRIu32
           "\n",
           stats.tx_sent, stats.tx_failed, stats.tx_queue_full, stats.tx_max_depth, stats.tx_stack_free);
    printf("rtcm: frames=%" PRIu32 " bytes=%" PRIu32 " crc32=0x%08" PRIX32 " last_msg=%" PRIu16
           " rejected_role=%" PRIu32 " unknown_bytes=%" PRIu32 " product_rejects=%" PRIu32 " rtcm_rejects=%" PRIu32
           "\n",
           rtcm.frames, rtcm.bytes, rtcm.crc32, rtcm.last_msg, rtcm.rejected_role, demux.unknown_bytes,
           demux.product_rejects, demux.rtcm_rejects);
    if (heartbeat.present) {
        // 心跳字段均为无符号 8/16 bit，扩展为 unsigned 无损。
        printf("heartbeat: seq=%u battery=%u state=%u\n", static_cast<unsigned>(heartbeat.value.seq),
               static_cast<unsigned>(heartbeat.value.battery_pct), static_cast<unsigned>(heartbeat.value.local_state));
    } else {
        printf("heartbeat: none\n");
    }
    printf("heap: free=%" PRIu32 " min=%" PRIu32 " link_stack_hwm=%" PRIu32 " snapshot_ms=%" PRIu32 "\n",
           system.free_heap, system.min_free_heap, exec.stack_free, link.snapshot_ms);
    printf("exec: rounds=%" PRIu32 " overruns=%" PRIu32 " max_round_ms=%" PRIu32 " commands=%" PRIu32
           " result_drops=%" PRIu32 " stack_free=%" PRIu32 "\n",
           exec.rounds, exec.overruns, exec.max_round_ms, exec.commands, exec.result_drops, exec.stack_free);
    return 0;
}

constexpr std::array<cli::Cmd, 1> kSystemCmds{{
    {"status", "print sessions, whitelist, alarm, stats, heap and stack", nullptr, cmd_status, nullptr},
}};

} // namespace

std::span<const cli::Cmd> system_cmds() noexcept
{
    return kSystemCmds;
}

} // namespace app
