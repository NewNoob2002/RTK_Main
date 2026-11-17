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

    /**
     * @brief  用外供栈创建任务并绑定到 core（xTaskCreateStaticPinnedToCore）
     * @param  stack: 任务栈存储，单位是 StackType_t 元素（IDF 上每个元素 1 字节），必须非空
     * @param  name: 任务名，FreeRTOS 不复制内容，字符串寿命必须覆盖任务生命期
     * @param  priority: FreeRTOS 优先级，必须小于 configMAX_PRIORITIES
     * @param  core: 绑定核心号，取值 0..configNUMBER_OF_CORES-1
     * @param  entry: 任务入口
     * @param  arg: 传给 entry 的参数
     * @retval fw::Status::kOk 任务已创建
     * @retval fw::Status::kBusy 已启动过
     * @retval fw::Status::kInvalidArg 栈为空、name/entry 为空、core 或 priority 越界、栈深度超出 configSTACK_DEPTH_TYPE
     * @retval fw::Status::kNoSpace 任务创建失败
     * @note   只由创建者在启动期调用一次；stack 的寿命必须覆盖整个任务生命期；不可在 ISR 中调用；不分配堆
     */
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
     * @note   任意任务可调用；不阻塞
     */
    [[nodiscard]] uint32_t stackHighWaterMark() const noexcept
    {
        if (handle_ == nullptr) {
            return 0U;
        }
        // ESP-IDF task.h 明确高水位单位为字节；UBaseType_t 在目标上不宽于 uint32_t。
        return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle_));
    }

private:
    /**
     * @brief  FreeRTOS 入口跳板：取回对象并调用用户入口
     * @param  arg: 指向本 BootTask 的指针（xTaskCreateStatic 的 pvParameters）
     */
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
