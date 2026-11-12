// 静态任务包装：栈与 TCB 都在对象内部，启动后不分配堆。
// 构造 constexpr、不访问 RTOS；硬件/RTOS 初始化放在 start()（CPP-010）。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtk/status.hpp"

namespace rtk::os {

/**
 * 静态任务。StackBytes 是栈字节数（IDF 的 xTaskCreate* 栈深度单位为字节，
 * 本包装按 StackBytes / sizeof(StackType_t) 换算）。
 * 线程安全：start() 只由创建者调用一次；任务句柄只读。ISR 安全：否。
 */
template <size_t StackBytes> class StaticTask {
public:
    using Entry = void (*)(void *arg) noexcept;

    static_assert(StackBytes > 0U, "stack must be non-empty");
    static_assert((StackBytes % sizeof(StackType_t)) == 0U, "stack must be word-aligned");

    constexpr StaticTask() noexcept = default;

    StaticTask(const StaticTask &) = delete;
    StaticTask &operator=(const StaticTask &) = delete;
    StaticTask(StaticTask &&) = delete;
    StaticTask &operator=(StaticTask &&) = delete;
    ~StaticTask() = default;

    /** 创建并启动静态任务；重复调用返回 kBusy，参数非法返回 kInvalidArg。 */
    [[nodiscard]] Status start(const char *name, uint32_t priority, BaseType_t core, Entry entry, void *arg) noexcept
    {
        if (handle_ != nullptr) {
            return Status::kBusy;
        }
        if ((name == nullptr) || (entry == nullptr)) {
            return Status::kInvalidArg;
        }

        // 任务创建后可能立即运行，先填好入口参数再创建。
        entry_ = entry;
        arg_ = arg;
        handle_ = xTaskCreateStaticPinnedToCore(&StaticTask::trampoline, name,
                                                static_cast<configSTACK_DEPTH_TYPE>(kStackDepth), this,
                                                static_cast<UBaseType_t>(priority), stack_.data(), &tcb_, core);
        if (handle_ == nullptr) {
            entry_ = nullptr;
            arg_ = nullptr;
            return Status::kNoSpace;
        }
        return Status::kOk;
    }

    [[nodiscard]] bool running() const noexcept
    {
        return handle_ != nullptr;
    }

    /** 栈高水位（剩余最小字节数）。未启动返回 0。 */
    [[nodiscard]] uint32_t stack_high_water_mark() const noexcept
    {
        if (handle_ == nullptr) {
            return 0;
        }
        // IDF v6.1 当前支持目标的 StackType_t 是 uint8_t（esp32 见 portmacro.h 的 typedef），
        // uxTaskGetStackHighWaterMark() 按 StackType_t 计数，因此返回值本来就是字节数；
        // 下面乘 sizeof(StackType_t) 是恒等换算，保留只为让接口按字节返回。
        // 剩余量 ≤ StackBytes / sizeof(StackType_t)，32 位目标上乘法不会溢出。
        const uint32_t words = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle_));
        const uint32_t word_bytes = static_cast<uint32_t>(sizeof(StackType_t));
        return words * word_bytes;
    }

private:
    static constexpr size_t kStackDepth = StackBytes / sizeof(StackType_t);

    static void trampoline(void *arg) noexcept
    {
        const auto *self = static_cast<const StaticTask *>(arg);
        self->entry_(self->arg_);
    }

    StaticTask_t tcb_{};
    std::array<StackType_t, kStackDepth> stack_{};
    TaskHandle_t handle_ = nullptr;
    Entry entry_ = nullptr;
    void *arg_ = nullptr;
};

} // namespace rtk::os
