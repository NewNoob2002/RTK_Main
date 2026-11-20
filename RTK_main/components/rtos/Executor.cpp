// Executor 实现：静态任务主循环（等待→命令分派→按周期 poll→统计→喂狗→让出）与异步完成表。
// run/processCommand/complete 只在执行器自己的任务中运行；post/poll/cancel/call 任意任务可调；不分配堆、非 ISR。
#include "rtos/Executor.hpp"

#include <limits>
#include <string_view>

#include "fw/Log.hpp"
#include "esp_task_wdt.h"
#include "rtos/CriticalSection.hpp"
#include "rtos/EspStatus.hpp"
#include "rtos/ExecTrace.hpp"

namespace rtos {
namespace {

/**
 * @brief  把毫秒换成 tick，并保证非零毫秒至少 1 tick
 * @param  ms: 等待毫秒数
 * @retval ms 为 0 返回 0；否则大于等于 1 的 tick 数
 */
TickType_t waitTicks(uint32_t ms) noexcept
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return ms != 0U && ticks == 0U ? 1U : ticks;
}

/**
 * @brief  在本执行器的模块表里找命令目标并调用其 onCommand
 * @param  modules: 本执行器的模块表
 * @param  executor: 执行器编号，用于过滤模块
 * @param  available: 各模块可用标志，下标与 modules 对齐
 * @param  command: 待分派命令
 * @retval 结果对象：目标不存在为 kNotFound，模块不可用为 kInvalidState，无 onCommand 为
 *         kNotSupported，否则为模块返回值；seq 恒为命令 seq
 * @note   只在执行器任务中调用；不阻塞、不分配
 */
fw::CommandResult dispatchCommand(std::span<const fw::Module> modules, fw::ExecutorId executor,
                                  std::span<const bool> available,
                                  const fw::Command &command) noexcept
{
    fw::CommandResult result{command.seq, fw::Result::kNotFound, 0};
    for (size_t i = 0; i < modules.size(); ++i) {
        const auto &module = modules[i];
        if (module.executor != executor || module.id != command.target) {
            continue;
        }
        result.status = !available[i]                      ? fw::Result::kInvalidState
                        : module.ops->onCommand == nullptr ? fw::Result::kNotSupported
                                                           : fw::Result::kOk;
        if (result.status == fw::Result::kOk) {
            module.ops->onCommand(module.self, command, result);
        }
        break;
    }
    // seq 是执行器契约，模块不得覆盖它。
    result.seq = command.seq;
    return result;
}

/**
 * @brief  校验 init 的标量参数与静态存储尺寸
 * @param  cfg: 执行器配置
 * @param  storage: 队列、完成表、栈与 TCB 的静态存储
 * @retval 满足全部约束返回 true；否则 false（调用方返回 kInvalidArg）
 * @note   只读参数，无副作用；由 Executor::init 在创建队列前调用一次。
 */
bool initArgsValid(const ExecutorConfig &cfg, const ExecutorStorageView &storage) noexcept
{
    if (cfg.name == nullptr || std::string_view{cfg.name}.empty() || cfg.core < 0 ||
        cfg.core >= configNUMBER_OF_CORES || cfg.priority >= configMAX_PRIORITIES) {
        return false;
    }
    if (storage.stack.empty() ||
        storage.stack.size_bytes() > std::numeric_limits<uint32_t>::max() ||
        storage.tcb == nullptr || storage.cmdQueue == nullptr || storage.queueDepth == 0U ||
        storage.queueDepth > std::numeric_limits<UBaseType_t>::max() ||
        storage.pending.size() != kMaxPending) {
        return false;
    }
    // 队列深度不能超过调用方给出的静态队列区容量。
    return storage.queueDepth <= storage.cmdArea.size() / sizeof(fw::Command);
}

/**
 * @brief  校验模块表中属于本执行器的条目：数量上限、ops 非空、ModuleId 不重复
 * @param  modules: 本执行器轮询的模块表
 * @param  executor: 执行器编号
 * @retval 满足约束返回 true
 * @note   只读参数；只在启动期调用一次，O(n²) 可接受。
 */
bool moduleTableValid(std::span<const fw::Module> modules, fw::ExecutorId executor) noexcept
{
    if (modules.size() > kExecMaxModules) {
        return false;
    }
    for (size_t i = 0; i < modules.size(); ++i) {
        if (modules[i].executor != executor) {
            continue;
        }
        if (modules[i].ops == nullptr) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (modules[j].executor == executor && modules[j].id == modules[i].id) {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief  判断目标模块是否注册在本执行器上
 * @param  modules: 本执行器保存的模块表
 * @param  executor: 本执行器编号
 * @param  target: 目标模块编号
 * @retval true 找到匹配模块；false 未注册
 * @note   只读 init 后不变的元数据；任意任务可调用
 */
bool hasTargetModule(std::span<const fw::Module> modules, fw::ExecutorId executor,
                     fw::ModuleId target) noexcept
{
    for (const auto &module : modules) {
        if (module.executor == executor && module.id == target) {
            return true;
        }
    }
    return false;
}

} // namespace

/**
 * @brief  创建命令队列并清空完成表后逐个调用本执行器模块的 ops->init（公开契约见头文件声明）
 * @param  cfg: 任务名、优先级、核、等待/预算/让出等配置
 * @param  modules: 模块表，必须指向静态存储（执行器保存该 span）
 * @param  storage: 队列、完成表、栈与 TCB 的静态存储
 * @retval fw::Status::kOk 校验与队列创建完成；个别模块 init 失败只标记为不可用
 * @retval fw::Status::kInvalidArg 参数非法（name 为空、尺寸不足、模块数超限、ModuleId 重复、ops 为空）
 * @retval fw::Status::kBusy 重复调用
 * @retval fw::Status::kNoSpace 队列创建失败
 * @note   块保留在定义处是因为本函数与 StaticQueue/StaticStreamBuffer 的 init 同名，工具按短名匹配会误取它们头文件里的注释。
 */
fw::Status Executor::init(const ExecutorConfig &cfg, std::span<const fw::Module> modules,
                          const ExecutorStorageView &storage) noexcept
{
    if (inited_) {
        return fw::Status::kBusy;
    }
    if (!initArgsValid(cfg, storage) || !moduleTableValid(modules, cfg.id)) {
        return fw::Status::kInvalidArg;
    }
    // 长度已限制在 UBaseType_t 范围内；定长命令的大小远小于该上限。
    cmd_q_ = xQueueCreateStatic(static_cast<UBaseType_t>(storage.queueDepth), sizeof(fw::Command),
                                storage.cmdArea.data(), storage.cmdQueue);
    if (cmd_q_ == nullptr) {
        return fw::Status::kNoSpace;
    }
    for (auto &slot : storage.pending) {
        slot = PendingSlot{};
    }
    cfg_ = cfg;
    modules_ = modules;
    storage_ = storage;
    for (size_t i = 0; i < modules_.size(); ++i) {
        const auto &module = modules_[i];
        if (module.executor != cfg_.id) {
            continue;
        }
        const fw::Result status =
            module.ops->init == nullptr ? fw::Result::kOk : module.ops->init(module.self);
        ok_[i] = fw::isOk(status);
        if (!ok_[i]) {
            // Result 的底层类型为 uint8_t，扩展为 unsigned 无损。
            FW_LOGE(fw::TraceCat::kExec, "module init failed: id=%u status=%u", module.id,
                    static_cast<unsigned>(status));
        }
    }
    inited_ = true;
    return fw::Status::kOk;
}

fw::Status Executor::start() noexcept
{
    if (!inited_) {
        return fw::Status::kInvalidState;
    }
    if (running()) {
        return fw::Status::kBusy;
    }
    // 显式绑核由配置指定；产品链路放 core1，避免与 core0 的 BT worker 争用。
    // init 已验证栈字节数可用 uint32_t 表示，优先级小于 configMAX_PRIORITIES。
    const TaskHandle_t task = xTaskCreateStaticPinnedToCore(
        &Executor::taskEntry, cfg_.name, static_cast<uint32_t>(storage_.stack.size_bytes()), this,
        static_cast<UBaseType_t>(cfg_.priority), storage_.stack.data(), storage_.tcb, cfg_.core);
    if (task == nullptr) {
        return fw::Status::kNoSpace;
    }
    {
        const CriticalSection guard(stats_mux_);
        task_ = task;
    }
    return fw::Status::kOk;
}

bool Executor::running() const noexcept
{
    const CriticalSection guard(stats_mux_);
    return task_ != nullptr;
}

ExecStats Executor::stats() const noexcept
{
    const CriticalSection guard(stats_mux_);
    return stats_;
}

bool Executor::moduleOk(fw::ModuleId id) const noexcept
{
    const CriticalSection guard(stats_mux_);
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (modules_[i].executor == cfg_.id && modules_[i].id == id) {
            return ok_[i];
        }
    }
    return false;
}

/**
 * @brief  在持有 stats_mux_ 临界区时按 seq 查找非空闲完成表项
 * @param  seq: 目标命令序号
 * @retval 找到的槽位指针；未找到返回 nullptr
 * @note   调用方必须已持有 stats_mux_；不阻塞、不调用 FreeRTOS API
 */
PendingSlot *Executor::findSlotLocked(uint32_t seq) noexcept
{
    if (seq == 0U) {
        return nullptr;
    }
    for (auto &slot : storage_.pending) {
        if (slot.state != PendingState::kFree && slot.seq == seq) {
            return &slot;
        }
    }
    return nullptr;
}

/**
 * @brief  在持有 stats_mux_ 临界区时分配单调递增且跳过 0 与在途冲突的新序号
 * @retval 非零且不与当前完成表项冲突的命令序号
 * @note   调用方必须已持有 stats_mux_；至多循环 kMaxPending + 1 次
 */
uint32_t Executor::allocSeqLocked() noexcept
{
    // 完成表最多 kMaxPending 项，最多探测 kMaxPending + 1 次必能找到不冲突的非零序号。
    for (size_t attempt = 0; attempt <= kMaxPending; ++attempt) {
        if (next_seq_ == 0U) {
            next_seq_ = 1U;
        }
        const uint32_t candidate = next_seq_;
        ++next_seq_;
        if (next_seq_ == 0U) {
            next_seq_ = 1U;
        }
        if (findSlotLocked(candidate) == nullptr) {
            return candidate;
        }
    }
    return 1U;
}

/**
 * @brief  占完成表槽位并入队命令的公共实现（供 post 与 call 复用）
 * @param  cmd: 待执行命令
 * @param  caller: 调用方身份
 * @param  waiter: 同步等待任务句柄（异步 post 为 nullptr）
 * @param  dropTraceArg: 满队/满表时的 trace 负载 b（post 传 0，call 传 timeoutMs）
 * @param[out] out: 成功时写入凭据；失败时重置为无效凭据
 * @retval fw::Result::kOk 成功入队
 * @retval fw::Result::kInvalidState 执行器未运行
 * @retval fw::Result::kNotFound 目标模块不存在
 * @retval fw::Result::kBusy 完成表满或命令队列满
 * @note   任意任务可调用；不阻塞，临界区内不调用 FreeRTOS API
 */
fw::Result Executor::postInternal(const fw::Command &cmd, Caller caller, TaskHandle_t waiter,
                                  uint32_t dropTraceArg, Ticket &out) noexcept
{
    out = Ticket{};
    if (!running()) {
        return fw::Result::kInvalidState;
    }
    if (!hasTargetModule(modules_, cfg_.id, cmd.target)) {
        return fw::Result::kNotFound;
    }
    size_t slotIdx = kMaxPending;
    uint32_t seq = 0U;
    {
        const CriticalSection guard(stats_mux_);
        for (size_t i = 0; i < storage_.pending.size(); ++i) {
            if (storage_.pending[i].state == PendingState::kFree) {
                slotIdx = i;
                seq = allocSeqLocked();
                auto &slot = storage_.pending[i];
                slot.seq = seq;
                slot.result = fw::CommandResult{seq, fw::Result::kOk, 0};
                slot.waiterTask = waiter;
                slot.caller = caller;
                slot.state = PendingState::kQueued;
                break;
            }
        }
    }
    if (slotIdx == kMaxPending) {
        traceExec(ExecTrace::kCallDrop, static_cast<uint32_t>(cmd.target), dropTraceArg);
        return fw::Result::kBusy;
    }
    fw::Command queued = cmd;
    queued.seq = seq;
    if (xQueueSend(cmd_q_, &queued, 0) != pdTRUE) {
        {
            const CriticalSection guard(stats_mux_);
            auto &slot = storage_.pending[slotIdx];
            if (slot.seq == seq && slot.state == PendingState::kQueued) {
                slot = PendingSlot{};
            }
        }
        traceExec(ExecTrace::kCallDrop, static_cast<uint32_t>(cmd.target), dropTraceArg);
        return fw::Result::kBusy;
    }
    out.seq = seq;
    // slotIdx < kMaxPending < UINT8_MAX，窄化到 uint8_t 安全。
    out.slot = static_cast<uint8_t>(slotIdx);
    return fw::Result::kOk;
}

fw::Result Executor::post(const fw::Command &cmd, Caller caller, Ticket &out) noexcept
{
    return postInternal(cmd, caller, nullptr, 0U, out);
}

PollState Executor::poll(const Ticket &t, fw::CommandResult &out) noexcept
{
    if (!inited_ || t.seq == 0U || t.slot >= storage_.pending.size()) {
        return PollState::kInvalid;
    }
    const CriticalSection guard(stats_mux_);
    auto &slot = storage_.pending[t.slot];
    if (slot.seq != t.seq) {
        return PollState::kInvalid;
    }
    switch (slot.state) {
    case PendingState::kQueued:
    case PendingState::kRunning:
        return PollState::kPending;
    case PendingState::kDone:
        out = slot.result;
        slot = PendingSlot{};
        return PollState::kDone;
    case PendingState::kCancelled:
        return PollState::kCancelled;
    case PendingState::kFree:
    default:
        return PollState::kInvalid;
    }
}

void Executor::cancel(const Ticket &t) noexcept
{
    if (!inited_ || t.seq == 0U || t.slot >= storage_.pending.size()) {
        return;
    }
    const CriticalSection guard(stats_mux_);
    auto &slot = storage_.pending[t.slot];
    if (slot.seq != t.seq) {
        return;
    }
    switch (slot.state) {
    case PendingState::kQueued:
    case PendingState::kRunning:
        slot.state = PendingState::kCancelled;
        slot.waiterTask = nullptr;
        break;
    case PendingState::kDone:
        // 结果已就绪但调用方放弃取走，直接释放槽位避免泄漏。
        slot = PendingSlot{};
        break;
    case PendingState::kCancelled:
    case PendingState::kFree:
    default:
        break;
    }
}

/**
 * @brief  写入命令最终结果并按需唤醒同步等待任务，或在已取消时丢弃并释放槽位
 * @param  seq: 命令序号
 * @param  result: 命令执行结果
 * @note   只在执行器任务内调用；临界区外执行 traceExec 与 xTaskNotifyGive
 */
void Executor::finishCommand(uint32_t seq, const fw::CommandResult &result) noexcept
{
    TaskHandle_t waiter = nullptr;
    bool dropped = false;
    {
        const CriticalSection guard(stats_mux_);
        PendingSlot *const slot = findSlotLocked(seq);
        if (slot != nullptr && slot->state == PendingState::kCancelled) {
            *slot = PendingSlot{};
        } else if (slot != nullptr && slot->state == PendingState::kRunning) {
            slot->result = result;
            // seq 是执行器契约，模块不得覆盖它。
            slot->result.seq = seq;
            slot->state = PendingState::kDone;
            waiter = slot->waiterTask;
            slot->waiterTask = nullptr;
        } else {
            dropped = true;
            ++stats_.resultDrops;
        }
    }
    if (dropped) {
        traceExec(ExecTrace::kResultDrop, seq, 1U);
    }
    if (waiter != nullptr) {
        xTaskNotifyGive(waiter);
    }
}

void Executor::complete(uint32_t seq, const fw::CommandResult &r) noexcept
{
    TaskHandle_t owner = nullptr;
    {
        const CriticalSection guard(stats_mux_);
        owner = task_;
    }
    if (seq == 0U || owner == nullptr || owner != xTaskGetCurrentTaskHandle()) {
        return;
    }
    finishCommand(seq, r);
}

fw::Result Executor::call(const fw::Command &cmd, fw::CommandResult &result,
                          uint32_t timeoutMs) noexcept
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    TaskHandle_t owner = nullptr;
    {
        const CriticalSection guard(stats_mux_);
        owner = task_;
    }
    if (owner == nullptr || owner == current) {
        return fw::Result::kInvalidState;
    }
    Ticket ticket{};
    const fw::Result posted = postInternal(cmd, Caller::kConsole, current, timeoutMs, ticket);
    if (!fw::isOk(posted)) {
        return posted;
    }
    const uint32_t begin = nowMs();
    for (;;) {
        const PollState state = poll(ticket, result);
        if (state == PollState::kDone) {
            return fw::Result::kOk;
        }
        if (state != PollState::kPending) {
            return fw::Result::kTimeout;
        }
        const uint32_t elapsed = nowMs() - begin;
        if (elapsed >= timeoutMs) {
            cancel(ticket);
            return fw::Result::kTimeout;
        }
        // 阻塞等待执行器在 finishCommand 中 xTaskNotifyGive；允许旧通知提前返回并在下一轮复查 poll()。
        static_cast<void>(ulTaskNotifyTake(pdTRUE, waitTicks(timeoutMs - elapsed)));
    }
}

/**
 * @brief  处理从命令队列取出的一条命令：已取消则跳过并释放，否则分派并在非延迟时立即完成
 * @param  command: 从 cmd_q_ 取出的命令
 * @param[out] commands: 已分派命令计数器，实际分派时加一
 * @note   只在执行器任务内调用；不持有临界区时调用模块 onCommand
 */
void Executor::processCommand(const fw::Command &command, uint32_t &commands) noexcept
{
    bool skipped = false;
    {
        const CriticalSection guard(stats_mux_);
        PendingSlot *const slot = findSlotLocked(command.seq);
        if (slot != nullptr) {
            if (slot->state == PendingState::kCancelled) {
                *slot = PendingSlot{};
                skipped = true;
            } else if (slot->state == PendingState::kQueued) {
                slot->state = PendingState::kRunning;
            }
        }
    }
    if (skipped) {
        return;
    }
    // available 只由本任务在 start 阶段修改；运行中为只读，不存在并发写者。
    const auto result = dispatchCommand(modules_, cfg_.id, ok_, command);
    ++commands;
    if (result.detail != fw::kDetailDeferred) {
        finishCommand(command.seq, result);
    }
}

/**
 * @brief  FreeRTOS 任务入口：等启动放行后进入主循环
 * @param  arg: 指向本 Executor 的指针
 */
void Executor::taskEntry(void *arg) noexcept
{
    // RTOS 原样返回 start 传入的 Executor 指针，类型恢复安全。
    auto &self = *static_cast<Executor *>(arg);
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    {
        const CriticalSection guard(self.stats_mux_);
        self.task_ = current;
    }
    for (size_t i = 0; i < self.modules_.size(); ++i) {
        const auto &module = self.modules_[i];
        if (module.executor != self.cfg_.id || !self.moduleOk(module.id) ||
            module.ops->start == nullptr) {
            continue;
        }
        const fw::Result status = module.ops->start(module.self);
        if (!fw::isOk(status)) {
            {
                const CriticalSection guard(self.stats_mux_);
                self.ok_[i] = false;
            }
            // Result 为 uint8_t 枚举，转换为 unsigned 无损。
            FW_LOGE(fw::TraceCat::kExec, "module start failed: id=%u status=%u", module.id,
                    static_cast<unsigned>(status));
        }
    }
    self.run();
}

// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
/**
 * @brief  执行器主循环：等命令、轮询模块、喂看门狗，永不返回
 * @note   不拆（ADR-0013 §2.4 的保留例外）：每轮实时路径，等待→排空命令→轮询模块→统计→喂狗的顺序与 pending 标志构成单一状态机。
 */
void Executor::run() noexcept
{
    bool watchdog = false;
    if (cfg_.watchdog) {
        const esp_err_t error = esp_task_wdt_add(nullptr);
        watchdog = error == ESP_OK;
        if (!watchdog) {
            FW_LOGW(fw::TraceCat::kExec, "task wdt subscribe failed: %s", esp_err_to_name(error));
        }
    }
    bool pending = false;
    for (;;) {
        fw::Command command{};
        bool received = false;
        const uint32_t maxWait = pending ? 0U : cfg_.idleWaitMs;
        if (cfg_.wait != nullptr) {
            cfg_.wait(cfg_.waitCtx, maxWait);
        } else {
            received = xQueueReceive(cmd_q_, &command, waitTicks(maxWait)) == pdTRUE;
        }
        const uint32_t begin = nowMs();
        uint32_t commands = 0;
        while (received || xQueueReceive(cmd_q_, &command, 0) == pdTRUE) {
            received = false;
            processCommand(command, commands);
        }
        pending = false;
        for (size_t i = 0; i < modules_.size(); ++i) {
            const auto &module = modules_[i];
            if (module.executor != cfg_.id || !moduleOk(module.id) || module.ops->poll == nullptr) {
                continue;
            }
            const uint32_t now = nowMs();
            if (module.pollPeriodMs == 0U || (now - last_poll_ms_[i]) >= module.pollPeriodMs) {
                last_poll_ms_[i] = now;
                pending = module.ops->poll(module.self, now) || pending;
            }
        }
        const uint32_t duration = nowMs() - begin;
        // 超轮预算即记事件：此处是本任务上下文，写环不阻塞；格式化留给 DebugModule。
        if (duration > cfg_.roundBudgetMs) {
            traceExec(ExecTrace::kRoundOverrun, duration, cfg_.roundBudgetMs);
        }
        // IDF 高水位返回字节；本工程 ESP32 StackType_t 为 uint8_t，结果在 u32 范围内。
        const uint32_t stackFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        {
            const CriticalSection guard(stats_mux_);
            ++stats_.rounds;
            stats_.overruns += duration > cfg_.roundBudgetMs ? 1U : 0U;
            if (duration > stats_.maxRoundMs) {
                stats_.maxRoundMs = duration;
            }
            stats_.commands += commands;
            stats_.stackFree = stackFree;
        }
        if (watchdog) {
            const esp_err_t error = esp_task_wdt_reset();
            if (error != ESP_OK) {
                traceExec(ExecTrace::kWdtResetFail, static_cast<uint32_t>(error), 0U);
                watchdog = false;
            }
        }
        vTaskDelay(cfg_.yieldTicks);
    }
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)
} // namespace rtos
