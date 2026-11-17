// 系统模块控制台命令表（ADR-0009 §2.5）：status / log / sys。
// 线程：仅在 console REPL 任务调用，非 ISR；只读 DataCenter 或调用 IDF 系统/任务/日志查询接口。

#include "modules/system/SystemModule.hpp"
#include "modules/system/SystemMemory.hpp"

#include "app/App.hpp"
#include "app/BootReport.hpp"
#include "support/ResetReason.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log_level.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "fw/Trace.hpp"
#include "modules/rtcm/RtcmModule.hpp"
#include "support/ConsoleSupport.hpp"

extern "C" {
// 链接脚本提供的静态段起止符号，命名由 ESP-IDF 链接脚本固定，须在全局作用域声明 C 链接。
// NOLINTBEGIN(readability-identifier-naming)
extern const char _data_start;
extern const char _data_end;
extern const char _bss_start;
extern const char _bss_end;
// NOLINTEND(readability-identifier-naming)
}

namespace app {

namespace {

constexpr uint64_t kBytesPerKib = 1024ULL;
constexpr uint64_t kTenthsPerUnit = 10ULL;
constexpr uint64_t kUsPerMs = 1000ULL;
constexpr uint64_t kMsPerSec = 1000ULL;
constexpr uint16_t kChipRevDivisor = 100U;
constexpr uint32_t kRebootDelayMs = 100U;
constexpr size_t kLogLevelArgs = 3U;
constexpr size_t kCoreTextLen = 16U;
#if RTK_GNSS_ENABLED
constexpr uint32_t kStaticObsReservedBytes = 16384U;
#endif

uint64_t kibTenths(uint64_t bytes) noexcept
{
    return (bytes * kTenthsPerUnit) / kBytesPerKib;
}

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

int cmdStatusOverview(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status [link|rtcm|sys|all]");
    }
    const auto &data = appContext().data;
    const auto system = data.systemStatus.read();
    const auto link = data.linkStatus.read();
    const auto sessions = data.sessions.read();
    const auto alarm = data.alarmState.read();
    const auto stats = data.linkStats.read();
    const auto rtcm = data.rtcmStats.read();
    const auto demux = data.demuxStats.read();
    const auto exec = data.execStats.read();

    const uint64_t rtcmKib = kibTenths(rtcm.bytes);
    const uint64_t freeKib = kibTenths(system.freeHeap);
    const uint64_t minKib = kibTenths(system.minFreeHeap);

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

    cli::printSection("rtcm");
    cli::printKv("forward", "%" PRIu32 " frames, %llu.%llu KB, %" PRIu32 " B/s", rtcm.frames,
                 static_cast<unsigned long long>(rtcmKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(rtcmKib % kTenthsPerUnit), rtcm.bytesPerSec);
    cli::printKv("errors",
                 "role=%" PRIu32 "  unknown=%" PRIu32 " B  prod_rej=%" PRIu32 "  rtcm_rej=%" PRIu32,
                 rtcm.rejectedRole, demux.unknownBytes, demux.productRejects, demux.rtcmRejects);

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
    return 0;
}

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

int cmdLogLevel(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kLogLevelArgs)) || (argv == nullptr)) {
        return cli::fail("usage: log level <tag|*> <E|W|I|N>");
    }
    const std::span<char *const> args(argv, kLogLevelArgs);
    const std::string_view tag =
        (args[1] != nullptr) ? std::string_view{args[1]} : std::string_view{};
    const std::string_view lvl =
        (args[2] != nullptr) ? std::string_view{args[2]} : std::string_view{};
    if (tag.empty()) {
        return cli::fail("bad tag");
    }
    if (lvl.size() != 1U) {
        return cli::fail("level E|W|I|N");
    }
    esp_log_level_t level = ESP_LOG_NONE;
    switch (lvl[0]) {
    case 'E':
    case 'e':
        level = ESP_LOG_ERROR;
        break;
    case 'W':
    case 'w':
        level = ESP_LOG_WARN;
        break;
    case 'I':
    case 'i':
        level = ESP_LOG_INFO;
        break;
    case 'N':
    case 'n':
        level = ESP_LOG_NONE;
        break;
    default:
        // CONFIG_LOG_MAXIMUM_LEVEL=3（INFO），编译期不含 DEBUG/VERBOSE。
        return cli::fail("level E|W|I|N");
    }
    esp_log_level_set(args[1], level);
    return 0;
}

