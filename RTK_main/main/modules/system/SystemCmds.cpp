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

uint64_t kib_tenths(uint64_t bytes) noexcept
{
    return (bytes * kTenthsPerUnit) / kBytesPerKib;
}

void print_link_section() noexcept
{
    const auto &data = app_context().data;
    const auto system = data.system_status.read();
    const auto link = data.link_status.read();
    const auto sessions = data.sessions.read();
    const auto store = data.store_status.read();
    const auto alarm = data.alarm_state.read();
    const auto stats = data.link_stats.read();
    const auto heartbeat = data.heartbeat.read();

    cli::printSection("link");
    cli::printKv("state", "%s", system.link_running ? "running" : "not running");
    cli::printKv("bt", "%s", system.bt_ready ? "ready" : "init failed");
    cli::printKv("policy", "%s", policy_name(link.policy));
    cli::printKv("pairing", "%s", link.pairing ? "open" : "closed");
    // 会话数与白名单数上限均不超过 kMaxAlarms/kMaxSessions，转换到 unsigned 无损。
    cli::printKv("sessions", "%u / %u", static_cast<unsigned>(sessions.count),
                 static_cast<unsigned>(kMaxSessions));
    std::array<char, kMacStrBuf> mac_buf{};
    for (size_t idx = 0U; idx < sessions.count; ++idx) {
        const SessionSnapshot &session = sessions.entries[idx];
        format_mac(session.addr, mac_buf);
        cli::printKv("session", "handle=0x%08" PRIx32 " role=%s mac=%s", session.handle,
                     role_name(session.role), mac_buf.data());
    }
    cli::printKv("whitelist", "%u (pending=%s, store=%s)",
                 static_cast<unsigned>(link.whitelist_count),
                 store.whitelist_pending ? "yes" : "no", esp_err_to_name(store.whitelist_error));
    for (size_t idx = 0U; idx < link.whitelist_count; ++idx) {
        format_mac(link.whitelist[idx], mac_buf);
        cli::printKv("wl_addr", "%s", mac_buf.data());
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
    cli::printKv("parser", "crc_err=%" PRIu32 " len_err=%" PRIu32, stats.parser_crc_errors,
                 stats.parser_length_errors);
    cli::printKv("spp_tx",
                 "accepted=%" PRIu32 " completed=%" PRIu32 " cancelled=%" PRIu32
                 " B errors=%" PRIu32,
                 stats.tx_accepted, stats.tx_completed, stats.tx_cancelled, stats.tx_errors);
    cli::printKv("spp_flow",
                 "cong=%" PRIu32 " high_water=%" PRIu32 " B paused=%" PRIu32 " inflight=%" PRIu32,
                 stats.tx_cong, stats.tx_high_water, stats.tx_paused, stats.tx_inflight);
    cli::printKv("spp_events",
                 "anon_write_fail=%" PRIu32 " stray=%" PRIu32 " dispatch_pauses=%" PRIu32
                 " cancelled=%" PRIu32,
                 stats.tx_anon_write_fail, stats.tx_stray_write, stats.dispatchPauses,
                 stats.dispatchCancelled);
    cli::printKv("spp_rx",
                 "evt_drop=%" PRIu32 " wakes=%" PRIu32 " s0=%" PRIu32 "/%" PRIu32 "(drop=%" PRIu32
                 ") "
                 "s1=%" PRIu32 "/%" PRIu32 "(drop=%" PRIu32 ")",
                 stats.evt_dropped, stats.rx_wake_calls, stats.rx_used[0], stats.rx_high_water[0],
                 stats.rx_dropped_bytes[0], stats.rx_used[1], stats.rx_high_water[1],
                 stats.rx_dropped_bytes[1]);
}

void print_sys_section() noexcept
{
    const auto &data = app_context().data;
    const auto system = data.system_status.read();
    const auto link = data.link_status.read();
    const auto exec = data.exec_stats.read();

    const uint64_t free_kib = kib_tenths(system.free_heap);
    const uint64_t min_kib = kib_tenths(system.min_free_heap);
    const int64_t uptime_us = esp_timer_get_time();
    const uint64_t uptime_ms =
        (uptime_us >= 0) ? (static_cast<uint64_t>(uptime_us) / kUsPerMs) : 0ULL;

    cli::printSection("sys");
    cli::printKv("uptime", "%" PRIu64 " ms", uptime_ms);
    cli::printKv("heap_free", "%" PRIu32 " B (%llu.%llu KB)", system.free_heap,
                 static_cast<unsigned long long>(free_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(free_kib % kTenthsPerUnit));
    cli::printKv("heap_min", "%" PRIu32 " B (%llu.%llu KB)", system.min_free_heap,
                 static_cast<unsigned long long>(min_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(min_kib % kTenthsPerUnit));
    cli::printKv("snapshot_ms", "%" PRIu32 " ms", link.snapshot_ms);
    cli::printKv("exec_rounds", "%" PRIu32, exec.rounds);
    cli::printKv("exec_overruns", "%" PRIu32, exec.overruns);
    cli::printKv("exec_max_rnd", "%" PRIu32 " ms", exec.maxRoundMs);
    cli::printKv("exec_cmds", "%" PRIu32, exec.commands);
    cli::printKv("exec_drops", "%" PRIu32, exec.resultDrops);
    cli::printKv("link_stk_hwm", "%" PRIu32 " B", exec.stackFree);
#if RTK_GNSS_ENABLED
    const auto io_exec = data.io_exec_stats.read();
    cli::printKv("io_rounds", "%" PRIu32, io_exec.rounds);
    cli::printKv("io_overruns", "%" PRIu32, io_exec.overruns);
    cli::printKv("io_max_rnd", "%" PRIu32 " ms", io_exec.maxRoundMs);
    cli::printKv("io_cmds", "%" PRIu32, io_exec.commands);
    cli::printKv("io_drops", "%" PRIu32, io_exec.resultDrops);
    cli::printKv("io_stk_hwm", "%" PRIu32 " B", io_exec.stackFree);
#endif
}

int cmd_status_overview(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status [link|rtcm|sys|all]");
    }
    const auto &data = app_context().data;
    const auto system = data.system_status.read();
    const auto link = data.link_status.read();
    const auto sessions = data.sessions.read();
    const auto alarm = data.alarm_state.read();
    const auto stats = data.link_stats.read();
    const auto rtcm = data.rtcm_stats.read();
    const auto demux = data.demux_stats.read();
    const auto exec = data.exec_stats.read();

    const uint64_t rtcm_kib = kib_tenths(rtcm.bytes);
    const uint64_t free_kib = kib_tenths(system.free_heap);
    const uint64_t min_kib = kib_tenths(system.min_free_heap);

    cli::printSection("link");
    cli::printKv("state", "%s (bt=%s, policy=%s, pair=%s)",
                 system.link_running ? "running" : "down", system.bt_ready ? "ready" : "err",
                 policy_name(link.policy), link.pairing ? "open" : "closed");
    // 会话数/白名单/报警字段均为小无符号整数，转换到 unsigned 无损。
    cli::printKv("sessions", "%u/%u  wl=%u  rx=%" PRIu32 "  tx=%" PRIu32 "  drop=%" PRIu32,
                 static_cast<unsigned>(sessions.count), static_cast<unsigned>(kMaxSessions),
                 static_cast<unsigned>(link.whitelist_count), stats.framesRx, stats.repliesTx,
                 stats.rxDropped);
    cli::printKv("alarm", "state=%u  level=%u  override=%s", static_cast<unsigned>(alarm.state),
                 static_cast<unsigned>(alarm.level), alarm.override ? "yes" : "no");

    cli::printSection("rtcm");
    cli::printKv("forward", "%" PRIu32 " frames, %llu.%llu KB, %" PRIu32 " B/s", rtcm.frames,
                 static_cast<unsigned long long>(rtcm_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(rtcm_kib % kTenthsPerUnit), rtcm.bytes_per_sec);
    cli::printKv("errors",
                 "role=%" PRIu32 "  unknown=%" PRIu32 " B  prod_rej=%" PRIu32 "  rtcm_rej=%" PRIu32,
                 rtcm.rejected_role, demux.unknown_bytes, demux.product_rejects,
                 demux.rtcm_rejects);

    cli::printSection("sys");
    cli::printKv("heap", "free=%llu.%llu KB  min=%llu.%llu KB",
                 static_cast<unsigned long long>(free_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(free_kib % kTenthsPerUnit),
                 static_cast<unsigned long long>(min_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(min_kib % kTenthsPerUnit));
    cli::printKv("exec",
                 "rounds=%" PRIu32 "  overruns=%" PRIu32 "  max_rnd=%" PRIu32
                 " ms  link_stk=%" PRIu32 " B",
                 exec.rounds, exec.overruns, exec.maxRoundMs, exec.stackFree);
#if RTK_GNSS_ENABLED
    const auto io_exec = data.io_exec_stats.read();
    const auto gnss = data.gnss_status.read();
    const auto r_wr = data.gnss_app_ring.typedWriterStats();
    const auto r_rd = data.gnss_app_ring.typedReaderStats(0U);
    const auto sub = data.gnss_app_sub.snapshot();
    cli::printKv("io_exec",
                 "rounds=%" PRIu32 "  overruns=%" PRIu32 "  max_rnd=%" PRIu32 " ms  io_stk=%" PRIu32
                 " B",
                 io_exec.rounds, io_exec.overruns, io_exec.maxRoundMs, io_exec.stackFree);
    cli::printSection("gnss");
    cli::printKv("state", "%s  model=%s  baud=%" PRIu32, ::gnss::stateName(gnss.state),
                 ::gnss::modelName(gnss.model), gnss.host_baud);
    cli::printKv("pipe", "push=%" PRIu32 "  drop=%" PRIu32 "  written=%" PRIu32 " B",
                 gnss.pipe.pushed_frames, gnss.pipe.dropped_frames, gnss.pipe.written_bytes);
    cli::printKv("app_ring",
                 "cap=%uB hwm=%zuB mask=0x%02" PRIX32 " wr=%" PRIu32 " rd=%" PRIu32 " drop=%" PRIu32
                 " qovsz=%" PRIu32,
                 static_cast<unsigned>(kGnssAppRingBytes), r_wr.highWaterBytes, sub.active_mask,
                 r_wr.base.records, r_rd.base.records,
                 r_rd.base.dropped + r_wr.oversizeDrop + r_wr.pinnedDrop, sub.query_oversize_total);
    cli::printKv("obs_ring", "reserved=%" PRIu32 "B not allocated (K6)", kStaticObsReservedBytes);
#endif
    return 0;
}

void print_gnss_section() noexcept
{
#if RTK_GNSS_ENABLED
    const auto &data = app_context().data;
    const auto gnss = data.gnss_status.read();
    const auto r_wr = data.gnss_app_ring.typedWriterStats();
    const auto r_rd = data.gnss_app_ring.typedReaderStats(0U);
    const auto sub = data.gnss_app_sub.snapshot();
    cli::printSection("gnss");
    cli::printKv("state", "%s  model=%s  baud=%" PRIu32, ::gnss::stateName(gnss.state),
                 ::gnss::modelName(gnss.model), gnss.host_baud);
    cli::printKv("pipe", "push=%" PRIu32 "  drop=%" PRIu32 "  written=%" PRIu32 " B",
                 gnss.pipe.pushed_frames, gnss.pipe.dropped_frames, gnss.pipe.written_bytes);
    cli::printKv("app_ring",
                 "cap=%uB hwm=%zuB mask=0x%02" PRIX32 " wr=%" PRIu32 "/%" PRIu32 "B overwr=%" PRIu32
                 " nosub=%" PRIu32 " ovsz=%" PRIu32 " pin=%" PRIu32,
                 static_cast<unsigned>(kGnssAppRingBytes), r_wr.highWaterBytes, sub.active_mask,
                 r_wr.base.records, r_wr.base.bytes, r_wr.base.overwritten, r_wr.noSubscriber,
                 r_wr.oversizeDrop, r_wr.pinnedDrop);
    cli::printKv("app_reader",
                 "rec=%" PRIu32 "/%" PRIu32 "B drop=%" PRIu32 " filt=%" PRIu32 " pin=%" PRIu32
                 " qovsz=%" PRIu32,
                 r_rd.base.records, r_rd.base.bytes, r_rd.base.dropped, r_rd.filtered,
                 r_rd.pinnedDrop, sub.query_oversize_total);
    cli::printKv("obs_ring", "reserved=%" PRIu32 "B not allocated (K6)", kStaticObsReservedBytes);
#else
    cli::print_section("gnss");
    cli::print_kv("state", "disabled");
#endif
}

int cmd_status_link(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status link");
    }
    print_link_section();
    return 0;
}

int cmd_status_rtcm(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status rtcm");
    }
    print_rtcm_status();
    return 0;
}

int cmd_status_gnss(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status gnss");
    }
    print_gnss_section();
    return 0;
}

int cmd_status_sys(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status sys");
    }
    print_sys_section();
    return 0;
}

int cmd_status_all(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: status all");
    }
    print_link_section();
    print_rtcm_status();
    print_gnss_section();
    print_sys_section();
    return 0;
}

