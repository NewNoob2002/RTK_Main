// 静态队列包装：存储与队列控制块都在对象内部，init() 后不分配堆。
// 构造 constexpr、不访问 RTOS；线程安全：FreeRTOS 队列本身可多任务使用。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "rtk/status.hpp"

namespace rtk::os {

template <typename T, size_t N> class StaticQueue {
public:
    static_assert(N > 0U, "queue depth must be positive");
    static_assert(std::is_trivially_copyable<T>::value, "FreeRTOS queue copies bytes");

    constexpr StaticQueue() noexcept = default;

    StaticQueue(const StaticQueue &) = delete;
    StaticQueue &operator=(const StaticQueue &) = delete;
    StaticQueue(StaticQueue &&) = delete;
    StaticQueue &operator=(StaticQueue &&) = delete;
    ~StaticQueue() = default;

    /** 创建静态队列；重复调用返回 kBusy。 */
    [[nodiscard]] Status init() noexcept
    {
        if (handle_ != nullptr) {
            return Status::kBusy;
        }
        handle_ = xQueueCreateStatic(static_cast<UBaseType_t>(N), static_cast<UBaseType_t>(sizeof(T)), storage_.data(),
                                     &queue_);
        return (handle_ != nullptr) ? Status::kOk : Status::kNoSpace;
    }

    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vQueueDelete(handle_);
            handle_ = nullptr;
        }
    }

    /** 入队；timeout_ms=0 不等待。未初始化或超时返回 false。 */
    [[nodiscard]] bool send(const T &item, uint32_t timeout_ms) noexcept
    {
        if (handle_ == nullptr) {
            return false;
        }
        return xQueueSend(handle_, &item, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }

    /** 出队；timeout_ms=0 不等待。未初始化或超时返回 false。 */
    [[nodiscard]] bool receive(T &item, uint32_t timeout_ms) noexcept
    {
        if (handle_ == nullptr) {
            return false;
        }
        return xQueueReceive(handle_, &item, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr;
    }

private:
    StaticQueue_t queue_{};
    std::array<uint8_t, N * sizeof(T)> storage_{};
    QueueHandle_t handle_ = nullptr;
};

} // namespace rtk::os
