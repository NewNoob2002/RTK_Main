// Executor 实现：静态任务主循环（等待→命令分派→按周期 poll→统计→喂狗→让出），线程模型见 Executor.hpp。
// 只在执行器自己的任务中运行；不分配堆、不在 ISR 中调用。
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
 * @param  storage: 队列、栈与 TCB 的静态存储
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
        storage.tcb == nullptr || storage.cmdQueue == nullptr || storage.resultQueue == nullptr ||
        storage.cmdQueue == storage.resultQueue || storage.queueDepth == 0U ||
        storage.queueDepth > std::numeric_limits<UBaseType_t>::max()) {
        return false;
    }
    // 队列深度不能超过调用方给出的静态队列区容量。
    return storage.queueDepth <= storage.cmdArea.size() / sizeof(fw::Command) &&
           storage.queueDepth <= storage.resultArea.size() / sizeof(fw::CommandResult);
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
} // namespace

/**
 * @brief  创建命令/结果队列后逐个调用本执行器模块的 ops->init（公开契约见头文件声明）
 * @param  cfg: 任务名、优先级、核、等待/预算/让出等配置
 * @param  modules: 模块表，必须指向静态存储（执行器保存该 span）
 * @param  storage: 队列、栈与 TCB 的静态存储
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
    // 长度已限制在 UBaseType_t 范围内；定长命令/结果的大小远小于该上限。
    cmd_q_ = xQueueCreateStatic(static_cast<UBaseType_t>(storage.queueDepth), sizeof(fw::Command),
                                storage.cmdArea.data(), storage.cmdQueue);
    if (cmd_q_ == nullptr) {
        return fw::Status::kNoSpace;
    }
    // 同上，队列深度已经过范围检查。
    result_q_ =
        xQueueCreateStatic(static_cast<UBaseType_t>(storage.queueDepth), sizeof(fw::CommandResult),
                           storage.resultArea.data(), storage.resultQueue);
    if (result_q_ == nullptr) {
        vQueueDelete(cmd_q_);
        cmd_q_ = nullptr;
        return fw::Status::kNoSpace;
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

fw::Result Executor::call(const fw::Command &cmd, fw::CommandResult &result,
                          uint32_t timeoutMs) noexcept
{
    TaskHandle_t owner = nullptr;
    {
        const CriticalSection guard(stats_mux_);
        owner = task_;
    }
    if (owner == nullptr || owner == xTaskGetCurrentTaskHandle()) {
        return fw::Result::kInvalidState;
    }
    bool found = false;
    for (const auto &module : modules_) {
        found = found || (module.executor == cfg_.id && module.id == cmd.target);
    }
    if (!found) {
        return fw::Result::kNotFound;
    }
    fw::CommandResult stale{};
    while (xQueueReceive(result_q_, &stale, 0) == pdTRUE) {
    }
    fw::Command queued = cmd;
    queued.seq = next_seq_++;
    if (next_seq_ == 0U) {
        next_seq_ = 1U;
    }
    const uint32_t begin = nowMs();
    if (xQueueSend(cmd_q_, &queued, 0) != pdTRUE) {
        // 队列满：调用方任务上下文，记事件后立即返回 kBusy（不阻塞、不重试）。
        traceExec(ExecTrace::kCallDrop, static_cast<uint32_t>(cmd.target), timeoutMs);
        return fw::Result::kBusy;
    }
    for (;;) {
        const uint32_t elapsed = nowMs() - begin;
        fw::CommandResult received{};
        if (xQueueReceive(result_q_, &received,
                          waitTicks(elapsed < timeoutMs ? timeoutMs - elapsed : 0U)) != pdTRUE) {
            return fw::Result::kTimeout;
        }
        if (received.seq == queued.seq) {
            result = received;
            return fw::Result::kOk;
        }
        if ((nowMs() - begin) >= timeoutMs) {
            return fw::Result::kTimeout;
        }
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
 * @note   不拆（ADR-0013 §2.4 的保留例外）：每轮实时路径，等待→排空命令→轮询模块→统计→喂狗的顺序与 pending 标志构成单一状态机，拆出需 4 个以上出入参且辅助函数要改冻结头。
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
        uint32_t drops = 0;
        while (received || xQueueReceive(cmd_q_, &command, 0) == pdTRUE) {
            received = false;
            // available 只由本任务在 start 阶段修改；运行中为只读，不存在并发写者。
            const auto result = dispatchCommand(modules_, cfg_.id, ok_, command);
            if (xQueueSend(result_q_, &result, 0) != pdTRUE) {
                fw::CommandResult old{};
                if (xQueueReceive(result_q_, &old, 0) == pdTRUE) {
                    ++drops;
                }
                if (xQueueSend(result_q_, &result, 0) != pdTRUE) {
                    traceExec(ExecTrace::kResultDrop, result.seq, drops);
                }
            }
            ++commands;
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
            stats_.resultDrops += drops;
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
