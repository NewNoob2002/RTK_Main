// 模板模块控制台命令表（ADR-0009 §2.5 动词分派示例）：演示参数校验、guard clause、只读 DataCenter 与命令投递。
// 线程模型：仅在 console REPL 任务调用，非 ISR；不分配堆。
#include "modules/_template/TemplateModule.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <span>

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {
namespace {
constexpr size_t kLimitArgs = 2U;
constexpr size_t kBitsPerByte = 8U;
// 复制模板后，在 AppConfig.hpp 新增 ModuleKind，并改为 id(ModuleKind::k<Name>)。
constexpr fw::ModuleId kTargetPlaceholder = 0xFFU;

/**
 * @brief 显示模板依赖的系统状态
 * @param argc: 参数数量
 * @retval 0 成功
 * @retval 非零 参数或提交失败，已输出 ERR
 * @note 仅 console REPL 任务，可等待执行器或控制台输出，非 ISR。
 */
int cmdTmplStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: tmpl status");
    }
    const auto sys = appContext().data.systemStatus.read();
    const auto exec = appContext().data.execStats.read();
    cli::outPrintf("tmpl: bt=%s exec_rounds=%" PRIu32 "\n", sys.btReady ? "ready" : "down",
                   exec.rounds);
    return 0;
}

/**
 * @brief 校验并提交模板周期设置
 * @param argc: 参数数量
 * @param argv: 参数数组
 * @retval 0 成功
 * @retval 非零 参数或提交失败，已输出 ERR
 * @note 仅 console REPL 任务，可等待执行器或控制台输出，非 ISR。
 */
int cmdTmplLimit(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kLimitArgs)) || (argv == nullptr)) {
        return cli::fail("usage: tmpl limit <1..60000>");
    }
    const std::span<char *const> args(argv, kLimitArgs);
    uint32_t limitMs = 0U;
    if (!cli::parseU32(args[1], limitMs) || (limitMs == 0U) || (limitMs > kTemplateMaxLimitMs)) {
        // 1..60000 转 unsigned 安全。
        return cli::fail("limit 1-%u", static_cast<unsigned>(kTemplateMaxLimitMs));
    }
    fw::Command cmd{};
    cmd.target = kTargetPlaceholder;
    cmd.verb = verb(TemplateVerb::kSetLimit);
    // sizeof(uint32_t)==4，转 uint8_t 无损。
    cmd.argLen = static_cast<uint8_t>(sizeof(uint32_t));
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        // 右移后取低 8 位无损。
        cmd.args[i] = static_cast<uint8_t>(limitMs >> (i * kBitsPerByte));
    }
    return submit(cmd, "tmpl limit") ? 0 : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 2U> kTemplateVerbs{{
    {"status", nullptr, "show template module status", cmdTmplStatus},
    {"limit", "<ms>", "set poll step limit (1-60000 ms)", cmdTmplLimit},
}};

/**
 * @brief 分派模板子命令
 * @note 处理器 ABI 固定为 int(void *, int, char **)，不可添加参数 const。
 * @param ctx: 处理器上下文
 * @param argc: 参数数量
 * @param argv: 参数数组
 * @retval 0 成功
 * @retval 非零 参数或提交失败，已输出 ERR
 * @note 仅 console REPL 任务，可等待执行器或控制台输出，非 ISR。
 */
int cmdTmpl(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kTemplateVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 1U> kTemplateCmds{{
    {"tmpl", "tmpl <status|limit <ms>>: template module demo", "<status|limit <ms>>", cmdTmpl,
     nullptr},
}};
} // namespace

std::span<const cli::Cmd> templateCmds() noexcept
{
    return kTemplateCmds;
}

std::span<const cli::Verb> templateVerbs() noexcept
{
    return kTemplateVerbs;
}
} // namespace app
