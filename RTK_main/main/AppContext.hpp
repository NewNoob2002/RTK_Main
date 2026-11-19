// 应用上下文拥有全部顶层可变对象，模块成员由注册清单生成。
// 仅 app_main 创建；各模块与执行器的运行期访问遵守各自线程契约，非 ISR。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

#include "AppConfig.hpp"
#include "app/BootConfig.hpp"
#include "app/RadioMode.hpp"
#include "fw/Atomic.hpp"
#include "data/AppData.hpp"
#if RTK_GNSS_ENABLED
#include "modules/gnss/RtcmPipe.hpp"
#endif
#include "module_list.inc"
#include "rtos/BootAlloc.hpp"

namespace app {

namespace board {
struct Profile;
}

/** 执行器非栈控制块存储（TCB 与命令/结果静态队列）；任务栈由启动期 boot_alloc 外供（ADR-0011 §7）。 */
template <size_t QueueDepth> struct ExecutorControlStorage {
    static_assert(QueueDepth > 0U, "queue depth must be > 0");
    StaticTask_t tcb{};
    std::array<uint8_t, QueueDepth * sizeof(fw::Command)> cmdArea{};
    StaticQueue_t cmdQueue{};
    std::array<uint8_t, QueueDepth * sizeof(fw::CommandResult)> resultArea{};
    StaticQueue_t resultQueue{};
};

/**
 * @brief 把执行器控制存储与外供栈组合成视图
 * @param st: 静态控制块与队列存储
 * @param stack: 外供任务栈（IDF 单位为字节），须覆盖任务生命周期
 * @retval 引用 st 与 stack 的执行器存储视图
 * @note 仅启动前单线程；不分配、不阻塞，非 ISR。
 */
template <size_t D>
[[nodiscard]] inline rtos::ExecutorStorageView viewOf(ExecutorControlStorage<D> &st,
                                                      std::span<StackType_t> stack) noexcept
{
    return rtos::ExecutorStorageView{stack,         &st.tcb,         st.cmdArea, &st.cmdQueue,
                                     st.resultArea, &st.resultQueue, D};
}

// 底层用 uint32_t：Xtensa LX6 没有 8 位原子 RMW，std::atomic<uint8_t> 会退回 libatomic 的锁实现（CPP-008）；
// 枚举体积让位于无锁原子（与下方 static_assert 一致），故不接受本条检查的优化建议。
// NOLINTNEXTLINE(performance-enum-size): 改成 1 字节会重新引入原子锁。
enum class BootPhase : uint32_t { kStarting = 0, kRunning, kConsoleOnly, kStopping };

/** 启动阶段的原子量必须无锁（CPP-008）：否则每次读相位都会进 libatomic 锁，启动屏障可能被阻塞。 */
static_assert(fw::kAtomicUsable<BootPhase>, "启动相位原子须无锁");
static_assert(fw::kAtomicUsable<uint32_t>, "启动屏障掩码原子须无锁");

/** 启动屏障任务位（CODING-STYLE 规则 3）：bootArrived/bootStopped/launched 掩码的各位；bit31 保留给看门狗诊断。 */
enum class BootTask : uint8_t {
    kLink = 1U << 0U,  ///< link 执行器任务
    kIo = 1U << 1U,    ///< io 执行器任务（GNSS）
    kTrace = 1U << 2U, ///< trace_out 任务
};

/**
 * @brief 取得启动屏障中的任务掩码位
 * @param task: 启动屏障任务
 * @retval task 对应的 uint32_t 掩码位
 * @note 纯函数，线程/ISR 安全。
 * @note BootTask → 掩码位；掩码仍为 uint32_t 以便原子 fetch_or。
 */
[[nodiscard]] constexpr uint32_t taskBit(BootTask task) noexcept
{
    return static_cast<uint32_t>(task);
}

/** 固件唯一上下文：拥有 BootArena、DataCenter、全部模块实例与执行器控制存储；全零静态初值落入 .bss。 */
struct AppContext {
    static constexpr size_t kLinkStackBytes = 4096;
#if RTK_GNSS_ENABLED
    static constexpr size_t kIoStackBytes = 4096;
#endif
    static constexpr size_t kModuleCount = 0
// X-macro 生成计数表达式，函数无法展开注册清单；前导 + 不可单独加括号。
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define MODULE_DEF(type, member, name, kind, executor, period, enable) +1
#include "module_list.inc"
#undef MODULE_DEF
        ;

    rtos::BootArena bootArena{};
    BootConfig bootConfig{};
    RadioMode radioMode = RadioMode::kBluetooth; // 启动任务在绑定上下文前写入，随后只读。
    std::optional<RadioReason> radioReason{};
    const board::Profile *boardProfile = nullptr; // 启动任务发布，之后只读。
    std::atomic<BootPhase> bootPhase{};
    std::atomic<uint32_t> bootArrived{};
    std::atomic<uint32_t> bootStopped{};

    /**
     * @brief 等待启动阶段放行或确认当前任务停稳
     * @param task: 当前任务的屏障位
     * @param watchdog: 是否在等待中喂狗并在停止时退订
     * @retval true 已进入 kRunning，可继续工作
     * @retval false 启动已停止或降级，调用方须退出任务
     * @note 仅执行器或 trace 任务调用；按 tick 阻塞，非 ISR。
     */
    [[nodiscard]] bool awaitBoot(BootTask task, bool watchdog) noexcept;

    /**
     * @brief  按步骤表完成启动；任一步失败则回滚并降级为 console-only，最后启动 REPL
     * @note   只在 app_main 任务调用一次。
     */
    void boot() noexcept;
    data::DataCenter data{};
// X-macro 生成异构成员声明（全零默认初值落 .bss），constexpr 函数不能替代。
#define MODULE_DEF(type, member, name, kind, executor, period, enable) type member{};
#include "module_list.inc"
#undef MODULE_DEF
    // 执行器本体含冻结非零默认成员（next_seq_/stats_mux_），由 AppMain.cpp 独立定义并以指针绑定，避免拖入 .data。
    rtos::Executor *linkExecutor = nullptr;
    ExecutorControlStorage<kModuleCount> linkStorage{};
#if RTK_GNSS_ENABLED
    rtos::Executor *ioExecutor = nullptr;
    ExecutorControlStorage<kModuleCount> ioStorage{};
    RtcmPipe rtcmPipe{};
#endif

    /**
     * @brief 把当前启动模式启用的模块绑定到本实例
     * @note 仅启动前单线程调用，不阻塞，非 ISR。

     */
    void bindContexts() noexcept
    {
// X-macro 批量绑定成员上下文指针，避免手写遗漏。
#define MODULE_DEF(type, member, name, kind, executor, period, enable)                             \
    if (moduleEnabled(radioMode, ModuleEnable::enable)) {                                          \
        (member).bindContext(*this);                                                               \
    }
#include "module_list.inc"
#undef MODULE_DEF
    }
};

/**
 * @brief 取得固件唯一的应用上下文
 * @retval 全生命周期有效的上下文引用
 * @note 任意任务可调用；不阻塞、不分配，非 ISR；成员访问遵守模块契约。

 */
[[nodiscard]] AppContext &appContext() noexcept;
} // namespace app