int cmd_log_level(void * /*ctx*/, int argc, char **argv) noexcept
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

int cmd_sys_info(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys info");
    }
    const auto sys = app_context().data.system_status.read();
    esp_chip_info_t chip{CHIP_ESP32, 0U, 0U, 0U};
    esp_chip_info(&chip);

    std::array<uint8_t, core::kAddrLen> bt_mac{};
    std::array<char, kMacStrBuf> mac_buf{};
    const esp_err_t mac_err = esp_read_mac(bt_mac.data(), ESP_MAC_BT);
    if (mac_err == ESP_OK) {
        core::BtAddr addr{};
        addr.bytes = bt_mac;
        format_mac(addr, mac_buf);
    }

    const int64_t uptime_us = esp_timer_get_time();
    const uint64_t uptime_ms =
        (uptime_us >= 0) ? (static_cast<uint64_t>(uptime_us) / kUsPerMs) : 0ULL;
    const uint64_t uptime_s = uptime_ms / kMsPerSec;

    cli::printSection("sys");
    // kAppVersion 长度远小于 int 上限，转换安全。
    cli::printKv("version", "%.*s", static_cast<int>(kAppVersion.size()), kAppVersion.data());
    cli::printKv("idf", "%s", esp_get_idf_version());
    cli::printKv("chip", "ESP32 rev %u.%u (%u cores)",
                 static_cast<unsigned>(chip.revision / kChipRevDivisor),
                 static_cast<unsigned>(chip.revision % kChipRevDivisor),
                 static_cast<unsigned>(chip.cores));
    cli::printKv("bt_mac", "%s", (mac_err == ESP_OK) ? mac_buf.data() : esp_err_to_name(mac_err));
    cli::printKv("sn", "%s", sys.sn.data());
    cli::printKv("bt_name", "%s", sys.device_name.data());
    cli::printKv("uptime", "%" PRIu64 " ms (%" PRIu64 " s)", uptime_ms, uptime_s);
    cli::printKv("reset_reason", "%s", app::reset_reason_name(esp_reset_reason()));
    return 0;
}

