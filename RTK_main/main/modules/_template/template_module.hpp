// 模板模块（kLink 执行器）：演示模块最小接口、可选操作与命令接入。
// init 启动前调用；poll 与 on_command 仅在 kLink 任务调用；均非阻塞、非 ISR。
#pragma once

#include <cstdint>
#include <span>
#include "cli/console.hpp"
#include "fw/command.hpp"
#include "fw/result.hpp"

namespace app {
struct AppContext;

/** 模板模块命令动词（跨模块共享时移至 main/def/<name>_def.hpp）。 */
enum class TemplateVerb : uint8_t { kReset, kSetLimit };
// 底层类型均为 uint8_t，转换无窄化；线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t verb(TemplateVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}
/** 步进周期上限（ms）；命令行与模块共用同一常量。 */
inline constexpr uint32_t kTemplateMaxLimitMs = 60000U;

/** 模板模块状态快照。 */
struct TemplateState {
    bool initialized = false;
    uint32_t ticks = 0U;
    uint32_t limit_ms = 0U;
};

/** 模板模块（kLink 执行器）：示例化状态维护、周期步进与命令处理。 */
class TemplateModule final {
public:
    constexpr explicit TemplateModule(AppContext &context) noexcept : context_(context) {}
    ~TemplateModule() = default;
    TemplateModule(const TemplateModule &) = delete;
    TemplateModule &operator=(const TemplateModule &) = delete;
    TemplateModule(TemplateModule &&) = delete;
    TemplateModule &operator=(TemplateModule &&) = delete;
    [[nodiscard]] fw::Result init() noexcept;
    // start() 为可选操作，无需启动钩子时省略，fw::make_ops 自动置 nullptr。
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    [[nodiscard]] TemplateState state() const noexcept;

private:
    [[nodiscard]] fw::Result reset() noexcept;
    [[nodiscard]] fw::Result set_limit(const fw::Command &cmd) noexcept;

    AppContext &context_;
    TemplateState state_{};
    bool timer_started_ = false;
    uint32_t last_tick_ms_ = 0U;
};
/** 模板模块控制台命令表；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> template_cmds() noexcept;
} // namespace app
