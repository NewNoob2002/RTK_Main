// 模板模块（kLink 执行器）：演示模块最小接口、可选操作与命令接入。
// init 启动前调用；poll 与 onCommand 仅在 kLink 任务调用；均非阻塞、非 ISR。
#pragma once

#include <cstdint>
#include <span>
#include "cli/Console.hpp"
#include "fw/Command.hpp"
#include "fw/Result.hpp"

namespace app {
struct AppContext;

/** 模板模块命令动词（跨模块共享时移至 main/def/<name>_def.hpp）。 */
enum class TemplateVerb : uint8_t { kReset, kSetLimit };
/**
 * @brief 转换模板动词为数值编号
 * @param value: 模板命令动词
 * @retval 底层 uint8_t 编号
 * @note 纯函数，线程/ISR 安全。
 */
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
    uint32_t limitMs = 0U;
};

/** 模板模块（kLink 执行器）：示例化状态维护、周期步进与命令处理。 */
class TemplateModule final {
public:
    /**
     * @brief 记录模板模块所属上下文
     * @param context: 全生命周期有效的应用上下文
     * @note 仅构造时使用；不分配、不阻塞、不访问硬件。
     */
    constexpr explicit TemplateModule(AppContext &context) noexcept : context_(context) {}
    ~TemplateModule() = default;
    TemplateModule(const TemplateModule &) = delete;
    TemplateModule &operator=(const TemplateModule &) = delete;
    TemplateModule(TemplateModule &&) = delete;
    TemplateModule &operator=(TemplateModule &&) = delete;
    /**
     * @brief 初始化模板状态
     * @retval kOk 总是，重复调用保持状态
     * @note 仅启动前单线程，非 ISR，不阻塞。
     */
    [[nodiscard]] fw::Result init() noexcept;
    // start() 为可选操作，无需启动钩子时省略，fw::makeOps 自动置 nullptr。
    /**
     * @brief 按毫秒周期推进模板计数
     * @param nowMs: 当前单调时间（毫秒）
     * @retval false 总是，无需立即再轮询
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;
    /**
     * @brief 校验并处理模板清零与周期设置指令
     * @param cmd: 指令信封
     * @param[out] result: 执行结果，未初始化为 kInvalidState，非法动词或参数为 kInvalidArg
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief 读取模板状态副本
     * @retval 当前状态副本
     * @note 仅 kLink 或启动前，非 ISR，不阻塞。
     */
    [[nodiscard]] TemplateState state() const noexcept;

private:
    /**
     * @brief 清零模板步进计数
     * @retval kOk 总是
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    [[nodiscard]] fw::Result reset() noexcept;
    /**
     * @brief 校验并设置模板步进周期
     * @param cmd: 四字节小端周期，范围 1..60000 ms
     * @retval kOk 已设置
     * @retval kInvalidArg 长度或范围不合法，原值不变
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    [[nodiscard]] fw::Result setLimit(const fw::Command &cmd) noexcept;

    AppContext &context_;
    TemplateState state_{};
    bool timer_started_ = false;
    uint32_t last_tick_ms_ = 0U;
};
/**
 * @brief 取得模板命令表
 * @retval 静态只读命令表
 * @note 仅启动期登记，非 ISR，不阻塞。
 */
[[nodiscard]] std::span<const cli::Cmd> templateCmds() noexcept;
/**
 * @brief 取得模板动词表
 * @retval 静态只读动词表
 * @note 仅启动期登记，非 ISR，不阻塞。
 */
[[nodiscard]] std::span<const cli::Verb> templateVerbs() noexcept;
} // namespace app
