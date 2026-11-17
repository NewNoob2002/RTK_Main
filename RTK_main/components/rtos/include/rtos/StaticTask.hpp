// 静态任务包装：栈与 TCB 都在对象内部，启动后不分配堆。
// 构造 constexpr、不访问 RTOS；硬件/RTOS 初始化放在 start()（CPP-010）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw/Status.hpp"

namespace rtos {

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

    /**
     * @brief  创建静态任务并绑定到 core（xTaskCreateStaticPinnedToCore）
     * @param  name: 任务名，FreeRTOS 不复制内容，字符串寿命必须覆盖任务生命期
     * @param  priority: FreeRTOS 优先级
     * @param  core: 绑定核心号，取值 0..configNUMBER_OF_CORES-1
     * @param  entry: 任务入口
     * @param  arg: 传给 entry 的参数
     * @retval fw::Status::kOk 任务已创建
     * @retval fw::Status::kBusy 已启动过
     * @retval fw::Status::kInvalidArg name 或 entry 为空
     * @retval fw::Status::kNoSpace 任务创建失败
     * @note   只由创建者在启动期调用一次；不可在 ISR 中调用；不分配堆
     * @note   栈取自对象内部的 StackBytes 字节（按词长换算成 kStackDepth 个 StackType_t）
     */
    [[nodiscard]] fw::Status start(const char *name, uint32_t priority, BaseType_t core,
                                   Entry entry, void *arg) noexcept
    {
        if (handle_ != nullptr) {
            return fw::Status::kBusy;
        }
        if ((name == nullptr) || (entry == nullptr)) {
            return fw::Status::kInvalidArg;
        }

        // 任务创建后可能立即运行，先填好入口参数再创建。
        entry_ = entry;
        arg_ = arg;
        handle_ = xTaskCreateStaticPinnedToCore(
            &StaticTask::trampoline, name, static_cast<configSTACK_DEPTH_TYPE>(kStackDepth), this,
            static_cast<UBaseType_t>(priority), stack_.data(), &tcb_, core);
        if (handle_ == nullptr) {
            entry_ = nullptr;
            arg_ = nullptr;
            return fw::Status::kNoSpace;
        }
        return fw::Status::kOk;
    }

    /**
     * @brief  任务是否已创建成功
     * @retval true 已启动；false 尚未启动或创建失败
     */
    [[nodiscard]] bool running() const noexcept
    {
        return handle_ != nullptr;
    }

    /**
     * @brief  查询栈高水位
     * @retval 任务运行以来剩余栈的最小字节数；未启动返回 0
     * @note   任意任务可调用；uxTaskGetStackHighWaterMark 本身不阻塞
     */
    [[nodiscard]] uint32_t stackHighWaterMark() const noexcept
    {
        if (handle_ == nullptr) {
            return 0;
        }
        // IDF v6.1 当前支持目标的 StackType_t 是 uint8_t（esp32 见 portmacro.h 的 typedef），
        // uxTaskGetStackHighWaterMark() 按 StackType_t 计数，因此返回值本来就是字节数；
        // 下面乘 sizeof(StackType_t) 是恒等换算，保留只为让接口按字节返回。
        // 剩余量 ≤ StackBytes / sizeof(StackType_t)，32 位目标上乘法不会溢出。
        const uint32_t words = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle_));
        const uint32_t wordBytes = static_cast<uint32_t>(sizeof(StackType_t));
        return words * wordBytes;
    }

private:
    static constexpr size_t kStackDepth = StackBytes / sizeof(StackType_t);

    /**
     * @brief  FreeRTOS 入口跳板：取回对象并调用用户入口
     * @param  arg: 指向本 StaticTask 的指针（xTaskCreateStatic 的 pvParameters）
     */
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

} // namespace rtos
