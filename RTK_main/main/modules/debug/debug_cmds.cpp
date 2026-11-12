// debug 命令组（ADR-0009 §2.5）：list / on / off / report / save。
// 线程：仅 console REPL 任务调用，非 ISR。掩码是全局原子量，直接写；report/save 经 Executor::call 投递。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "app_context.hpp"
#include "cli/console.hpp"
#include "cli/out.hpp"
#include "def/debug_def.hpp"
#include "modules/debug/debug_module.hpp"
#include "modules/debug/trace_text.hpp"
#include "support/console_support.hpp"

namespace app {

namespace {

constexpr uint32_t kReportMinSeconds = 1U;
constexpr uint32_t kReportMaxSeconds = 3600U;
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kBitsPerByte = 8U;
constexpr const char *kAllCats = "all";

int cmd_list(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: debug list");
    }
    const DebugModule &module = app_context().debug;
    const fw::TraceRingStats ring = module.ring_stats();
    cli::out_printf("ring events=%u depth=%u pushed=%lu dropped=%lu max_depth=%u\n",
           static_cast<unsigned>(DebugModule::kRingEvents), static_cast<unsigned>(module.ring_depth()),
           static_cast<unsigned long>(ring.pushed), static_cast<unsigned long>(ring.dropped),
           static_cast<unsigned>(ring.max_depth));
    cli::out_printf("report period=%us%s\n", static_cast<unsigned>(module.report_period_s()),
           (module.report_period_s() == 0U) ? " (off)" : "");
    const uint32_t mask = fw::trace_mask();
    constexpr uint32_t kCount = static_cast<uint32_t>(fw::TraceCat::kCount);
    for (uint32_t index = 0U; index < kCount; ++index) {
        const auto cat = static_cast<fw::TraceCat>(index);
        const bool on = (mask & fw::trace_bit(cat)) != 0U;
        cli::out_printf("%-5s %-3s %s\n", debug::trace_cat_name(cat), on ? "on" : "off", debug::trace_cat_summary(cat));
    }
    return 0;
}

int toggle_cats(bool enable, int argc, char **argv) noexcept
{
    if ((argc < 0) || (argv == nullptr) || (argc < 2)) {
        return cli::fail("usage: debug %s <category...|all>", enable ? "on" : "off");
    }
    // argc ≥ 2 已校验，转 size_t 无损；用 span 索引避免裸指针算术。
    const std::span<char *const> args(argv, static_cast<size_t>(argc));
    uint32_t mask = fw::trace_mask();
    for (size_t index = 1U; index < args.size(); ++index) {
        const std::string_view name{args[index]};
        if (name == kAllCats) {
            mask = enable ? fw::kTraceAllMask : 0U;
            continue;
        }
        fw::TraceCat cat{};
        if (!debug::parse_cat(name, cat)) {
            return cli::fail("unknown category: %s", args[index]);
        }
        const uint32_t bit = fw::trace_bit(cat);
        mask = enable ? (mask | bit) : (mask & ~bit);
    }
    fw::trace_set_mask(mask);
    return 0;
}

int cmd_on(void * /*ctx*/, int argc, char **argv) noexcept
{
    return toggle_cats(true, argc, argv);
}

int cmd_off(void * /*ctx*/, int argc, char **argv) noexcept
{
    return toggle_cats(false, argc, argv);
}

int cmd_report(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != 2) || (argv == nullptr)) {
        return cli::fail("usage: debug report <1..3600|off>");
    }
    const std::span<char *const> args(argv, 2U);
    fw::Command command{};
    command.target = id(ModuleKind::kDebug);
    command.verb = verb(DebugVerb::kReport);
    command.arg_len = 2U;
    if (strcmp(args[1], "off") == 0) {
        command.args[0] = 0U;
        command.args[1] = 0U;
    } else {
        uint32_t seconds = 0U;
        if (!cli::parse_u32(args[1], seconds) || (seconds < kReportMinSeconds) || (seconds > kReportMaxSeconds)) {
            return cli::fail("invalid period: %s", args[1]);
        }
        // seconds ≤ 3600，取低字节与次低字节都不会丢位。
        command.args[0] = static_cast<uint8_t>(seconds & kByteMask);
        command.args[1] = static_cast<uint8_t>((seconds >> kBitsPerByte) & kByteMask);
    }
    return submit(command, "debug report") ? 0 : cli::kCmdFail;
}

int cmd_save(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: debug save");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kDebug);
    command.verb = verb(DebugVerb::kSave);
    return submit(command, "debug save") ? 0 : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 5U> kVerbs{{
    {"list", nullptr, "list trace categories, mask and ring stats", cmd_list},
    {"on", "<category...|all>", "enable trace categories", cmd_on},
    {"off", "<category...|all>", "disable trace categories", cmd_off},
    {"report", "<1..3600|off>", "set periodic report interval (s)", cmd_report},
    {"save", nullptr, "save mask and report interval", cmd_save},
}};

int cmd_debug(void *ctx, int argc, char **argv) noexcept
{
    if ((argc < 0) || (argv == nullptr)) {
        return cli::fail("usage: debug <list|on|off|report|save>");
    }
    // argc 由 esp_console 统计且 ≥ 0，转 size_t 无损。
    return cli::dispatch(kVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 1U> kCmds{{
    {"debug", "debug <list|on|off|report|save>: trace ring and report control", "<list|on|off|report|save>", cmd_debug,
     nullptr},
}};

} // namespace

std::span<const cli::Cmd> debug_cmds() noexcept
{
    return kCmds;
}

std::span<const cli::Verb> debug_verbs() noexcept
{
    return kVerbs;
}

} // namespace app
