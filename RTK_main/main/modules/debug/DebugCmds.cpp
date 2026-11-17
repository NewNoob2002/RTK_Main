// debug 命令组（ADR-0009 §2.5 / §2.11）：list / on / off / report / save。
// 线程：仅 console REPL 任务调用，非 ISR。掩码是全局原子量，直接写；report/save 经 Executor::call 投递。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "AppContext.hpp"
#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "fw/Log.hpp"
#include "def/DebugDef.hpp"
#include "modules/debug/DebugModule.hpp"
#include "modules/debug/TraceText.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

constexpr uint32_t kReportMinSeconds = 1U;
constexpr uint32_t kReportMaxSeconds = 3600U;
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kBitsPerByte = 8U;
constexpr const char *kAllCats = "all";
constexpr const char *kHexSuffix = "hex";

/**
 * @brief  打印各类别的追踪开关与日志级别（`debug list` 与 `debug log` 共用）
 */
void printCatTable() noexcept
{
    const uint32_t mask = fw::traceMask();
    const uint32_t hexMask = fw::logHexMask();
    cli::outPrintf("cat   trace log  summary\n");
    constexpr uint32_t kCount = static_cast<uint32_t>(fw::TraceCat::kCount);
    for (uint32_t index = 0U; index < kCount; ++index) {
        const auto cat = static_cast<fw::TraceCat>(index);
        const bool on = (mask & fw::traceBit(cat)) != 0U;
        const bool hex = (hexMask & fw::traceBit(cat)) != 0U;
        cli::outPrintf("%-5s %-5s %c%-3s %s\n", debug::traceCatName(cat), on ? "on" : "off",
                       fw::logLevelChar(fw::logCatLevel(cat)), hex ? "+hx" : "",
                       debug::traceCatSummary(cat));
    }
}

/**
 * @brief  `debug list`：打印两个环的统计、报告周期与各类别的追踪开关/日志级别
 * @param  argc: 参数个数（不接受额外参数）
 * @retval 0 成功；非 0 参数错误
 */
int cmdList(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: debug list");
    }
    const DebugModule &module = appContext().debug;
    const fw::TraceRingStats ring = module.ringStats();
    cli::outPrintf("trace ring events=%u depth=%u pushed=%lu dropped=%lu max_depth=%u\n",
                   static_cast<unsigned>(DebugModule::kRingEvents),
                   static_cast<unsigned>(module.ringDepth()),
                   static_cast<unsigned long>(ring.pushed),
                   static_cast<unsigned long>(ring.dropped), static_cast<unsigned>(ring.maxDepth));
    const fw::LogRingStats log = module.logRingStats();
    cli::outPrintf("log ring bytes=%u used=%u pushed=%lu dropped=%lu max_used=%u lost=%lu\n",
                   static_cast<unsigned>(DebugModule::kLogRingBytes),
                   static_cast<unsigned>(module.logRingUsed()),
                   static_cast<unsigned long>(log.pushed), static_cast<unsigned long>(log.dropped),
                   static_cast<unsigned>(log.maxUsed),
                   static_cast<unsigned long>(fw::logStats().lost));
    cli::outPrintf("report period=%us%s\n", static_cast<unsigned>(module.reportPeriodS()),
                   (module.reportPeriodS() == 0U) ? " (off)" : "");
    printCatTable();
    return 0;
}

/**
 * @brief  解析类别列表（名称或 all）为位掩码
 * @param  args: 类别名
 * @param[out] bits: 选中类别的位掩码
 * @retval true 全部认识
 * @retval false 有不认识的名字（已打印 ERR）
 */
bool parseCatList(std::span<char *const> args, uint32_t &bits) noexcept
{
    bits = 0U;
    for (const char *const arg : args) {
        const std::string_view name{arg};
        if (name == kAllCats) {
            bits = fw::kTraceAllMask;
            continue;
        }
        fw::TraceCat cat{};
        if (!debug::parseCat(name, cat)) {
            static_cast<void>(cli::fail("unknown category: %s", arg));
            return false;
        }
        bits |= fw::traceBit(cat);
    }
    return true;
}

/**
 * @brief  开关追踪类别：args = {"on"|"off", <cat...|all>}
 * @param  args: 参数（不含 "debug"/"trace"）
 * @retval 0 成功；非 0 参数错误
 */
int toggleTrace(std::span<char *const> args) noexcept
{
    if (args.size() < 2U) {
        return cli::fail("usage: debug trace <on|off> <category...|all>");
    }
    const std::string_view action{args[0]};
    if ((action != "on") && (action != "off")) {
        return cli::fail("usage: debug trace <on|off> <category...|all>");
    }
    uint32_t bits = 0U;
    if (!parseCatList(args.subspan(1U), bits)) {
        return cli::kCmdFail;
    }
    const uint32_t mask = fw::traceMask();
    fw::traceSetMask((action == "on") ? (mask | bits) : (mask & ~bits));
    return 0;
}

/**
 * @brief  `debug trace <on|off> <cat...|all>`：开关实时追踪事件（与日志级别无关）
 * @param  argc: 参数个数（含 "trace"）
 * @param  argv: 参数表
 * @retval 0 成功；非 0 参数错误
 */
int cmdTrace(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 1) || (argv == nullptr)) {
        return cli::fail("usage: debug trace <on|off> <category...|all>");
    }
    const std::span<char *const> args(argv, static_cast<size_t>(argc));
    return toggleTrace(args.subspan(1U));
}

