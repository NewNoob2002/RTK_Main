// 系统状态打印组：link/rtcm/gnss/sys 各段与聚合打印。
// 仅 console REPL 任务；逐行写控制台，读 DataCenter 快照，不分配堆、非 ISR。

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_system.h"
#include "esp_timer.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "modules/rtcm/RtcmModule.hpp"
#include "support/ConsoleSupport.hpp"
#include "SystemCmdsPrivate.hpp"

namespace app {

namespace detail {

uint64_t kibTenths(uint64_t bytes) noexcept
{
    return (bytes * kTenthsPerUnit) / kBytesPerKib;
}

// NOLINTBEGIN(readability-function-size): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
/**
 * @brief 打印链路、会话、白名单与告警快照
 * @note 仅 console REPL 任务；输出可阻塞，非 ISR。不拆（ADR-0013 §2.4）：同段报告共享快照，拆分需重复采样或传递六个以上参数。
 * @note 不拆（ADR-0013 §2.4）：读取一份快照后顺序打印；拆开要么重复读快照（同一段报告内数值可能不一致）， 要么传 6 个以上参数。
 */
void printLinkSection() noexcept
{
    const auto &data = appContext().data;
    const auto system = data.systemStatus.read();
    const auto link = data.linkStatus.read();
    const auto sessions = data.sessions.read();
    const auto store = data.storeStatus.read();
    const auto alarm = data.alarmState.read();
    const auto stats = data.linkStats.read();
    const auto heartbeat = data.heartbeat.read();

    cli::printSection("link");
    cli::printKv("state", "%s", system.linkRunning ? "running" : "not running");
    cli::printKv("bt", "%s", system.btReady ? "ready" : "init failed");
    cli::printKv("policy", "%s", policyName(link.policy));
    cli::printKv("pairing", "%s", link.pairing ? "open" : "closed");
    // 会话数与白名单数上限均不超过 kMaxAlarms/kMaxSessions，转换到 unsigned 无损。
    cli::printKv("sessions", "%u / %u", static_cast<unsigned>(sessions.count),
                 static_cast<unsigned>(kMaxSessions));
    std::array<char, kMacStrBuf> macBuf{};
    for (size_t idx = 0U; idx < sessions.count; ++idx) {
        const SessionSnapshot &session = sessions.entries[idx];
        formatMac(session.addr, macBuf);
        cli::printKv("session", "handle=0x%08" PRIx32 " role=%s mac=%s", session.handle,
                     roleName(session.role), macBuf.data());
    }
    cli::printKv("whitelist", "%u (pending=%s, store=%s)",
                 static_cast<unsigned>(link.whitelistCount), store.whitelistPending ? "yes" : "no",
                 esp_err_to_name(store.whitelistError));
    for (size_t idx = 0U; idx < link.whitelistCount; ++idx) {
        formatMac(link.whitelist[idx], macBuf);
        cli::printKv("wl_addr", "%s", macBuf.data());
    }
    // state/level 均为 uint8_t，转换到 unsigned 无损。
    cli::printKv("alarm", "state=%u level=%u override=%s", static_cast<unsigned>(alarm.state),
                 static_cast<unsigned>(alarm.level), alarm.override ? "yes" : "no");
    if (heartbeat.present) {
        cli::printKv("heartbeat", "seq=%u battery=%u%% state=%u",
                     static_cast<unsigned>(heartbeat.value.seq),
                     static_cast<unsigned>(heartbeat.value.batteryPct),
                     static_cast<unsigned>(heartbeat.value.localState));
    } else {
        cli::printKv("heartbeat", "none");
    }
    cli::printKv("traffic",
                 "rx=%" PRIu32 " tx=%" PRIu32 " push=%" PRIu32 " werr=%" PRIu32 " disc=%" PRIu32
                 " drop=%" PRIu32,
                 stats.framesRx, stats.repliesTx, stats.pushesTx, stats.writeErrors,
                 stats.disconnects, stats.rxDropped);
    cli::printKv("parser", "crc_err=%" PRIu32 " len_err=%" PRIu32, stats.parserCrcErrors,
                 stats.parserLengthErrors);
    cli::printKv("spp_tx",
                 "accepted=%" PRIu32 " completed=%" PRIu32 " cancelled=%" PRIu32
                 " B errors=%" PRIu32,
                 stats.txAccepted, stats.txCompleted, stats.txCancelled, stats.txErrors);
    cli::printKv("spp_flow",
                 "cong=%" PRIu32 " high_water=%" PRIu32 " B paused=%" PRIu32 " inflight=%" PRIu32,
                 stats.txCong, stats.txHighWater, stats.txPaused, stats.txInflight);
    cli::printKv("spp_events",
                 "anon_write_fail=%" PRIu32 " stray=%" PRIu32 " dispatch_pauses=%" PRIu32
                 " cancelled=%" PRIu32,
                 stats.txAnonWriteFail, stats.txStrayWrite, stats.dispatchPauses,
                 stats.dispatchCancelled);
    cli::printKv(
        "spp_rx",
        "evt_drop=%" PRIu32 " wakes=%" PRIu32 " s0=%" PRIu32 "/%" PRIu32 "(drop=%" PRIu32 ") "
        "s1=%" PRIu32 "/%" PRIu32 "(drop=%" PRIu32 ")",
        stats.evtDropped, stats.rxWakeCalls, stats.rxUsed[0], stats.rxHighWater[0],
        stats.rxDroppedBytes[0], stats.rxUsed[1], stats.rxHighWater[1], stats.rxDroppedBytes[1]);
}
// NOLINTEND(readability-function-size)

/**
 * @brief 打印系统段（堆、运行时长、执行器）
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
void printSysSection() noexcept
{
    const auto &data = appContext().data;
    const auto system = data.systemStatus.read();
    const auto link = data.linkStatus.read();
    const auto exec = data.execStats.read();

    const uint64_t freeKib = kibTenths(system.freeHeap);
    const uint64_t minKib = kibTenths(system.minFreeHeap);
    const int64_t uptimeUs = esp_timer_get_time();
    const uint64_t uptimeMs = (uptimeUs >= 0) ? (static_cast<uint64_t>(uptimeUs) / kUsPerMs) : 0ULL;

    cli::printSection("sys");
    cli::printKv("uptime", "%" PRIu64 " ms", uptimeMs);
    cli::printKv("heap_free", "%" PRIu32 " B (%llu.%llu KB)", system.freeHeap,
                 static_cast<unsigned long long>(freeKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(freeKib % kTenthsPerUnit));
    cli::printKv("heap_min", "%" PRIu32 " B (%llu.%llu KB)", system.minFreeHeap,
                 static_cast<unsigned long long>(minKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(minKib % kTenthsPerUnit));
    cli::printKv("snapshot_ms", "%" PRIu32 " ms", link.snapshotMs);
    cli::printKv("exec_rounds", "%" PRIu32, exec.rounds);
    cli::printKv("exec_overruns", "%" PRIu32, exec.overruns);
    cli::printKv("exec_max_rnd", "%" PRIu32 " ms", exec.maxRoundMs);
    cli::printKv("exec_cmds", "%" PRIu32, exec.commands);
    cli::printKv("exec_drops", "%" PRIu32, exec.resultDrops);
    cli::printKv("link_stk_hwm", "%" PRIu32 " B", exec.stackFree);
#if RTK_GNSS_ENABLED
    const auto ioExec = data.ioExecStats.read();
    cli::printKv("io_rounds", "%" PRIu32, ioExec.rounds);
    cli::printKv("io_overruns", "%" PRIu32, ioExec.overruns);
    cli::printKv("io_max_rnd", "%" PRIu32 " ms", ioExec.maxRoundMs);
    cli::printKv("io_cmds", "%" PRIu32, ioExec.commands);
    cli::printKv("io_drops", "%" PRIu32, ioExec.resultDrops);
    cli::printKv("io_stk_hwm", "%" PRIu32 " B", ioExec.stackFree);
#endif
}

/**
 * @brief  打印 link 概览段（状态、会话、报警）
 * @note 控制台命令上下文；读一次快照后顺序打印，不阻塞。
 */
void printStatusLink() noexcept
{
    const auto &data = appContext().data;
    const auto system = data.systemStatus.read();
    const auto link = data.linkStatus.read();
    const auto sessions = data.sessions.read();
    const auto alarm = data.alarmState.read();
    const auto stats = data.linkStats.read();
    cli::printSection("link");
    cli::printKv("state", "%s (bt=%s, policy=%s, pair=%s)", system.linkRunning ? "running" : "down",
                 system.btReady ? "ready" : "err", policyName(link.policy),
                 link.pairing ? "open" : "closed");
    // 会话数/白名单/报警字段均为小无符号整数，转换到 unsigned 无损。
    cli::printKv("sessions", "%u/%u  wl=%u  rx=%" PRIu32 "  tx=%" PRIu32 "  drop=%" PRIu32,
                 static_cast<unsigned>(sessions.count), static_cast<unsigned>(kMaxSessions),
                 static_cast<unsigned>(link.whitelistCount), stats.framesRx, stats.repliesTx,
                 stats.rxDropped);
    cli::printKv("alarm", "state=%u  level=%u  override=%s", static_cast<unsigned>(alarm.state),
                 static_cast<unsigned>(alarm.level), alarm.override ? "yes" : "no");
}

/**
 * @brief  打印 rtcm 概览段（转发量、速率与错误计数）
 * @note 控制台命令上下文；读一次快照后顺序打印，不阻塞。
 */
void printStatusRtcm() noexcept
{
    const auto &data = appContext().data;
    const auto rtcm = data.rtcmStats.read();
    const auto demux = data.demuxStats.read();
    const uint64_t rtcmKib = kibTenths(rtcm.bytes);
    cli::printSection("rtcm");
    cli::printKv("forward", "%" PRIu32 " frames, %llu.%llu KB, %" PRIu32 " B/s", rtcm.frames,
                 static_cast<unsigned long long>(rtcmKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(rtcmKib % kTenthsPerUnit), rtcm.bytesPerSec);
    cli::printKv("errors",
                 "role=%" PRIu32 "  unknown=%" PRIu32 " B  prod_rej=%" PRIu32 "  rtcm_rej=%" PRIu32,
                 rtcm.rejectedRole, demux.unknownBytes, demux.productRejects, demux.rtcmRejects);
}

/**
 * @brief  打印 sys 概览段（堆、执行器，GNSS 启用时附 io 执行器与 GNSS 环形）
 * @note 控制台命令上下文；读一次快照后顺序打印，不阻塞。
 */
void printStatusSys() noexcept
{
    const auto &data = appContext().data;
    const auto system = data.systemStatus.read();
    const auto exec = data.execStats.read();
    const uint64_t freeKib = kibTenths(system.freeHeap);
    const uint64_t minKib = kibTenths(system.minFreeHeap);
    cli::printSection("sys");
    cli::printKv("heap", "free=%llu.%llu KB  min=%llu.%llu KB",
                 static_cast<unsigned long long>(freeKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(freeKib % kTenthsPerUnit),
                 static_cast<unsigned long long>(minKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(minKib % kTenthsPerUnit));
    cli::printKv("exec",
                 "rounds=%" PRIu32 "  overruns=%" PRIu32 "  max_rnd=%" PRIu32
                 " ms  link_stk=%" PRIu32 " B",
                 exec.rounds, exec.overruns, exec.maxRoundMs, exec.stackFree);
#if RTK_GNSS_ENABLED
    const auto ioExec = data.ioExecStats.read();
    const auto gnss = data.gnssStatus.read();
    const auto rWr = data.gnssAppRing.typedWriterStats();
    const auto rRd = data.gnssAppRing.typedReaderStats(0U);
    const auto sub = data.gnssAppSub.snapshot();
    cli::printKv("io_exec",
                 "rounds=%" PRIu32 "  overruns=%" PRIu32 "  max_rnd=%" PRIu32 " ms  io_stk=%" PRIu32
                 " B",
                 ioExec.rounds, ioExec.overruns, ioExec.maxRoundMs, ioExec.stackFree);
    cli::printSection("gnss");
    cli::printKv("state", "%s  model=%s  baud=%" PRIu32, ::gnss::stateName(gnss.state),
                 ::gnss::modelName(gnss.model), gnss.hostBaud);
    cli::printKv("pipe", "push=%" PRIu32 "  drop=%" PRIu32 "  written=%" PRIu32 " B",
                 gnss.pipe.pushedFrames, gnss.pipe.droppedFrames, gnss.pipe.writtenBytes);
    cli::printKv("app_ring",
                 "cap=%uB hwm=%zuB mask=0x%02" PRIX32 " wr=%" PRIu32 " rd=%" PRIu32 " drop=%" PRIu32
                 " qovsz=%" PRIu32,
                 static_cast<unsigned>(kGnssAppRingBytes), rWr.highWaterBytes, sub.activeMask,
                 rWr.base.records, rRd.base.records,
                 rRd.base.dropped + rWr.oversizeDrop + rWr.pinnedDrop, sub.queryOversizeTotal);
    cli::printKv("obs_ring", "reserved=%" PRIu32 "B not allocated (K6)", kStaticObsReservedBytes);
#endif
}

int cmdStatusOverview(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status [link|rtcm|sys|all]");
    }
    printStatusLink();
    printStatusRtcm();
    printStatusSys();
    return 0;
}

/**
 * @brief 打印 GNSS 段（状态、管道、环形缓冲、观测）
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
void printGnssSection() noexcept
{
#if RTK_GNSS_ENABLED
    const auto &data = appContext().data;
    const auto gnss = data.gnssStatus.read();
    const auto rWr = data.gnssAppRing.typedWriterStats();
    const auto rRd = data.gnssAppRing.typedReaderStats(0U);
    const auto sub = data.gnssAppSub.snapshot();
    cli::printSection("gnss");
    cli::printKv("state", "%s  model=%s  baud=%" PRIu32, ::gnss::stateName(gnss.state),
                 ::gnss::modelName(gnss.model), gnss.hostBaud);
    cli::printKv("pipe", "push=%" PRIu32 "  drop=%" PRIu32 "  written=%" PRIu32 " B",
                 gnss.pipe.pushedFrames, gnss.pipe.droppedFrames, gnss.pipe.writtenBytes);
    cli::printKv("app_ring",
                 "cap=%uB hwm=%zuB mask=0x%02" PRIX32 " wr=%" PRIu32 "/%" PRIu32 "B overwr=%" PRIu32
                 " nosub=%" PRIu32 " ovsz=%" PRIu32 " pin=%" PRIu32,
                 static_cast<unsigned>(kGnssAppRingBytes), rWr.highWaterBytes, sub.activeMask,
                 rWr.base.records, rWr.base.bytes, rWr.base.overwritten, rWr.noSubscriber,
                 rWr.oversizeDrop, rWr.pinnedDrop);
    cli::printKv("app_reader",
                 "rec=%" PRIu32 "/%" PRIu32 "B drop=%" PRIu32 " filt=%" PRIu32 " pin=%" PRIu32
                 " qovsz=%" PRIu32,
                 rRd.base.records, rRd.base.bytes, rRd.base.dropped, rRd.filtered, rRd.pinnedDrop,
                 sub.queryOversizeTotal);
    cli::printKv("obs_ring", "reserved=%" PRIu32 "B not allocated (K6)", kStaticObsReservedBytes);
#else
    cli::printSection("gnss");
    cli::printKv("state", "disabled");
#endif
}

int cmdStatusLink(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status link");
    }
    printLinkSection();
    return 0;
}

int cmdStatusRtcm(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status rtcm");
    }
    printRtcmStatus();
    return 0;
}

int cmdStatusGnss(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status gnss");
    }
    printGnssSection();
    return 0;
}

int cmdStatusSys(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status sys");
    }
    printSysSection();
    return 0;
}

int cmdStatusAll(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status all");
    }
    printLinkSection();
    printRtcmStatus();
    printGnssSection();
    printSysSection();
    return 0;
}

} // namespace detail

} // namespace app
