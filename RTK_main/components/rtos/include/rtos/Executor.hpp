// 执行器 = 静态 FreeRTOS 任务 + 模块表 + 异步命令总线；每轮 等待→分派命令→周期 poll→统计→喂狗→让出。
// 模块回调（init 除外）只在执行器任务中调用；post/poll/cancel/call 任意任务可调用；complete 仅本执行器任务调用。
// 不分配堆、不在 ISR 中调用（ADR-0007 §3.2/§3.5、ADR-0018 §2.2）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "fw/Command.hpp"
#include "fw/Module.hpp"
#include "fw/Result.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fw/Status.hpp"

namespace fw {

/** onCommand 把 result.detail 置为此值表示命令已受理，稍后由模块在本执行器任务内调 complete() 结束。 */
inline constexpr int32_t kDetailDeferred = std::numeric_limits<int32_t>::min();

} // namespace fw

namespace rtos {

inline constexpr size_t kExecMaxModules = 8;
inline constexpr size_t kMaxPending = 8;
inline constexpr uint8_t kInvalidSlot = std::numeric_limits<uint8_t>::max();
inline constexpr uint32_t kExecIdleWaitMs = 20;
inline constexpr uint32_t kExecRoundBudgetMs = 20;

static_assert(kMaxPending < kInvalidSlot, "kMaxPending must fit uint8_t slot index");

/** 命令调用方身份（ADR-0018 §2.2；权限检查在设置服务层完成，执行器仅记录来源）。 */
enum class Caller : uint8_t {
    kConsole = 0,
    kWeb,
    kPhone,
    kInternal,
    kMqtt, // 预留：仅电子围栏键（ADR-0018 §6 Q2）
};

/** 异步命令凭据；seq == 0 或 slot == kInvalidSlot 表示无效凭据。 */
struct Ticket {
    uint32_t seq = 0;
    uint8_t slot = kInvalidSlot;
};

/** 异步命令轮询结果。 */
enum class PollState : uint8_t {
    kPending = 0, // 已入队、执行中或延迟等待 complete()
    kDone,        // 已完成，本次 poll 已写入 out 并释放槽位
    kCancelled,   // 已取消，尚未被执行器跳过或丢弃释放
    kInvalid,     // 凭据无效、已取走释放或槽位已复用给其他 seq
};

/** 完成表槽位内部状态（kFree = 0，保证静态存储全零初值落入 .bss）。 */
enum class PendingState : uint8_t {
    kFree = 0,
    kQueued,
    kRunning,
    kDone,
    kCancelled,
};

/** 异步命令完成表项；全零初值落入 .bss，由执行器内部临界区保护。 */
struct PendingSlot {
    uint32_t seq = 0;
    fw::CommandResult result{};
    TaskHandle_t waiterTask = nullptr;
    Caller caller = Caller::kConsole;
    PendingState state = PendingState::kFree;
};

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
    uint32_t resultDrops = 0; // 未匹配到在途槽位而丢弃的结果数
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
    std::array<PendingSlot, kMaxPending> pending{};
};

/** ExecutorStorage 的类型擦除视图（避免 Executor 本身成为模板）。 */
struct ExecutorStorageView {
    std::span<StackType_t> stack;
    StaticTask_t *tcb = nullptr;
    std::span<uint8_t> cmdArea;
    StaticQueue_t *cmdQueue = nullptr;
    std::span<PendingSlot> pending;
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
    return ExecutorStorageView{st.stack, &st.tcb, st.cmdArea, &st.cmdQueue, st.pending, D};
}

/** 静态任务 + 模块表 + 异步命令总线的执行器；init 后由本任务独占运行，外部经 post/poll/cancel/call/stats 交互。 */
class Executor {
public:
    constexpr Executor() noexcept = default;
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    Executor(Executor &&) = delete;
    Executor &operator=(Executor &&) = delete;
    ~Executor() = default;

    /**
     * @brief  保存配置、创建命令队列并清空完成表，再对本执行器的模块调用 ops->init
     * @param  cfg: 任务名、优先级、核、等待/预算/让出等配置
     * @param  modules: 模块表（执行器保存该 span，必须指向静态存储）
     * @param  storage: 队列、完成表、栈与 TCB 的静态存储
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
     * @brief  非阻塞投递命令到本执行器并分配完成表凭据
     * @param  cmd: 待执行命令（target 指出目标模块，seq 由执行器覆盖分配）
     * @param  caller: 调用方身份
     * @param[out] out: 成功时写入有效凭据；失败时重置为无效凭据（seq=0, slot=kInvalidSlot）
     * @retval fw::Result::kOk 已占槽入队，可随后 poll() 或 cancel()
     * @retval fw::Result::kBusy 完成表满或命令队列满
     * @retval fw::Result::kInvalidState 执行器未运行
     * @retval fw::Result::kNotFound 目标模块不属于本执行器
     * @note   任意任务可调用（含本执行器自身任务）；不阻塞、不分配堆，非 ISR 安全
     */
    [[nodiscard]] fw::Result post(const fw::Command &cmd, Caller caller, Ticket &out) noexcept;

