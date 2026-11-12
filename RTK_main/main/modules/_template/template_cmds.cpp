// 模板模块控制台命令表：演示参数校验、guard clause、只读 DataCenter 与命令投递。
// 线程模型：仅在 console REPL 任务调用，非 ISR。
#include "modules/_template/template_module.hpp"
#include "app_context.hpp"
#include "support/console_support.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

namespace app {
namespace {
constexpr size_t kMinArgs = 2U;
constexpr size_t kLimitArgs = 3U;
constexpr size_t kBitsPerByte = 8U;
// 复制模板后，在 app_config.hpp 新增 ModuleKind，并改为 id(ModuleKind::k<Name>)。
constexpr fw::ModuleId kTargetPlaceholder = 0xFFU;

int handle_limit(std::span<char *const> args) noexcept
{
    if (args.size() != kLimitArgs) {
        printf("usage: tmpl limit <1..60000>\n");
        return 1;
    }
    uint32_t limit_ms = 0U;
    if (!cli::parse_u32(args[2], limit_ms) || (limit_ms == 0U) || (limit_ms > kTemplateMaxLimitMs)) {
        printf("invalid limit (1..%u ms)\n", static_cast<unsigned>(kTemplateMaxLimitMs)); // 1..60000 转 unsigned 安全
        return 1;
    }
    fw::Command cmd{};
    cmd.target = kTargetPlaceholder;
    cmd.verb = verb(TemplateVerb::kSetLimit);
    cmd.arg_len = static_cast<uint8_t>(sizeof(uint32_t)); // sizeof(uint32_t)==4，转 uint8_t 无损
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        cmd.args[i] = static_cast<uint8_t>(limit_ms >> (i * kBitsPerByte)); // 右移后取低 8 位无损
    }
    return submit(cmd, "tmpl limit") ? 0 : 1;
}

// NOLINTNEXTLINE(misc-const-correctness)
int cmd_tmpl(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 0) || (argv == nullptr)) {
        return 1;
    }
    const std::span<char *const> args(argv, static_cast<size_t>(argc)); // argc >= 0 已校验，转 size_t 无损
    if (args.size() < kMinArgs) {
        printf("usage: tmpl <status|limit <ms>>\n");
        return 1;
    }
    const std::string_view sub{args[1]};
    if (sub == "status") {
        if (args.size() != kMinArgs) {
            printf("usage: tmpl status\n");
            return 1;
        }
        const auto sys = app_context().data.system_status.read();
        const auto exec = app_context().data.exec_stats.read();
        printf("tmpl: bt=%s exec_rounds=%" PRIu32 "\n", sys.bt_ready ? "ready" : "down", exec.rounds);
        return 0;
    }
    if (sub == "limit") {
        return handle_limit(args);
    }
    printf("unknown subcommand: %s\n", args[1]);
    return 1;
}
constexpr std::array<cli::Cmd, 1> kTemplateCmds{{
    {"tmpl", "tmpl <status|limit <ms>>: template module demo", nullptr, cmd_tmpl, nullptr},
}};
} // namespace
std::span<const cli::Cmd> template_cmds() noexcept
{
    return kTemplateCmds;
}
} // namespace app