int cmd_sys_heap(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys heap");
    }
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_bytes = heap_caps_get_free_size(caps);
    const size_t min_bytes = heap_caps_get_minimum_free_size(caps);
    const size_t largest_bytes = heap_caps_get_largest_free_block(caps);

    const uint64_t free_kib = kib_tenths(free_bytes);
    const uint64_t min_kib = kib_tenths(min_bytes);
    const uint64_t largest_kib = kib_tenths(largest_bytes);

    cli::printSection("heap");
    cli::printKv("free", "%zu B (%llu.%llu KB)", free_bytes,
                 static_cast<unsigned long long>(free_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(free_kib % kTenthsPerUnit));
    cli::printKv("min", "%zu B (%llu.%llu KB)", min_bytes,
                 static_cast<unsigned long long>(min_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(min_kib % kTenthsPerUnit));
    cli::printKv("largest", "%zu B (%llu.%llu KB)", largest_bytes,
                 static_cast<unsigned long long>(largest_kib / kTenthsPerUnit),
                 static_cast<unsigned long long>(largest_kib % kTenthsPerUnit));
    return 0;
}

int cmd_sys_mem(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys mem");
    }
    // 链接脚本符号是边界地址，不是同一 C++ 数组；先转 uintptr_t 再计算字节差。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): 链接脚本边界符号必须转换为地址数值。
    const auto data_begin = reinterpret_cast<uintptr_t>(&_data_start);
    const auto data_end = reinterpret_cast<uintptr_t>(&_data_end);
    const auto bss_begin = reinterpret_cast<uintptr_t>(&_bss_start);
    const auto bss_end = reinterpret_cast<uintptr_t>(&_bss_end);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    // ESP32 uintptr_t/size_t 同宽；边界顺序检查保证差值非负。
    const size_t static_data_bytes =
        (data_end >= data_begin) ? static_cast<size_t>(data_end - data_begin) : 0U;
    const size_t static_bss_bytes =
        (bss_end >= bss_begin) ? static_cast<size_t>(bss_end - bss_begin) : 0U;

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
    print_memory_report(app_context().boot_arena, static_data_bytes, static_bss_bytes, heaps);
    return 0;
}

