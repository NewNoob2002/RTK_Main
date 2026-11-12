// Executor 实现：静态任务主循环（等待→命令分派→按周期 poll→统计→喂狗→让出），线程模型见 executor.hpp。
// 只在执行器自己的任务中运行；不分配堆、不在 ISR 中调用。
#include "rtos/executor.hpp"

#include <limits>
#include <string_view>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "rtos/critical_section.hpp"
#include "rtos/esp_status.hpp"
#include "rtos/exec_trace.hpp"

namespace rtos {
namespace {
constexpr const char *kTag = "executor";

TickType_t wait_ticks(uint32_t ms) noexcept
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return ms != 0U && ticks == 0U ? 1U : ticks;
}
fw::CommandResult dispatch_command(std::span<const fw::Module> modules, fw::ExecutorId executor,
                                   std::span<const bool> available, const fw::Command &command) noexcept
{
    fw::CommandResult result{command.seq, fw::Result::kNotFound, 0};
    for (size_t i = 0; i < modules.size(); ++i) {
        const auto &module = modules[i];
        if (module.executor != executor || module.id != command.target) {
            continue;
        }
        result.status = !available[i]                       ? fw::Result::kInvalidState
                        : module.ops->on_command == nullptr ? fw::Result::kNotSupported
                                                            : fw::Result::kOk;
        if (result.status == fw::Result::kOk) {
            module.ops->on_command(module.self, command, result);
        }
        break;
    }
    // seq 是执行器契约，模块不得覆盖它。
    result.seq = command.seq;
    return result;
}
} // namespace

fw::Status Executor::init(const ExecutorConfig &cfg, std::span<const fw::Module> modules,
                          const ExecutorStorageView &storage) noexcept
{
    if (inited_) {
        return fw::Status::kBusy;
    }
    if (cfg.name == nullptr || std::string_view{cfg.name}.empty() || modules.size() > kExecMaxModules || cfg.core < 0 ||
        cfg.core >= configNUMBER_OF_CORES || cfg.priority >= configMAX_PRIORITIES || storage.stack.empty() ||
        storage.stack.size_bytes() > std::numeric_limits<uint32_t>::max() || storage.tcb == nullptr ||
        storage.cmd_queue == nullptr || storage.result_queue == nullptr || storage.cmd_queue == storage.result_queue ||
        storage.queue_depth == 0U || storage.queue_depth > std::numeric_limits<UBaseType_t>::max() ||
        storage.queue_depth > storage.cmd_area.size() / sizeof(fw::Command) ||
        storage.queue_depth > storage.result_area.size() / sizeof(fw::CommandResult)) {
        return fw::Status::kInvalidArg;
    }
    for (size_t i = 0; i < modules.size(); ++i) {
        if (modules[i].executor != cfg.id) {
            continue;
        }
        if (modules[i].ops == nullptr) {
            return fw::Status::kInvalidArg;
        }
        for (size_t j = 0; j < i; ++j) {
            if (modules[j].executor == cfg.id && modules[j].id == modules[i].id) {
                return fw::Status::kInvalidArg;
            }
        }
    }
    // 长度已限制在 UBaseType_t 范围内；定长命令/结果的大小远小于该上限。
    cmd_q_ = xQueueCreateStatic(static_cast<UBaseType_t>(storage.queue_depth), sizeof(fw::Command),
                                storage.cmd_area.data(), storage.cmd_queue);
    if (cmd_q_ == nullptr) {
        return fw::Status::kNoSpace;
    }
    // 同上，队列深度已经过范围检查。
    result_q_ = xQueueCreateStatic(static_cast<UBaseType_t>(storage.queue_depth), sizeof(fw::CommandResult),
                                   storage.result_area.data(), storage.result_queue);
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
        const fw::Result status = module.ops->init == nullptr ? fw::Result::kOk : module.ops->init(module.self);
        ok_[i] = fw::is_ok(status);
        if (!ok_[i]) {
            // Result 的底层类型为 uint8_t，扩展为 unsigned 无损。
            ESP_LOGE(kTag, "module init failed: id=%u status=%u", module.id, static_cast<unsigned>(status));
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
        &Executor::task_entry, cfg_.name, static_cast<uint32_t>(storage_.stack.size_bytes()), this,
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

bool Executor::module_ok(fw::ModuleId id) const noexcept
{
    const CriticalSection guard(stats_mux_);
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (modules_[i].executor == cfg_.id && modules_[i].id == id) {
            return ok_[i];
        }
    }
    return false;
}

fw::Result Executor::call(const fw::Command &cmd, fw::CommandResult &result, uint32_t timeout_ms) noexcept
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
    const uint32_t begin = now_ms();
    if (xQueueSend(cmd_q_, &queued, 0) != pdTRUE) {
        // 队列满：调用方任务上下文，记事件后立即返回 kBusy（不阻塞、不重试）。
        trace_exec(ExecTrace::kCallDrop, static_cast<uint32_t>(cmd.target), timeout_ms);
        return fw::Result::kBusy;
    }
    for (;;) {
        const uint32_t elapsed = now_ms() - begin;
        fw::CommandResult received{};
        if (xQueueReceive(result_q_, &received, wait_ticks(elapsed < timeout_ms ? timeout_ms - elapsed : 0U)) !=
            pdTRUE) {
            return fw::Result::kTimeout;
        }
        if (received.seq == queued.seq) {
            result = received;
            return fw::Result::kOk;
        }
        if ((now_ms() - begin) >= timeout_ms) {
            return fw::Result::kTimeout;
        }
    }
}

