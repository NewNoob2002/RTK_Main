// 模板模块控制台命令表（ADR-0009 §2.5 动词分派示例）：演示参数校验、guard clause、只读 DataCenter 与命令投递。
// 线程模型：仅在 console REPL 任务调用，非 ISR；不分配堆。
#include "modules/_template/template_module.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <span>

#include "app_context.hpp"
#include "support/console_support.hpp"

namespace app {
namespace {
constexpr size_t kLimitArgs = 2U;
constexpr size_t kBitsPerByte = 8U;
// 复制模板后，在 app_config.hpp 新增 ModuleKind，并改为 id(ModuleKind::k<Name>)。
constexpr fw::ModuleId kTargetPlaceholder = 0xFFU;

int cmd_tmpl_status(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: tmpl status");
    }
    const auto sys = app_context().data.system_status.read();
    const auto exec = app_context().data.exec_stats.read();
    std::printf("tmpl: bt=%s exec_rounds=%" PRIu32 "\n", sys.bt_ready ? "ready" : "down", exec.rounds);
    return 0;
}

int cmd_tmpl_limit(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kLimitArgs)) || (argv == nullptr)) {
        return cli::fail("usage: tmpl limit <1..60000>");
    }
    const std::span<char *const> args(argv, kLimitArgs);
    uint32_t limit_ms = 0U;
    if (!cli::parse_u32(args[1], limit_ms) || (limit_ms == 0U) || (limit_ms > kTemplateMaxLimitMs)) {
        // 1..60000 转 unsigned 安全。
        return cli::fail("limit 1-%u", static_cast<unsigned>(kTemplateMaxLimitMs));
    }
    fw::Command cmd{};
    cmd.target = kTargetPlaceholder;
    cmd.verb = verb(TemplateVerb::kSetLimit);
    // sizeof(uint32_t)==4，转 uint8_t 无损。
    cmd.arg_len = static_cast<uint8_t>(sizeof(uint32_t));
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        // 右移后取低 8 位无损。
        cmd.args[i] = static_cast<uint8_t>(limit_ms >> (i * kBitsPerByte));
    }
    return submit(cmd, "tmpl limit") ? 0 : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 2U> kTemplateVerbs{{
    {"status", nullptr, "show template module status", cmd_tmpl_status},
    {"limit", "<ms>", "set poll step limit (1-60000 ms)", cmd_tmpl_limit},
}};

// esp_console_cmd_func_with_context_t 签名为 int(void *, int, char **)，不可改为 char *const *。
// NOLINTNEXTLINE(misc-const-correctness)
int cmd_tmpl(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kTemplateVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 1U> kTemplateCmds{{
    {"tmpl", "tmpl <status|limit <ms>>: template module demo", "<status|limit <ms>>", cmd_tmpl, nullptr},
}};
} // namespace

std::span<const cli::Cmd> template_cmds() noexcept
{
    return kTemplateCmds;
}

std::span<const cli::Verb> template_verbs() noexcept
{
    return kTemplateVerbs;
}
} // namespace app