void print_task_row(const char *label, TaskHandle_t handle) noexcept
{
    if (handle == nullptr) {
        cli::printKv(label, "not running");
        return;
    }
    const BaseType_t core = xTaskGetCoreID(handle);
    const unsigned prio = static_cast<unsigned>(uxTaskPriorityGet(handle));
    // ESP-IDF 的 high-water mark 原始单位已经是字节（task.h），unsigned long 足以表示 UBaseType_t。
    const unsigned long hwm_bytes = static_cast<unsigned long>(uxTaskGetStackHighWaterMark(handle));
    std::array<char, kCoreTextLen> core_buf{};
    if (core == tskNO_AFFINITY) {
        (void)std::snprintf(core_buf.data(), core_buf.size(), "any");
    } else {
        (void)std::snprintf(core_buf.data(), core_buf.size(), "%d", static_cast<int>(core));
    }
    cli::printKv(label, "core=%-3s prio=%-2u stack_hwm=%lu B", core_buf.data(), prio, hwm_bytes);
}

int cmd_sys_tasks(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys tasks");
    }
    cli::printSection("tasks");
    print_task_row("link", xTaskGetHandle("link"));
#if RTK_GNSS_ENABLED
    print_task_row("io", xTaskGetHandle("io"));
#endif
    print_task_row("spp_worker", xTaskGetHandle("spp_worker"));
