// 系统模块控制台命令表（ADR-0009 §2.5）：status / log / sys。
// 线程：仅在 console REPL 任务调用，非 ISR；只读 DataCenter 或调用 IDF 系统/任务/日志查询接口。

#include "modules/system/system_module.hpp"

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
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_context.hpp"
#include "cli/out.hpp"
#include "fw/trace.hpp"
#include "modules/rtcm/rtcm_module.hpp"
#include "support/console_support.hpp"

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

uint64_t kib_tenths(uint64_t bytes) noexcept
{
    return (bytes * kTenthsPerUnit) / kBytesPerKib;
}

const char *reset_reason_str(esp_reset_reason_t reason) noexcept
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external-pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-wdt";
    case ESP_RST_TASK_WDT:
        return "task-wdt";
    case ESP_RST_WDT:
        return "other-wdt";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power-glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu-lockup";
    default:
        return "unknown";
    }
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

    cli::print_section("link");
    cli::print_kv("state", "%s", system.link_running ? "running" : "not running");
    cli::print_kv("bt", "%s", system.bt_ready ? "ready" : "init failed");
    cli::print_kv("policy", "%s", policy_name(link.policy));
    cli::print_kv("pairing", "%s", link.pairing ? "open" : "closed");
    // 会话数与白名单数上限均不超过 kMaxAlarms/kMaxSessions，转换到 unsigned 无损。
    cli::print_kv("sessions", "%u / %u", static_cast<unsigned>(sessions.count), static_cast<unsigned>(kMaxSessions));
    std::array<char, kMacStrBuf> mac_buf{};
    for (size_t idx = 0U; idx < sessions.count; ++idx) {
        const SessionSnapshot &session = sessions.entries[idx];
        format_mac(session.addr, mac_buf);
        cli::print_kv("session", "handle=0x%08" PRIx32 " role=%s mac=%s", session.handle, role_name(session.role),
                      mac_buf.data());
    }
    cli::print_kv("whitelist", "%u (pending=%s, store=%s)", static_cast<unsigned>(link.whitelist_count),
                  store.whitelist_pending ? "yes" : "no", esp_err_to_name(store.whitelist_error));
    for (size_t idx = 0U; idx < link.whitelist_count; ++idx) {
        format_mac(link.whitelist[idx], mac_buf);
        cli::print_kv("wl_addr", "%s", mac_buf.data());
    }
    // state/level 均为 uint8_t，转换到 unsigned 无损。
    cli::print_kv("alarm", "state=%u level=%u override=%s", static_cast<unsigned>(alarm.state),
                  static_cast<unsigned>(alarm.level), alarm.override ? "yes" : "no");
    if (heartbeat.present) {
        cli::print_kv("heartbeat", "seq=%u battery=%u%% state=%u", static_cast<unsigned>(heartbeat.value.seq),
                      static_cast<unsigned>(heartbeat.value.battery_pct),
                      static_cast<unsigned>(heartbeat.value.local_state));
    } else {
        cli::print_kv("heartbeat", "none");
    }
    cli::print_kv(
        "traffic", "rx=%" PRIu32 " tx=%" PRIu32 " push=%" PRIu32 " werr=%" PRIu32 " disc=%" PRIu32 " drop=%" PRIu32,
        stats.frames_rx, stats.replies_tx, stats.pushes_tx, stats.write_errors, stats.disconnects, stats.rx_dropped);
    cli::print_kv("parser", "crc_err=%" PRIu32 " len_err=%" PRIu32, stats.parser_crc_errors,
                  stats.parser_length_errors);
    cli::print_kv("spp_tx", "accepted=%" PRIu32 " completed=%" PRIu32 " cancelled=%" PRIu32 " B errors=%" PRIu32,
                  stats.tx_accepted, stats.tx_completed, stats.tx_cancelled, stats.tx_errors);
    cli::print_kv("spp_flow", "cong=%" PRIu32 " high_water=%" PRIu32 " B paused=%" PRIu32 " inflight=%" PRIu32,
                  stats.tx_cong, stats.tx_high_water, stats.tx_paused, stats.tx_inflight);
    cli::print_kv("spp_events",
                  "anon_write_fail=%" PRIu32 " stray=%" PRIu32 " dispatch_pauses=%" PRIu32 " cancelled=%" PRIu32,
                  stats.tx_anon_write_fail, stats.tx_stray_write, stats.dispatch_pauses, stats.dispatch_cancelled);
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
    const uint64_t uptime_ms = (uptime_us >= 0) ? (static_cast<uint64_t>(uptime_us) / kUsPerMs) : 0ULL;

    cli::print_section("sys");
    cli::print_kv("uptime", "%" PRIu64 " ms", uptime_ms);
    cli::print_kv("heap_free", "%" PRIu32 " B (%llu.%llu KB)", system.free_heap,
                  static_cast<unsigned long long>(free_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(free_kib % kTenthsPerUnit));
    cli::print_kv("heap_min", "%" PRIu32 " B (%llu.%llu KB)", system.min_free_heap,
                  static_cast<unsigned long long>(min_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(min_kib % kTenthsPerUnit));
    cli::print_kv("snapshot_ms", "%" PRIu32 " ms", link.snapshot_ms);
    cli::print_kv("exec_rounds", "%" PRIu32, exec.rounds);
    cli::print_kv("exec_overruns", "%" PRIu32, exec.overruns);
    cli::print_kv("exec_max_rnd", "%" PRIu32 " ms", exec.max_round_ms);
    cli::print_kv("exec_cmds", "%" PRIu32, exec.commands);
    cli::print_kv("exec_drops", "%" PRIu32, exec.result_drops);
    cli::print_kv("link_stk_hwm", "%" PRIu32 " B", exec.stack_free);
#if RTK_GNSS_ENABLED
    const auto io_exec = data.io_exec_stats.read();
    cli::print_kv("io_rounds", "%" PRIu32, io_exec.rounds);
    cli::print_kv("io_overruns", "%" PRIu32, io_exec.overruns);
    cli::print_kv("io_max_rnd", "%" PRIu32 " ms", io_exec.max_round_ms);
    cli::print_kv("io_cmds", "%" PRIu32, io_exec.commands);
    cli::print_kv("io_drops", "%" PRIu32, io_exec.result_drops);
    cli::print_kv("io_stk_hwm", "%" PRIu32 " B", io_exec.stack_free);
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

    cli::print_section("link");
    cli::print_kv("state", "%s (bt=%s, policy=%s, pair=%s)", system.link_running ? "running" : "down",
                  system.bt_ready ? "ready" : "err", policy_name(link.policy), link.pairing ? "open" : "closed");
    // 会话数/白名单/报警字段均为小无符号整数，转换到 unsigned 无损。
    cli::print_kv("sessions", "%u/%u  wl=%u  rx=%" PRIu32 "  tx=%" PRIu32 "  drop=%" PRIu32,
                  static_cast<unsigned>(sessions.count), static_cast<unsigned>(kMaxSessions),
                  static_cast<unsigned>(link.whitelist_count), stats.frames_rx, stats.replies_tx, stats.rx_dropped);
    cli::print_kv("alarm", "state=%u  level=%u  override=%s", static_cast<unsigned>(alarm.state),
                  static_cast<unsigned>(alarm.level), alarm.override ? "yes" : "no");

    cli::print_section("rtcm");
    cli::print_kv("forward", "%" PRIu32 " frames, %llu.%llu KB, %" PRIu32 " B/s", rtcm.frames,
                  static_cast<unsigned long long>(rtcm_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(rtcm_kib % kTenthsPerUnit), rtcm.bytes_per_sec);
    cli::print_kv("errors", "role=%" PRIu32 "  unknown=%" PRIu32 " B  prod_rej=%" PRIu32 "  rtcm_rej=%" PRIu32,
                  rtcm.rejected_role, demux.unknown_bytes, demux.product_rejects, demux.rtcm_rejects);

    cli::print_section("sys");
    cli::print_kv("heap", "free=%llu.%llu KB  min=%llu.%llu KB",
                  static_cast<unsigned long long>(free_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(free_kib % kTenthsPerUnit),
                  static_cast<unsigned long long>(min_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(min_kib % kTenthsPerUnit));
    cli::print_kv("exec", "rounds=%" PRIu32 "  overruns=%" PRIu32 "  max_rnd=%" PRIu32 " ms  link_stk=%" PRIu32 " B",
                  exec.rounds, exec.overruns, exec.max_round_ms, exec.stack_free);
#if RTK_GNSS_ENABLED
    const auto io_exec = data.io_exec_stats.read();
    const auto gnss = data.gnss_status.read();
    cli::print_kv("io_exec", "rounds=%" PRIu32 "  overruns=%" PRIu32 "  max_rnd=%" PRIu32 " ms  io_stk=%" PRIu32 " B",
                  io_exec.rounds, io_exec.overruns, io_exec.max_round_ms, io_exec.stack_free);
    cli::print_section("gnss");
    cli::print_kv("state", "%s  model=%s  baud=%" PRIu32, ::gnss::state_name(gnss.state),
                  ::gnss::model_name(gnss.model), gnss.host_baud);
    cli::print_kv("pipe", "push=%" PRIu32 "  drop=%" PRIu32 "  written=%" PRIu32 " B", gnss.pipe.pushed_frames,
                  gnss.pipe.dropped_frames, gnss.pipe.written_bytes);
#endif
    return 0;
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
    print_sys_section();
    return 0;
}

int cmd_log_level(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kLogLevelArgs)) || (argv == nullptr)) {
        return cli::fail("usage: log level <tag|*> <E|W|I|N>");
    }
    const std::span<char *const> args(argv, kLogLevelArgs);
    const std::string_view tag = (args[1] != nullptr) ? std::string_view{args[1]} : std::string_view{};
    const std::string_view lvl = (args[2] != nullptr) ? std::string_view{args[2]} : std::string_view{};
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
    const uint64_t uptime_ms = (uptime_us >= 0) ? (static_cast<uint64_t>(uptime_us) / kUsPerMs) : 0ULL;
    const uint64_t uptime_s = uptime_ms / kMsPerSec;

    cli::print_section("sys");
    // kAppVersion 长度远小于 int 上限，转换安全。
    cli::print_kv("version", "%.*s", static_cast<int>(kAppVersion.size()), kAppVersion.data());
    cli::print_kv("idf", "%s", esp_get_idf_version());
    cli::print_kv("chip", "ESP32 rev %u.%u (%u cores)", static_cast<unsigned>(chip.revision / kChipRevDivisor),
                  static_cast<unsigned>(chip.revision % kChipRevDivisor), static_cast<unsigned>(chip.cores));
    cli::print_kv("bt_mac", "%s", (mac_err == ESP_OK) ? mac_buf.data() : esp_err_to_name(mac_err));
    cli::print_kv("sn", "%s", sys.sn.data());
    cli::print_kv("bt_name", "%s", sys.device_name.data());
    cli::print_kv("uptime", "%" PRIu64 " ms (%" PRIu64 " s)", uptime_ms, uptime_s);
    cli::print_kv("reset_reason", "%s", reset_reason_str(esp_reset_reason()));
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

    cli::print_section("heap");
    cli::print_kv("free", "%zu B (%llu.%llu KB)", free_bytes,
                  static_cast<unsigned long long>(free_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(free_kib % kTenthsPerUnit));
    cli::print_kv("min", "%zu B (%llu.%llu KB)", min_bytes, static_cast<unsigned long long>(min_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(min_kib % kTenthsPerUnit));
    cli::print_kv("largest", "%zu B (%llu.%llu KB)", largest_bytes,
                  static_cast<unsigned long long>(largest_kib / kTenthsPerUnit),
                  static_cast<unsigned long long>(largest_kib % kTenthsPerUnit));
    return 0;
}

void print_task_row(const char *label, TaskHandle_t handle) noexcept
{
    if (handle == nullptr) {
        cli::print_kv(label, "not running");
        return;
    }
    const BaseType_t core = xTaskGetCoreID(handle);
    const unsigned prio = static_cast<unsigned>(uxTaskPriorityGet(handle));
    const unsigned long hwm_bytes = static_cast<unsigned long>(uxTaskGetStackHighWaterMark(handle)) *
                                    static_cast<unsigned long>(sizeof(StackType_t));
    std::array<char, kCoreTextLen> core_buf{};
    if (core == tskNO_AFFINITY) {
        (void)std::snprintf(core_buf.data(), core_buf.size(), "any");
    } else {
        (void)std::snprintf(core_buf.data(), core_buf.size(), "%d", static_cast<int>(core));
    }
    cli::print_kv(label, "core=%-3s prio=%-2u stack_hwm=%lu B", core_buf.data(), prio, hwm_bytes);
}

int cmd_sys_tasks(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys tasks");
    }
    cli::print_section("tasks");
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

int cmd_sys_reboot(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys reboot");
    }
    // 立即输出 OK（进 UART TX 环形缓冲），等待 100 ms 让中断发完再复位；esp_restart 不返回，trampoline 不会重复打印。
    cli::out_printf("OK\n");
    vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
    esp_restart();
    return 0;
}

constexpr std::array<cli::Verb, 5U> kStatusVerbs{{
    {"overview", nullptr, "one-screen overview (default)", cmd_status_overview},
    {"link", nullptr, "link, BT sessions, whitelist and alarm", cmd_status_link},
    {"rtcm", nullptr, "RTCM forwarding and demux counters", cmd_status_rtcm},
    {"sys", nullptr, "heap, uptime and executor stats", cmd_status_sys},
    {"all", nullptr, "full status of all sections", cmd_status_all},
}};

constexpr std::array<cli::Verb, 1U> kLogVerbs{{
    {"level", "<tag|*> <E|W|I|N>", "set IDF log level (max INFO)", cmd_log_level},
}};

constexpr std::array<cli::Verb, 4U> kSysVerbs{{
    {"info", nullptr, "firmware, IDF, chip, MAC, SN and uptime", cmd_sys_info},
    {"heap", nullptr, "internal RAM free, min and largest block", cmd_sys_heap},
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
    {"status", "status [link|rtcm|sys|all]: system and link status", "[link|rtcm|sys|all]", cmd_status, nullptr},
    {"log", "log level <tag|*> <E|W|I|N>: set IDF log level", "level <tag|*> <E|W|I|N>", cmd_log, nullptr},
    {"sys", "sys <info|heap|tasks|reboot>: system diagnostics and reboot", "<info|heap|tasks|reboot>", cmd_sys,
     nullptr},
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