/**
 * @brief  `debug on|off <cat...|all>`：`debug trace on|off` 的简写
 * @param  argc: 参数个数（含 "on"/"off"）
 * @param  argv: 参数表
 * @retval 0 成功；非 0 参数错误
 */
int cmdOnOff(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 1) || (argv == nullptr)) {
        return cli::fail("usage: debug <on|off> <category...|all>");
    }
    return toggleTrace(std::span<char *const>(argv, static_cast<size_t>(argc)));
}

/**
 * @brief  `debug log [<cat...|all> <e|w|i|d> [hex]]`：查看或设置各类别日志级别
 * @param  argc: 参数个数（含 "log"）
 * @param  argv: 参数表
 * @retval 0 成功；非 0 参数错误
 * @note   hex 打开这些类别的原始字节转储（只在 D 级生效），不带 hex 则关闭；`debug save` 写 NVS。
 */
int cmdLog(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 1) || (argv == nullptr)) {
        return cli::fail("usage: debug log [<category...|all> <e|w|i|d> [hex]]");
    }
    std::span<char *const> args =
        std::span<char *const>(argv, static_cast<size_t>(argc)).subspan(1U);
    if (args.empty()) {
        printCatTable();
        return 0;
    }
    bool wantHex = false;
    if (std::string_view(args.back()) == kHexSuffix) {
        wantHex = true;
        args = args.first(args.size() - 1U);
    }
    fw::LogLevel level{};
    if ((args.size() < 2U) || !fw::logParseLevel(args.back(), level)) {
        return cli::fail("usage: debug log [<category...|all> <e|w|i|d> [hex]]");
    }
    uint32_t bits = 0U;
    if (!parseCatList(args.first(args.size() - 1U), bits)) {
        return cli::kCmdFail;
    }
    constexpr uint32_t kCount = static_cast<uint32_t>(fw::TraceCat::kCount);
    for (uint32_t index = 0U; index < kCount; ++index) {
        const auto cat = static_cast<fw::TraceCat>(index);
        if ((bits & fw::traceBit(cat)) != 0U) {
            fw::logSetCatLevel(cat, level);
        }
    }
    const uint32_t hexMask = fw::logHexMask();
    fw::logSetHexMask(wantHex ? (hexMask | bits) : (hexMask & ~bits));
    return 0;
}

int cmdReport(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != 2) || (argv == nullptr)) {
        return cli::fail("usage: debug report <1..3600|off>");
    }
    const std::span<char *const> args(argv, 2U);
    fw::Command command{};
    command.target = id(ModuleKind::kDebug);
    command.verb = verb(DebugVerb::kReport);
    command.argLen = 2U;
    if (strcmp(args[1], "off") == 0) {
        command.args[0] = 0U;
        command.args[1] = 0U;
    } else {
        uint32_t seconds = 0U;
        if (!cli::parseU32(args[1], seconds) || (seconds < kReportMinSeconds) ||
            (seconds > kReportMaxSeconds)) {
            return cli::fail("invalid period: %s", args[1]);
        }
        // seconds ≤ 3600，取低字节与次低字节都不会丢位。
        command.args[0] = static_cast<uint8_t>(seconds & kByteMask);
        command.args[1] = static_cast<uint8_t>((seconds >> kBitsPerByte) & kByteMask);
    }
    return submit(command, "debug report") ? 0 : cli::kCmdFail;
}

int cmdSave(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: debug save");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kDebug);
    command.verb = verb(DebugVerb::kSave);
    return submit(command, "debug save") ? 0 : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 7U> kVerbs{{
    {"list", nullptr, "ring stats, trace switches and log levels per category", cmdList},
    {"trace", "<on|off> <category...|all>", "enable/disable real-time trace events", cmdTrace},
    {"log", "[<category...|all> <e|w|i|d> [hex]]", "show or set per-category log level", cmdLog},
    {"on", "<category...|all>", "short for: debug trace on", cmdOnOff},
    {"off", "<category...|all>", "short for: debug trace off", cmdOnOff},
    {"report", "<1..3600|off>", "set periodic report interval (s)", cmdReport},
    {"save", nullptr, "save trace mask, log levels, hex mask and report interval", cmdSave},
}};

/**
 * @brief  `debug` 命令组入口：按动词分发
 * @param  ctx: 透传给动词处理函数
 * @param  argc: 参数个数（含 "debug"）
 * @param  argv: 参数表
 * @retval 动词处理函数的返回值；参数错误非 0
 */
int cmdDebug(void *ctx, int argc, char **argv) noexcept
{
    if ((argc < 0) || (argv == nullptr)) {
        return cli::fail("usage: debug <list|trace|log|on|off|report|save>");
    }
    // argc 由 esp_console 统计且 ≥ 0，转 size_t 无损。
    return cli::dispatch(kVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 1U> kCmds{{
    {"debug", "debug <list|on|off|level|report|save>: trace ring, log level and report control",
     "<list|on|off|level|report|save>", cmdDebug, nullptr},
}};

} // namespace

std::span<const cli::Cmd> debugCmds() noexcept
{
    return kCmds;
}

std::span<const cli::Verb> debugVerbs() noexcept
{
    return kVerbs;
}

} // namespace app
