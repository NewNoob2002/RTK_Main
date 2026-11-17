// 外供栈静态任务包装：TCB 在对象内部，任务栈由启动期 boot_alloc() 注入，运行时不分配堆。
// 线程安全：start() 只由创建者调用一次；查询接口可在任务运行后只读调用。ISR 安全：否。
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw/Status.hpp"

namespace rtos {

/**
 * 接收外部栈 span 的静态任务包装（与 StaticTask<N> 同接口语义，去除内嵌大栈数组）。
 * 栈寿命必须覆盖整个任务生命期（来自 sealed BootArena 或静态存储）。
 */
class BootTask {
public:
    using Entry = void (*)(void *arg) noexcept;

    constexpr BootTask() noexcept = default;

    BootTask(const BootTask &) = delete;
    BootTask &operator=(const BootTask &) = delete;
    BootTask(BootTask &&) = delete;
    BootTask &operator=(BootTask &&) = delete;
    ~BootTask() = default;

    /** 创建并启动任务；重复调用返回 kBusy，栈或参数非法返回 kInvalidArg。 */
    [[nodiscard]] fw::Status start(std::span<StackType_t> stack, const char *name,
                                   uint32_t priority, BaseType_t core, Entry entry,
                                   void *arg) noexcept
    {
        if (handle_ != nullptr) {
            return fw::Status::kBusy;
        }
        if (stack.empty() || (stack.data() == nullptr) || (name == nullptr) || (*name == '\0') ||
            (entry == nullptr) || (core < 0) || (core >= configNUMBER_OF_CORES) ||
            (priority >= configMAX_PRIORITIES) ||
            (stack.size_bytes() > std::numeric_limits<configSTACK_DEPTH_TYPE>::max())) {
            return fw::Status::kInvalidArg;
        }
        stack_ = stack;
        entry_ = entry;
        arg_ = arg;
        // ESP-IDF 的栈深度单位为字节；上面已验证深度和优先级的转换范围，绑核理由由调用方记录。
        handle_ = xTaskCreateStaticPinnedToCore(
            &BootTask::trampoline, name, static_cast<configSTACK_DEPTH_TYPE>(stack_.size_bytes()),
            this, static_cast<UBaseType_t>(priority), stack_.data(), &tcb_, core);
        if (handle_ == nullptr) {
            stack_ = std::span<StackType_t>{};
            entry_ = nullptr;
            arg_ = nullptr;
            return fw::Status::kNoSpace;
        }
        return fw::Status::kOk;
    }

    [[nodiscard]] bool running() const noexcept
    {
        return handle_ != nullptr;
    }

    /** 栈高水位（剩余最小字节数）；未启动返回 0。 */
    [[nodiscard]] uint32_t stack_high_water_mark() const noexcept
    {
        if (handle_ == nullptr) {
            return 0U;
        }
        // ESP-IDF task.h 明确高水位单位为字节；UBaseType_t 在目标上不宽于 uint32_t。
        return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle_));
    }

private:
    static void trampoline(void *arg) noexcept
    {
        const auto *self = static_cast<const BootTask *>(arg);
        self->entry_(self->arg_);
    }

    StaticTask_t tcb_{};
    std::span<StackType_t> stack_{};
    TaskHandle_t handle_ = nullptr;
    Entry entry_ = nullptr;
    void *arg_ = nullptr;
};

} // namespace rtos
