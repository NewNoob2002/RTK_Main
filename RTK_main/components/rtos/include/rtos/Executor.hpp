// 执行器 = 一个静态 FreeRTOS 任务 + 一组模块 + CommandBus（命令队列/结果队列）。
// 每轮：wait（钩子或命令队列，最长 idle_wait_ms；上一轮有模块报告积压则为 0）→ 取出全部待处理命令并分派给目标模块
//       → 按周期调用各模块 poll() → 统计本轮耗时（> round_budget_ms 计 overrun）→ 喂狗 → vTaskDelay(yield_ticks)。
// 模块的所有回调（init 除外）只在本执行器任务中调用。
// 冻结接口（ADR-0007 §3.2、§3.5）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Command.hpp"
#include "fw/Module.hpp"
#include "fw/Result.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fw/Status.hpp"

namespace rtos {

inline constexpr size_t kExecMaxModules = 8;
inline constexpr uint32_t kExecIdleWaitMs = 20;
inline constexpr uint32_t kExecRoundBudgetMs = 20;

/** 执行器构造参数；init 前设定，运行期只读。 */
struct ExecutorConfig {
    const char *name = nullptr;
    fw::ExecutorId id = 0;
    uint32_t priority = 1;
    BaseType_t core = 0;
    uint32_t idle_wait_ms = kExecIdleWaitMs;       // 无积压时每轮最长等待
    uint32_t round_budget_ms = kExecRoundBudgetMs; // 单轮（不含 wait 与 yield）超过此值计 overrun
    uint32_t yield_ticks = 1;                      // 每轮末 vTaskDelay；0 = 不让出（不建议）
    bool watchdog = true;                          // 任务启动后 esp_task_wdt_add 自己，每轮 reset
    /**
     * 可选等待钩子（执行器任务中调用）：最长阻塞 max_ms，可以顺带处理它等到的事件（例如 BT 事件）。
     * 为 nullptr 时执行器阻塞在命令队列上（命令到达立即唤醒）。
     */
    void (*wait)(void *ctx, uint32_t max_ms) noexcept = nullptr;
    void *wait_ctx = nullptr;
};

/** 执行器运行统计（临界区内累加），由 stats() 拷贝出快照。 */
struct ExecStats {
    uint32_t rounds = 0;
    uint32_t overruns = 0;
    uint32_t max_round_ms = 0;
    uint32_t commands = 0;     // 已分派的命令数
    uint32_t result_drops = 0; // 结果队列满时丢弃的最旧结果数
    uint32_t stack_free = 0;   // 栈高水位（字节），每轮更新
};

/** 执行器所需的全部静态存储；由调用方定义为 constinit 全局并交给 Executor::init。 */
template <size_t StackBytes, size_t QueueDepth> struct ExecutorStorage {
    static_assert((StackBytes % sizeof(StackType_t)) == 0U, "stack must be word-aligned");
    static_assert(QueueDepth > 0U, "queue depth must be > 0");
    std::array<StackType_t, StackBytes / sizeof(StackType_t)> stack{};
    StaticTask_t tcb{};
    std::array<uint8_t, QueueDepth * sizeof(fw::Command)> cmd_area{};
    StaticQueue_t cmd_queue{};
    std::array<uint8_t, QueueDepth * sizeof(fw::CommandResult)> result_area{};
    StaticQueue_t result_queue{};
};

/** ExecutorStorage 的类型擦除视图（避免 Executor 本身成为模板）。 */
struct ExecutorStorageView {
    std::span<StackType_t> stack;
    StaticTask_t *tcb = nullptr;
    std::span<uint8_t> cmd_area;
    StaticQueue_t *cmd_queue = nullptr;
    std::span<uint8_t> result_area;
    StaticQueue_t *result_queue = nullptr;
    size_t queue_depth = 0;
};

template <size_t S, size_t D>
[[nodiscard]] ExecutorStorageView view_of(ExecutorStorage<S, D> &st) noexcept
{
    return ExecutorStorageView{
        st.stack, &st.tcb, st.cmd_area, &st.cmd_queue, st.result_area, &st.result_queue, D};
}

/** 静态任务 + 模块表 + 命令/结果队列的执行器；init 后由本任务独占运行，外部只经 call()/stats() 交互。 */
class Executor {
public:
    constexpr Executor() noexcept = default;
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    Executor(Executor &&) = delete;
    Executor &operator=(Executor &&) = delete;
    ~Executor() = default;

    /**
     * 启动前（单线程，app_main）调用：保存配置，创建两个静态队列，并对每个 module.executor == cfg.id 的模块调用
     * ops->init。 模块 init 失败：记录日志并把该模块标记为不可用（不 start、不 poll，命令返回
     * kInvalidState），整体仍返回 kOk。 参数非法（name 为空、模块数 > kExecMaxModules、同执行器内 ModuleId 重复、ops
     * 为空）→ kInvalidArg；重复调用 → kBusy。 modules 必须指向静态存储（执行器保存该 span）。
     */
    [[nodiscard]] fw::Status init(const ExecutorConfig &cfg, std::span<const fw::Module> modules,
                                  const ExecutorStorageView &storage) noexcept;

    /** 创建任务。任务首先对可用模块依次调用 ops->start（失败同样标记为不可用），然后进入轮询。未 init → kInvalidState。
     */
    [[nodiscard]] fw::Status start() noexcept;

    /**
     * CommandBus：从其他任务同步调用目标模块的 on_command。分配 seq，入命令队列，等待 seq 匹配的结果
     * （丢弃 seq 不匹配的过期结果）。线程安全：同一时刻只允许一个调用者任务（当前为控制台）。
     * 返回：kOk（result 有效，result.status 为模块结果）、kTimeout（等待超时，命令可能稍后仍被执行）、
     *       kBusy（命令队列满）、kInvalidState（未运行，或从本执行器任务自身调用——会死锁）、
     *       kNotFound（目标模块不存在）。
     */
    [[nodiscard]] fw::Result call(const fw::Command &cmd, fw::CommandResult &result,
                                  uint32_t timeout_ms) noexcept;

    [[nodiscard]] bool running() const noexcept;
    /** 任意任务可调用（内部临界区拷贝）。 */
    [[nodiscard]] ExecStats stats() const noexcept;
    /** 模块是否可用（init/start 成功）。任意任务可调用。 */
    [[nodiscard]] bool module_ok(fw::ModuleId id) const noexcept;

private:
    static void task_entry(void *arg) noexcept;
    void run() noexcept;

    ExecutorConfig cfg_{};
    std::span<const fw::Module> modules_{};
    std::array<bool, kExecMaxModules> ok_{};
    std::array<uint32_t, kExecMaxModules> last_poll_ms_{};
    ExecutorStorageView storage_{};
    QueueHandle_t cmd_q_ = nullptr;
    QueueHandle_t result_q_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint32_t next_seq_ = 1;
    ExecStats stats_{};
    mutable portMUX_TYPE stats_mux_ = portMUX_INITIALIZER_UNLOCKED;
    bool inited_ = false;
};

} // namespace rtos