void Executor::task_entry(void *arg) noexcept
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
        if (module.executor != self.cfg_.id || !self.module_ok(module.id) || module.ops->start == nullptr) {
            continue;
        }
        const fw::Result status = module.ops->start(module.self);
        if (!fw::is_ok(status)) {
            {
                const CriticalSection guard(self.stats_mux_);
                self.ok_[i] = false;
            }
            // Result 为 uint8_t 枚举，转换为 unsigned 无损。
            ESP_LOGE(kTag, "module start failed: id=%u status=%u", module.id, static_cast<unsigned>(status));
        }
    }
    self.run();
}

void Executor::run() noexcept
{
    bool watchdog = false;
    if (cfg_.watchdog) {
        const esp_err_t error = esp_task_wdt_add(nullptr);
        watchdog = error == ESP_OK;
        if (!watchdog) {
            ESP_LOGW(kTag, "task wdt subscribe failed: %s", esp_err_to_name(error));
        }
    }
    bool pending = false;
    for (;;) {
        fw::Command command{};
        bool received = false;
        const uint32_t max_wait = pending ? 0U : cfg_.idle_wait_ms;
        if (cfg_.wait != nullptr) {
            cfg_.wait(cfg_.wait_ctx, max_wait);
        } else {
            received = xQueueReceive(cmd_q_, &command, wait_ticks(max_wait)) == pdTRUE;
        }
        const uint32_t begin = now_ms();
        uint32_t commands = 0;
        uint32_t drops = 0;
        while (received || xQueueReceive(cmd_q_, &command, 0) == pdTRUE) {
            received = false;
            // available 只由本任务在 start 阶段修改；运行中为只读，不存在并发写者。
            const auto result = dispatch_command(modules_, cfg_.id, ok_, command);
            if (xQueueSend(result_q_, &result, 0) != pdTRUE) {
                fw::CommandResult old{};
                if (xQueueReceive(result_q_, &old, 0) == pdTRUE) {
                    ++drops;
                }
                if (xQueueSend(result_q_, &result, 0) != pdTRUE) {
                    ESP_LOGE(kTag, "result enqueue failed");
                }
            }
            ++commands;
        }
        pending = false;
        for (size_t i = 0; i < modules_.size(); ++i) {
            const auto &module = modules_[i];
            if (module.executor != cfg_.id || !module_ok(module.id) || module.ops->poll == nullptr) {
                continue;
            }
            const uint32_t now = now_ms();
            if (module.poll_period_ms == 0U || (now - last_poll_ms_[i]) >= module.poll_period_ms) {
                last_poll_ms_[i] = now;
                pending = module.ops->poll(module.self, now) || pending;
            }
        }
        const uint32_t duration = now_ms() - begin;
        // 超轮预算即记事件：此处是本任务上下文，写环不阻塞；格式化留给 DebugModule。
        if (duration > cfg_.round_budget_ms) {
            trace_exec(ExecTrace::kRoundOverrun, duration, cfg_.round_budget_ms);
        }
        // IDF 高水位返回字节；本工程 ESP32 StackType_t 为 uint8_t，结果在 u32 范围内。
        const uint32_t stack_free = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        {
            const CriticalSection guard(stats_mux_);
            ++stats_.rounds;
            stats_.overruns += duration > cfg_.round_budget_ms ? 1U : 0U;
            if (duration > stats_.max_round_ms) {
                stats_.max_round_ms = duration;
            }
            stats_.commands += commands;
            stats_.result_drops += drops;
            stats_.stack_free = stack_free;
        }
        if (watchdog) {
            const esp_err_t error = esp_task_wdt_reset();
            if (error != ESP_OK) {
                ESP_LOGW(kTag, "task wdt reset failed: %s", esp_err_to_name(error));
                watchdog = false;
            }
        }
        vTaskDelay(cfg_.yield_ticks);
    }
}
} // namespace rtos