#if FW_TRACE_ENABLED
    print_task_row("trace_out", xTaskGetHandle("trace_out"));
#endif
    print_task_row("console", xTaskGetCurrentTaskHandle());
    print_task_row("idle0", xTaskGetIdleTaskHandleForCore(0));
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    print_task_row("idle1", xTaskGetIdleTaskHandleForCore(1));
#endif
    return 0;
}

int cmd_sys_boot(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys boot");
    }
    // 只读启动记录：boot() 返回后不再变化，console-only 阶段也可用。
    app::print_boot_table(app::boot_records());
    return 0;
}

int cmd_sys_reboot(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
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
    {"overview", nullptr, "one-screen overview (default)", cmd_status_overview},
    {"link", nullptr, "link, BT sessions, whitelist and alarm", cmd_status_link},
    {"rtcm", nullptr, "RTCM forwarding and demux counters", cmd_status_rtcm},
    {"gnss", nullptr, "GNSS state, pipe, app ring and obs", cmd_status_gnss},
    {"sys", nullptr, "heap, uptime and executor stats", cmd_status_sys},
    {"all", nullptr, "full status of all sections", cmd_status_all},
}};

constexpr std::array<cli::Verb, 1U> kLogVerbs{{
    {"level", "<tag|*> <E|W|I|N>", "set IDF log level (max INFO)", cmd_log_level},
}};

constexpr std::array<cli::Verb, 6U> kSysVerbs{{
    {"info", nullptr, "firmware, IDF, chip, MAC, SN and uptime", cmd_sys_info},
    {"heap", nullptr, "internal RAM free, min and largest block", cmd_sys_heap},
    {"mem", nullptr, "static DRAM, BootArena and heap_caps breakdown", cmd_sys_mem},
    {"boot", nullptr, "boot step table: result, timing and heap", cmd_sys_boot},
    {"tasks", nullptr, "known task cores, priorities and stack HWM", cmd_sys_tasks},
    {"reboot", nullptr, "print OK, wait 100 ms and restart", cmd_sys_reboot},
}};

int cmd_status(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kStatusVerbs, ctx, argc, argv, "overview");
}

int cmd_log(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kLogVerbs, ctx, argc, argv);
}

int cmd_sys(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kSysVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 3U> kSystemCmds{{
    {"status", "status [link|rtcm|gnss|sys|all]: system and link status",
     "[link|rtcm|gnss|sys|all]", cmd_status, nullptr},
    {"log", "log level <tag|*> <E|W|I|N>: set IDF log level", "level <tag|*> <E|W|I|N>", cmd_log,
     nullptr},
    {"sys", "sys <info|heap|mem|boot|tasks|reboot>: system diagnostics and reboot",
     "<info|heap|mem|boot|tasks|reboot>", cmd_sys, nullptr},
}};

} // namespace

std::span<const cli::Cmd> system_cmds() noexcept
{
    return kSystemCmds;
}

std::span<const cli::Verb> status_verbs() noexcept
{
    return kStatusVerbs;
}

std::span<const cli::Verb> log_verbs() noexcept
{
    return kLogVerbs;
}

std::span<const cli::Verb> sys_verbs() noexcept
{
    return kSysVerbs;
}

} // namespace app
