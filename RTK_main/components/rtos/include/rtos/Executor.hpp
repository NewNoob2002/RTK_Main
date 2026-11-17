// 执行器 = 静态 FreeRTOS 任务 + 模块表 + CommandBus；每轮 等待→分派命令→周期 poll→统计→喂狗→让出。
// 模块回调（init 除外）只在执行器任务中调用；不分配堆、不在 ISR 中调用。
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
    uint32_t idleWaitMs = kExecIdleWaitMs;       // 无积压时每轮最长等待
    uint32_t roundBudgetMs = kExecRoundBudgetMs; // 单轮（不含 wait 与 yield）超过此值计 overrun
    uint32_t yieldTicks = 1;                     // 每轮末 vTaskDelay；0 = 不让出（不建议）
    bool watchdog = true;                        // 任务启动后 espTaskWdtAdd 自己，每轮 reset
    /**
     * 可选等待钩子（执行器任务中调用）：最长阻塞 maxMs，可以顺带处理它等到的事件（例如 BT 事件）。
     * 为 nullptr 时执行器阻塞在命令队列上（命令到达立即唤醒）。
     */
    void (*wait)(void *ctx, uint32_t maxMs) noexcept = nullptr;
    void *waitCtx = nullptr;
};

/** 执行器运行统计（临界区内累加），由 stats() 拷贝出快照。 */
struct ExecStats {
    uint32_t rounds = 0;
    uint32_t overruns = 0;
    uint32_t maxRoundMs = 0;
    uint32_t commands = 0;    // 已分派的命令数
    uint32_t resultDrops = 0; // 结果队列满时丢弃的最旧结果数
    uint32_t stackFree = 0;   // 栈高水位（字节），每轮更新
};

/** 执行器所需的全部静态存储；由调用方定义为 constinit 全局并交给 Executor::init。 */
template <size_t StackBytes, size_t QueueDepth> struct ExecutorStorage {
    static_assert((StackBytes % sizeof(StackType_t)) == 0U, "stack must be word-aligned");
    static_assert(QueueDepth > 0U, "queue depth must be > 0");
    std::array<StackType_t, StackBytes / sizeof(StackType_t)> stack{};
    StaticTask_t tcb{};
    std::array<uint8_t, QueueDepth * sizeof(fw::Command)> cmdArea{};
    StaticQueue_t cmdQueue{};
    std::array<uint8_t, QueueDepth * sizeof(fw::CommandResult)> resultArea{};
    StaticQueue_t resultQueue{};
};

/** ExecutorStorage 的类型擦除视图（避免 Executor 本身成为模板）。 */
struct ExecutorStorageView {
    std::span<StackType_t> stack;
    StaticTask_t *tcb = nullptr;
    std::span<uint8_t> cmdArea;
    StaticQueue_t *cmdQueue = nullptr;
    std::span<uint8_t> resultArea;
    StaticQueue_t *resultQueue = nullptr;
    size_t queueDepth = 0;
};

/**
 * @brief  把具体尺寸的 ExecutorStorage 类型擦除成 ExecutorStorageView
 * @param  st: 调用方定义的 constinit 静态存储
 * @retval 指向 st 各字段的视图（不复制、不分配）
 */
template <size_t S, size_t D>
[[nodiscard]] ExecutorStorageView viewOf(ExecutorStorage<S, D> &st) noexcept
{
    return ExecutorStorageView{st.stack,      &st.tcb,         st.cmdArea, &st.cmdQueue,
                               st.resultArea, &st.resultQueue, D};
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
     * @brief  保存配置、创建命令/结果队列，并对本执行器的模块调用 ops->init
     * @param  cfg: 任务名、优先级、核、等待/预算/让出等配置
     * @param  modules: 模块表（执行器保存该 span，必须指向静态存储）
     * @param  storage: 队列、栈与 TCB 的静态存储
     * @retval fw::Status::kOk 初始化完成（个别模块 init 失败只标记为不可用，不影响返回值）
     * @retval fw::Status::kInvalidArg name 为空、core/priority 越界、存储尺寸不足、模块数超
     *         kExecMaxModules、本执行器内 ModuleId 重复或 ops 为空
     * @retval fw::Status::kBusy 重复调用
     * @retval fw::Status::kNoSpace 队列创建失败
     * @note   只在启动期单线程（app_main）调用；不阻塞、不分配堆；模块 init 失败的日志为 E 级
     */
    [[nodiscard]] fw::Status init(const ExecutorConfig &cfg, std::span<const fw::Module> modules,
                                  const ExecutorStorageView &storage) noexcept;

    /**
     * @brief  创建执行器任务；任务先对可用模块调用 ops->start，再进入轮询循环
     * @retval fw::Status::kOk 任务已创建
     * @retval fw::Status::kInvalidState 尚未 init
     * @retval fw::Status::kBusy 已启动
     * @retval fw::Status::kNoSpace 任务创建失败
     * @note   启动期调用；绑核由 cfg.core 指定；模块 start 失败只标记为不可用
     */
    [[nodiscard]] fw::Status start() noexcept;

    /**
     * @brief  经 CommandBus 同步调用目标模块的 onCommand 并等待结果
     * @param  cmd: 待执行命令（target 指出目标模块）
     * @param[out] result: 成功时写入执行器结果，seq 为本次分配值；失败时内容不变
     * @param  timeoutMs: 等待结果的毫秒数
     * @retval fw::Result::kOk result 有效，result.status 为模块结果
     * @retval fw::Result::kTimeout 等待超时，命令可能稍后仍被执行
     * @retval fw::Result::kBusy 命令队列满
     * @retval fw::Result::kInvalidState 未运行，或从本执行器任务自身调用（会死锁）
     * @retval fw::Result::kNotFound 目标模块不存在
     * @note   任务上下文，timeoutMs>0 会阻塞；同一时刻只允许一个调用者任务（当前为控制台）
     */
    [[nodiscard]] fw::Result call(const fw::Command &cmd, fw::CommandResult &result,
                                  uint32_t timeoutMs) noexcept;

    /**
     * @brief  执行器任务是否已创建
     * @retval true 任务已创建；false 否则
     * @note   任意任务可调用（内部临界区）
     */
    [[nodiscard]] bool running() const noexcept;

    /**
     * @brief  取运行统计快照
     * @retval 统计副本（临界区内拷贝）
     * @note   任意任务可调用
     */
    [[nodiscard]] ExecStats stats() const noexcept;

    /**
     * @brief  模块是否可用（init/start 均成功）
     * @param  id: 模块编号
     * @retval true 可用；false 模块不属于本执行器或 init/start 失败
     * @note   任意任务可调用
     */
    [[nodiscard]] bool moduleOk(fw::ModuleId id) const noexcept;

private:
    static void taskEntry(void *arg) noexcept;
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