    /**
     * @brief  非阻塞查询异步命令状态；完成时取出结果并释放完成表槽位
     * @param  t: post() 返回的凭据
     * @param[out] out: 仅返回 kDone 时写入命令结果；其余返回值不修改 out
     * @retval PollState::kPending 仍在排队、执行中或延迟等待 complete()
     * @retval PollState::kDone 已完成，out 有效且对应槽位已释放
     * @retval PollState::kCancelled 已取消，等待执行器跳过或丢弃结果后释放
     * @retval PollState::kInvalid 凭据无效、已取走释放或槽位已复用给其他 seq
     * @note   任意任务可调用；不阻塞、不分配堆，非 ISR 安全
     */
    [[nodiscard]] PollState poll(const Ticket &t, fw::CommandResult &out) noexcept;

    /**
     * @brief  取消已投递的异步命令
     * @param  t: post() 返回的凭据（无效或已释放时静默忽略）
     * @note   任意任务可调用；不阻塞、不分配堆，非 ISR 安全。
     *         未出队时标记取消，执行器取出时跳过 onCommand 并释放槽位；
     *         执行中/延迟中标记取消，结果到达时丢弃并释放槽位；已完成则立即释放槽位。
     */
    void cancel(const Ticket &t) noexcept;

    /**
     * @brief  异步完成一条延迟受理（detail == fw::kDetailDeferred）的命令
     * @param  seq: 原命令序号（cmd.seq）
     * @param  r: 命令结果（执行器强制回填 r.seq = seq）
     * @note   仅本执行器任务内调用；不阻塞、不分配堆，非 ISR 安全。
     *         若命令已取消则丢弃结果并释放槽位；若 seq 无匹配在途槽位则计入 resultDrops。
     */
    void complete(uint32_t seq, const fw::CommandResult &r) noexcept;

    /**
     * @brief  经异步总线同步调用目标模块的 onCommand 并通过任务通知等待结果
     * @param  cmd: 待执行命令（target 指出目标模块）
     * @param[out] result: 成功时写入执行器结果，seq 为本次分配值；失败时内容不变
     * @param  timeoutMs: 等待结果的毫秒数
     * @retval fw::Result::kOk result 有效，result.status 为模块结果
     * @retval fw::Result::kTimeout 等待超时并已调用 cancel()；未出队命令将被跳过，已执行中结果将丢弃
     * @retval fw::Result::kBusy 完成表满或命令队列满
     * @retval fw::Result::kInvalidState 未运行，或从本执行器任务自身调用（会死锁）
     * @retval fw::Result::kNotFound 目标模块不属于本执行器
     * @note   仅可阻塞的任务上下文（如控制台 REPL）调用；允许多个非本执行器任务并发调用，非 ISR 安全
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
    friend struct ExecutorTestAccess;

    static void taskEntry(void *arg) noexcept;
    void run() noexcept;
    void processCommand(const fw::Command &command, uint32_t &commands) noexcept;
    [[nodiscard]] fw::Result postInternal(const fw::Command &cmd, Caller caller,
                                          TaskHandle_t waiter, uint32_t dropTraceArg,
                                          Ticket &out) noexcept;
    void finishCommand(uint32_t seq, const fw::CommandResult &result) noexcept;
    [[nodiscard]] PendingSlot *findSlotLocked(uint32_t seq) noexcept;
    [[nodiscard]] uint32_t allocSeqLocked() noexcept;

    ExecutorConfig cfg_{};
    std::span<const fw::Module> modules_{};
    std::array<bool, kExecMaxModules> ok_{};
    std::array<uint32_t, kExecMaxModules> last_poll_ms_{};
    ExecutorStorageView storage_{};
    QueueHandle_t cmd_q_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint32_t next_seq_ = 1;
    ExecStats stats_{};
    mutable portMUX_TYPE stats_mux_ = portMUX_INITIALIZER_UNLOCKED;
    bool inited_ = false;
};

} // namespace rtos
