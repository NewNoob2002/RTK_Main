// 模块 = 唯一的扩展单位。模块操作表是函数指针结构体（不用 virtual，多态仍只限 Transport/Sink）。
// 每个模块只属于一个执行器：它的所有 ops 都只在该执行器任务中调用（init 除外，见下），内部状态因此无需加锁。
// 冻结接口（ADR-0007 §3.2）。
#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

#include "fw/Command.hpp"
#include "fw/Result.hpp"

namespace fw {

/** 模块编号；产品在 main 侧用 enum class 定义具体取值，运行期只作 uint8_t 使用。 */
using ModuleId = uint8_t; // 产品在 main/AppConfig.hpp 用 enum class 定义具体值，再转换为 ModuleId
/** 执行器编号；产品在 main 侧定义具体取值，运行期只作 uint8_t 使用。 */
using ExecutorId = uint8_t; // 同上

/** 模块操作表（函数指针集合）；表本身静态存储、生命周期与模块对象一致，各回调的线程约束由模块决定。 */
struct ModuleOps {
    /** 启动前、单线程（app_main）调用；可失败。失败的模块不会被 start/poll，其命令返回 kInvalidState。 */
    Result (*init)(void *self) noexcept = nullptr;
    /** 执行器任务启动后、第一次 poll 前调用一次（在执行器任务中）。可为 nullptr。 */
    Result (*start)(void *self) noexcept = nullptr;
    /**
     * 周期调用（执行器任务中），必须非阻塞，单次耗时应 ≤ 执行器 roundBudgetMs。
     * 返回 true 表示仍有待处理工作（例如 RX 积压），执行器下一轮等待时间取 0。可为 nullptr。
     */
    bool (*poll)(void *self, uint32_t nowMs) noexcept = nullptr;
    /** 处理 CommandBus 命令（执行器任务中）。result.seq 由执行器填写，模块只填 status/detail。可为 nullptr（→
     * kNotSupported）。 */
    void (*onCommand)(void *self, const Command &cmd, CommandResult &result) noexcept = nullptr;
};

/**
 * @brief  编译期按 T 实际提供的成员生成模块操作表。
 * @retval ModuleOps 只含 T 提供的 noexcept 成员对应的跳板，缺失的成员保持 nullptr
 * @note   consteval、无运行期分配；self 必须绑定到同一个 T 对象，调用上下文遵守 ModuleOps 契约。
 */
template <typename T> [[nodiscard]] consteval ModuleOps makeOps() noexcept
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
    if constexpr (requires(T &module, uint32_t nowMs) {
                      { module.poll(nowMs) } noexcept -> std::same_as<bool>;
                  }) {
        ops.poll = [](void *self, uint32_t nowMs) noexcept {
            // self 的类型约束同 init 跳板。
            return static_cast<T *>(self)->poll(nowMs);
        };
    }
    if constexpr (requires(T &module, const Command &cmd, CommandResult &result) {
                      { module.onCommand(cmd, result) } noexcept -> std::same_as<void>;
                  }) {
        ops.onCommand = [](void *self, const Command &cmd, CommandResult &result) noexcept {
            // self 的类型约束同 init 跳板。
            static_cast<T *>(self)->onCommand(cmd, result);
        };
    }
    return ops;
}

/** 模块注册项：把静态对象与操作表绑定到执行器和 ModuleId，由 Executor 在启动时校验。 */
struct Module {
    std::string_view name;     // 日志/控制台/状态输出用，静态字符串
    ModuleId id = 0;           // CommandBus 寻址，同一执行器内唯一
    ExecutorId executor = 0;   // 所属执行器
    uint16_t pollPeriodMs = 0; // 0 = 每轮都调用；>0 = 距上次调用 ≥ 该值才调用
    void *self = nullptr;
    const ModuleOps *ops = nullptr;
};

} // namespace fw