int cmdSysInfo(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys info");
    }
    const auto sys = appContext().data.systemStatus.read();
    esp_chip_info_t chip{CHIP_ESP32, 0U, 0U, 0U};
    esp_chip_info(&chip);

    std::array<uint8_t, core::kAddrLen> btMac{};
    std::array<char, kMacStrBuf> macBuf{};
    const esp_err_t macErr = esp_read_mac(btMac.data(), ESP_MAC_BT);
    if (macErr == ESP_OK) {
        core::BtAddr addr{};
        addr.bytes = btMac;
        formatMac(addr, macBuf);
    }

    const int64_t uptimeUs = esp_timer_get_time();
    const uint64_t uptimeMs = (uptimeUs >= 0) ? (static_cast<uint64_t>(uptimeUs) / kUsPerMs) : 0ULL;
    const uint64_t uptimeS = uptimeMs / kMsPerSec;

    cli::printSection("sys");
    // kAppVersion 长度远小于 int 上限，转换安全。
    cli::printKv("version", "%.*s", static_cast<int>(kAppVersion.size()), kAppVersion.data());
    cli::printKv("idf", "%s", esp_get_idf_version());
    cli::printKv("chip", "ESP32 rev %u.%u (%u cores)",
                 static_cast<unsigned>(chip.revision / kChipRevDivisor),
                 static_cast<unsigned>(chip.revision % kChipRevDivisor),
                 static_cast<unsigned>(chip.cores));
    cli::printKv("bt_mac", "%s", (macErr == ESP_OK) ? macBuf.data() : esp_err_to_name(macErr));
    cli::printKv("sn", "%s", sys.sn.data());
    cli::printKv("bt_name", "%s", sys.deviceName.data());
    cli::printKv("uptime", "%" PRIu64 " ms (%" PRIu64 " s)", uptimeMs, uptimeS);
    cli::printKv("reset_reason", "%s", app::resetReasonName(esp_reset_reason()));
    return 0;
}

int cmdSysHeap(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys heap");
    }
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t freeBytes = heap_caps_get_free_size(caps);
    const size_t minBytes = heap_caps_get_minimum_free_size(caps);
    const size_t largestBytes = heap_caps_get_largest_free_block(caps);

    const uint64_t freeKib = kibTenths(freeBytes);
    const uint64_t minKib = kibTenths(minBytes);
    const uint64_t largestKib = kibTenths(largestBytes);

    cli::printSection("heap");
    cli::printKv("free", "%zu B (%llu.%llu KB)", freeBytes,
                 static_cast<unsigned long long>(freeKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(freeKib % kTenthsPerUnit));
    cli::printKv("min", "%zu B (%llu.%llu KB)", minBytes,
                 static_cast<unsigned long long>(minKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(minKib % kTenthsPerUnit));
    cli::printKv("largest", "%zu B (%llu.%llu KB)", largestBytes,
                 static_cast<unsigned long long>(largestKib / kTenthsPerUnit),
                 static_cast<unsigned long long>(largestKib % kTenthsPerUnit));
    return 0;
}

int cmdSysMem(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys mem");
    }
    // 链接脚本符号是边界地址，不是同一 C++ 数组；先转 uintptr_t 再计算字节差。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): 链接脚本边界符号必须转换为地址数值。
    const auto dataBegin = reinterpret_cast<uintptr_t>(&_data_start);
    const auto dataEnd = reinterpret_cast<uintptr_t>(&_data_end);
    const auto bssBegin = reinterpret_cast<uintptr_t>(&_bss_start);
    const auto bssEnd = reinterpret_cast<uintptr_t>(&_bss_end);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    // ESP32 uintptr_t/size_t 同宽；边界顺序检查保证差值非负。
    const size_t staticDataBytes =
        (dataEnd >= dataBegin) ? static_cast<size_t>(dataEnd - dataBegin) : 0U;
    const size_t staticBssBytes =
        (bssEnd >= bssBegin) ? static_cast<size_t>(bssEnd - bssBegin) : 0U;

    std::array<MemoryHeapView, 4U> heaps{{
        {"internal_8bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 0U, 0U, 0U, 0U},
        {"8bit", MALLOC_CAP_8BIT, 0U, 0U, 0U, 0U},
        {"dma", MALLOC_CAP_DMA, 0U, 0U, 0U, 0U},
        {"default", MALLOC_CAP_DEFAULT, 0U, 0U, 0U, 0U},
    }};
    for (auto &view : heaps) {
        view.total = heap_caps_get_total_size(view.caps);
        view.free = heap_caps_get_free_size(view.caps);
        view.minimum = heap_caps_get_minimum_free_size(view.caps);
        view.largest = heap_caps_get_largest_free_block(view.caps);
    }
    printMemoryReport(appContext().bootArena, staticDataBytes, staticBssBytes, heaps);
    return 0;
}

void printTaskRow(const char *label, TaskHandle_t handle) noexcept
{
    if (handle == nullptr) {
        cli::printKv(label, "not running");
        return;
    }
    const BaseType_t core = xTaskGetCoreID(handle);
    const unsigned prio = static_cast<unsigned>(uxTaskPriorityGet(handle));
    // ESP-IDF 的 high-water mark 原始单位已经是字节（task.h），unsigned long 足以表示 UBaseType_t。
    const unsigned long hwmBytes = static_cast<unsigned long>(uxTaskGetStackHighWaterMark(handle));
    std::array<char, kCoreTextLen> coreBuf{};
    if (core == tskNO_AFFINITY) {
        (void)std::snprintf(coreBuf.data(), coreBuf.size(), "any");
    } else {
        (void)std::snprintf(coreBuf.data(), coreBuf.size(), "%d", static_cast<int>(core));
    }
    cli::printKv(label, "core=%-3s prio=%-2u stack_hwm=%lu B", coreBuf.data(), prio, hwmBytes);
}

int cmdSysTasks(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys tasks");
    }
    cli::printSection("tasks");
    printTaskRow("link", xTaskGetHandle("link"));
#if RTK_GNSS_ENABLED
    printTaskRow("io", xTaskGetHandle("io"));
#endif
    printTaskRow("spp_worker", xTaskGetHandle("spp_worker"));
#if FW_TRACE_ENABLED
    printTaskRow("trace_out", xTaskGetHandle("trace_out"));
#endif
    printTaskRow("console", xTaskGetCurrentTaskHandle());
    printTaskRow("idle0", xTaskGetIdleTaskHandleForCore(0));
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    printTaskRow("idle1", xTaskGetIdleTaskHandleForCore(1));
#endif
    return 0;
}

