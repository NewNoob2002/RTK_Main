// 冻结接口（ADR-0007 §3.2）。实现方不得修改。
// 模块 = 唯一的扩展单位。模块操作表是函数指针结构体（不用 virtual，多态仍只限 Transport/Sink）。
// 每个模块只属于一个执行器：它的所有 ops 都只在该执行器任务中调用（init 除外，见下），内部状态因此无需加锁。
//
// Generated-by: Claude Code + claude-opus-5-5
// Owner: UNKNOWN（待人工审查签字）
#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

#include "app/command.hpp"
#include "app/result.hpp"

namespace app {

using ModuleId = uint8_t;   // 产品在 main/app_ids.hpp 用 enum class 定义具体值，再转换为 ModuleId
using ExecutorId = uint8_t; // 同上

struct ModuleOps {
    /** 启动前、单线程（app_main）调用；可失败。失败的模块不会被 start/poll，其命令返回 kInvalidState。 */
    Result (*init)(void *self) noexcept = nullptr;
    /** 执行器任务启动后、第一次 poll 前调用一次（在执行器任务中）。可为 nullptr。 */
    Result (*start)(void *self) noexcept = nullptr;
    /**
     * 周期调用（执行器任务中），必须非阻塞，单次耗时应 ≤ 执行器 round_budget_ms。
     * 返回 true 表示仍有待处理工作（例如 RX 积压），执行器下一轮等待时间取 0。可为 nullptr。
     */
    bool (*poll)(void *self, uint32_t now_ms) noexcept = nullptr;
    /** 处理 CommandBus 命令（执行器任务中）。result.seq 由执行器填写，模块只填 status/detail。可为 nullptr（→
     * kNotSupported）。 */
    void (*on_command)(void *self, const Command &cmd, CommandResult &result) noexcept = nullptr;
};

/**
 * 编译期生成模块操作表；线程/ISR 安全，无运行期分配。
 * T 仅提供所需的 noexcept 成员；self 必须绑定到同一个 T 对象，调用上下文遵守 ModuleOps 契约。
 */
template <typename T> [[nodiscard]] consteval ModuleOps make_ops() noexcept
{
    ModuleOps ops{};
    if constexpr (requires(T &module) {
                      { module.init() } noexcept -> std::same_as<Result>;
                  }) {
        ops.init = [](void *self) noexcept {
            // 注册表将 T 对象地址与本操作表成对绑定，恢复原类型安全。
            return static_cast<T *>(self)->init();
        };
    }
    if constexpr (requires(T &module) {
                      { module.start() } noexcept -> std::same_as<Result>;
                  }) {
        ops.start = [](void *self) noexcept {
            // self 的类型约束同 init 跳板。
            return static_cast<T *>(self)->start();
        };
    }
    if constexpr (requires(T &module, uint32_t now_ms) {
                      { module.poll(now_ms) } noexcept -> std::same_as<bool>;
                  }) {
        ops.poll = [](void *self, uint32_t now_ms) noexcept {
            // self 的类型约束同 init 跳板。
            return static_cast<T *>(self)->poll(now_ms);
        };
    }
    if constexpr (requires(T &module, const Command &cmd, CommandResult &result) {
                      { module.on_command(cmd, result) } noexcept -> std::same_as<void>;
                  }) {
        ops.on_command = [](void *self, const Command &cmd, CommandResult &result) noexcept {
            // self 的类型约束同 init 跳板。
            static_cast<T *>(self)->on_command(cmd, result);
        };
    }
    return ops;
}

struct Module {
    std::string_view name;       // 日志/控制台/状态输出用，静态字符串
    ModuleId id = 0;             // CommandBus 寻址，同一执行器内唯一
    ExecutorId executor = 0;     // 所属执行器
    uint16_t poll_period_ms = 0; // 0 = 每轮都调用；>0 = 距上次调用 ≥ 该值才调用
    void *self = nullptr;
    const ModuleOps *ops = nullptr;
};

} // namespace app
