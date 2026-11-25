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
#include "sdkconfig.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "app/PowerRequests.hpp"
#include "cli/Out.hpp"
#include "fw/Trace.hpp"
#include "modules/rtcm/RtcmModule.hpp"
#include "support/ConsoleSupport.hpp"
#include "SystemCmdsPrivate.hpp"

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

using detail::kChipRevDivisor;
using detail::kCoreTextLen;
using detail::kibTenths;
using detail::kLogLevelArgs;
using detail::kMsPerSec;
using detail::kRebootDelayMs;
using detail::kTenthsPerUnit;
using detail::kUsPerMs;

#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE)
constexpr bool kWifiConfigEnabled = true;
#else
constexpr bool kWifiConfigEnabled = false;
#endif

/**
 * @brief 查询或设置 IDF 日志级别
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
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

/**
 * @brief 打印版本、重启原因与身份信息
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
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
    const auto version = appVersion();
    // 版本字段最多 32 B，精度转换无损。
    cli::printKv("version", "%.*s", static_cast<int>(version.size()), version.data());
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

/**
 * @brief 打印当前与历史最小堆容量
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
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
    printInternalHeapBudget(minBytes, largestBytes, kWifiConfigEnabled);
    return 0;
}

/**
 * @brief 打印静态段和启动内存台账
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdSysMem(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys mem");
    }
    // 链接脚本符号是边界地址，不是同一 C++ 数组；先转 uintptr_t 再计算字节差。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): 链接脚本符号取地址
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
    printMemoryReport(appContext().bootArena, staticDataBytes, staticBssBytes, heaps,
                      kWifiConfigEnabled);
    return 0;
}

/**
 * @brief 打印指定任务的栈余量与状态
 * @param label: 显示任务名
 * @param handle: SPP 会话句柄
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
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

/**
 * @brief 打印任务栈余量及 OLED 单核 CPU 占用估算。
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
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
    const auto oledTask = xTaskGetHandle("oled");
    printTaskRow("oled", oledTask);
    if (oledTask != nullptr) {
        constexpr uint32_t kTenths = 10U;
        const uint32_t cpu = appContext().oled.recentPerf().cpuX10(oled::kPerfWindowMs);
        cli::printKv("oled_cpu_pct", "%" PRIu32 ".%" PRIu32, cpu / kTenths, cpu % kTenths);
    }
    printTaskRow("console", xTaskGetCurrentTaskHandle());
    printTaskRow("idle0", xTaskGetIdleTaskHandleForCore(0));
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    printTaskRow("idle1", xTaskGetIdleTaskHandleForCore(1));
#endif
    return 0;
}

/**
 * @brief 打印已执行的启动步骤记录
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdSysBoot(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys boot");
    }
    // 只读启动记录：boot() 返回后不再变化，console-only 阶段也可用。
    app::printBootTable(app::bootRecords());
    return 0;
}

/**
 * @brief 确认后重启固件
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdSysReboot(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: sys reboot");
    }
    if (!power::s_power_requests.claimRestart()) {
        return cli::fail("power action already in progress");
    }
    // 立即输出 OK（进 UART TX 环形缓冲），等待 100 ms 让中断发完再复位；esp_restart 不返回，trampoline 不会重复打印。
    cli::outPrintf("OK\n");
    vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
#if defined(ESP_PLATFORM)
    restartWithOledQuiesce(RebootBanner::kRebooting);
#else
    esp_restart();
#endif
    return 0;
}

constexpr std::array<cli::Verb, 6U> kStatusVerbs{{
    {"overview", nullptr, "one-screen overview (default)", detail::cmdStatusOverview},
    {"link", nullptr, "link, BT sessions, whitelist and alarm", detail::cmdStatusLink},
    {"rtcm", nullptr, "RTCM forwarding and demux counters", detail::cmdStatusRtcm},
    {"gnss", nullptr, "GNSS state, pipe, app ring and obs", detail::cmdStatusGnss},
    {"sys", nullptr, "heap, uptime and executor stats", detail::cmdStatusSys},
    {"all", nullptr, "full status of all sections", detail::cmdStatusAll},
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

/**
 * @brief 分派系统与链路状态子命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 * @note 命令表项：签名由 cli::Cmd 的处理器类型固定；缺省动词 "overview" 在此注入。
 */
int cmdStatus(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kStatusVerbs, ctx, argc, argv, "overview");
}

/**
 * @brief 分派日志级别子命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 * @note 命令表项：签名由 cli::Cmd 的处理器类型固定，本函数只转发表与上下文。
 */
int cmdLog(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kLogVerbs, ctx, argc, argv);
}

/**
 * @brief 分派系统诊断子命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 * @note 命令表项：签名由 cli::Cmd 的处理器类型固定，本函数只转发表与上下文。
 */
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
