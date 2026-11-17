// 应用上下文拥有全部顶层可变对象，模块成员由注册清单生成。
// 仅 app_main 创建；各模块与执行器的运行期访问遵守各自线程契约，非 ISR。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

#include "AppConfig.hpp"
#include "fw/Atomic.hpp"
#include "data/AppData.hpp"
#if RTK_GNSS_ENABLED
#include "modules/gnss/RtcmPipe.hpp"
#endif
#include "module_list.inc"
#include "rtos/BootAlloc.hpp"

namespace app {

/** 执行器非栈控制块存储（TCB 与命令/结果静态队列）；任务栈由启动期 boot_alloc 外供（ADR-0011 §7）。 */
template <size_t QueueDepth> struct ExecutorControlStorage {
    static_assert(QueueDepth > 0U, "queue depth must be > 0");
    StaticTask_t tcb{};
    std::array<uint8_t, QueueDepth * sizeof(fw::Command)> cmd_area{};
    StaticQueue_t cmd_queue{};
    std::array<uint8_t, QueueDepth * sizeof(fw::CommandResult)> result_area{};
    StaticQueue_t result_queue{};
};

template <size_t D>
[[nodiscard]] inline rtos::ExecutorStorageView view_of(ExecutorControlStorage<D> &st,
                                                       std::span<StackType_t> stack) noexcept
{
    return rtos::ExecutorStorageView{stack, &st.tcb, st.cmd_area, &st.cmd_queue, st.result_area, &st.result_queue, D};
}

// 底层用 uint32_t：Xtensa LX6 没有 8 位原子 RMW，std::atomic<uint8_t> 会退回 libatomic 的锁实现（CPP-008）；
// 枚举体积让位于无锁原子（与下方 static_assert 一致），故不接受本条检查的优化建议。
// NOLINTNEXTLINE(performance-enum-size): 改成 1 字节会重新引入原子锁。
enum class BootPhase : uint32_t { kStarting = 0, kRunning, kConsoleOnly, kStopping };

/** 启动阶段的原子量必须无锁（CPP-008）：否则每次读相位都会进 libatomic 锁，启动屏障可能被阻塞。 */
static_assert(fw::kAtomicUsable<BootPhase>, "启动相位原子须无锁");
static_assert(fw::kAtomicUsable<uint32_t>, "启动屏障掩码原子须无锁");

/** 启动屏障任务位（CODING-STYLE 规则 3）：boot_arrived/boot_stopped/launched 掩码的各位；bit31 保留给看门狗诊断。 */
enum class BootTask : uint8_t {
    kLink = 1U << 0U,  ///< link 执行器任务
    kIo = 1U << 1U,    ///< io 执行器任务（GNSS）
    kTrace = 1U << 2U, ///< trace_out 任务
};

/** BootTask → 掩码位；掩码仍为 uint32_t 以便原子 fetch_or。 */
[[nodiscard]] constexpr uint32_t task_bit(BootTask task) noexcept
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
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
#define MODULE_DEF(type, member, name, kind, executor, period) +1
#include "module_list.inc"
#undef MODULE_DEF
        ;

    rtos::BootArena boot_arena{};
    std::atomic<BootPhase> boot_phase{};
    std::atomic<uint32_t> boot_arrived{};
    std::atomic<uint32_t> boot_stopped{};

    /** 启动任务门控：执行器/trace 任务等待组合根放行；失败返回前退订 TWDT 并确认停稳，非 ISR。 */
    [[nodiscard]] bool await_boot(BootTask task, bool watchdog) noexcept;

    /**
     * 固件入口（R8a）：按有序步骤表完成启动，失败时按步骤语义降级/回滚/停机。
     * 仅 app_main 的启动任务调用一次，可阻塞；返回后控制台可用。
     */
    void boot() noexcept;
    data::DataCenter data{};
// X-macro 生成异构成员声明（全零默认初值落 .bss），constexpr 函数不能替代。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MODULE_DEF(type, member, name, kind, executor, period) type member{};
#include "module_list.inc"
#undef MODULE_DEF
    // 执行器本体含冻结非零默认成员（next_seq_/stats_mux_），由 AppMain.cpp 独立定义并以指针绑定，避免拖入 .data。
    rtos::Executor *link_executor = nullptr;
    ExecutorControlStorage<kModuleCount> link_storage{};
#if RTK_GNSS_ENABLED
    rtos::Executor *io_executor = nullptr;
    ExecutorControlStorage<kModuleCount> io_storage{};
    RtcmPipe rtcm_pipe{};
#endif

    /** 启动前单线程调用：把各模块的上下文指针绑定到本实例。 */
    void bind_contexts() noexcept
    {
// X-macro 批量绑定成员上下文指针，避免手写遗漏。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MODULE_DEF(type, member, name, kind, executor, period) member.bind_context(*this);
#include "module_list.inc"
#undef MODULE_DEF
    }
};

/** 返回固件全生命周期的上下文；无阻塞、无分配，非 ISR；成员访问遵守各模块契约。 */
[[nodiscard]] AppContext &app_context() noexcept;
} // namespace app