int cmdSysBoot(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys boot");
    }
    // 只读启动记录：boot() 返回后不再变化，console-only 阶段也可用。
    app::printBootTable(app::bootRecords());
    return 0;
}

int cmdSysReboot(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys reboot");
    }
    // 立即输出 OK（进 UART TX 环形缓冲），等待 100 ms 让中断发完再复位；esp_restart 不返回，trampoline 不会重复打印。
    cli::outPrintf("OK\n");
    vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
    esp_restart();
    return 0;
}

constexpr std::array<cli::Verb, 6U> kStatusVerbs{{
    {"overview", nullptr, "one-screen overview (default)", cmdStatusOverview},
    {"link", nullptr, "link, BT sessions, whitelist and alarm", cmdStatusLink},
    {"rtcm", nullptr, "RTCM forwarding and demux counters", cmdStatusRtcm},
    {"gnss", nullptr, "GNSS state, pipe, app ring and obs", cmdStatusGnss},
    {"sys", nullptr, "heap, uptime and executor stats", cmdStatusSys},
    {"all", nullptr, "full status of all sections", cmdStatusAll},
}};

constexpr std::array<cli::Verb, 1U> kLogVerbs{{
    {"level", "<tag|*> <E|W|I|N>", "set IDF log level (max INFO)", cmdLogLevel},
}};

constexpr std::array<cli::Verb, 6U> kSysVerbs{{
    {"info", nullptr, "firmware, IDF, chip, MAC, SN and uptime", cmdSysInfo},
    {"heap", nullptr, "internal RAM free, min and largest block", cmdSysHeap},
    {"mem", nullptr, "static DRAM, BootArena and heap_caps breakdown", cmdSysMem},
    {"boot", nullptr, "boot step table: result, timing and heap", cmdSysBoot},
    {"tasks", nullptr, "known task cores, priorities and stack HWM", cmdSysTasks},
    {"reboot", nullptr, "print OK, wait 100 ms and restart", cmdSysReboot},
}};

// 命令表项：签名由 cli::Cmd 的处理器类型固定；缺省动词 "overview" 在此注入。
int cmdStatus(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kStatusVerbs, ctx, argc, argv, "overview");
}

// 命令表项：签名由 cli::Cmd 的处理器类型固定，本函数只转发表与上下文。
int cmdLog(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kLogVerbs, ctx, argc, argv);
}

// 命令表项：签名由 cli::Cmd 的处理器类型固定，本函数只转发表与上下文。
int cmdSys(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kSysVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 3U> kSystemCmds{{
    {"status", "status [link|rtcm|gnss|sys|all]: system and link status",
     "[link|rtcm|gnss|sys|all]", cmdStatus, nullptr},
    {"log", "log level <tag|*> <E|W|I|N>: set IDF log level", "level <tag|*> <E|W|I|N>", cmdLog,
     nullptr},
    {"sys", "sys <info|heap|mem|boot|tasks|reboot>: system diagnostics and reboot",
     "<info|heap|mem|boot|tasks|reboot>", cmdSys, nullptr},
}};

} // namespace

std::span<const cli::Cmd> systemCmds() noexcept
{
    return kSystemCmds;
}

std::span<const cli::Verb> statusVerbs() noexcept
{
    return kStatusVerbs;
}

std::span<const cli::Verb> logVerbs() noexcept
{
    return kLogVerbs;
}

std::span<const cli::Verb> sysVerbs() noexcept
{
    return kSysVerbs;
}

} // namespace app
