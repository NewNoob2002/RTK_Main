// 静态队列包装：存储与队列控制块都在对象内部，init() 后不分配堆。
// 构造 constexpr、不访问 RTOS；线程安全：FreeRTOS 队列本身可多任务使用。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "fw/Status.hpp"

namespace rtos {

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

    /**
     * @brief  创建静态队列（xQueueCreateStatic）；存储区与队列块都在对象内部
     * @retval fw::Status::kOk 创建成功
     * @retval fw::Status::kBusy 已初始化
     * @retval fw::Status::kNoSpace 队列创建失败
     * @note   启动期调用；不分配堆；创建后可由多任务并发 send/receive
     */
    [[nodiscard]] fw::Status init() noexcept
    {
        if (handle_ != nullptr) {
            return fw::Status::kBusy;
        }
        handle_ = xQueueCreateStatic(static_cast<UBaseType_t>(N),
                                     static_cast<UBaseType_t>(sizeof(T)), storage_.data(), &queue_);
        return (handle_ != nullptr) ? fw::Status::kOk : fw::Status::kNoSpace;
    }

    /** @brief 删除队列（vQueueDelete）并置为未初始化；未初始化时无副作用。 */
    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vQueueDelete(handle_);
            handle_ = nullptr;
        }
    }

    /**
     * @brief  入队（xQueueSend），按字节复制 item
     * @param  item: 待入队元素
     * @param  timeoutMs: 队列满时的等待毫秒数，0 表示不等待
     * @retval true 已入队
     * @retval false 未初始化、超时或队列满
     * @note   任务上下文；不可在 ISR 中调用（需 FromISR 版本）；timeoutMs>0 会阻塞
     */
    [[nodiscard]] bool send(const T &item, uint32_t timeoutMs) noexcept
    {
        if (handle_ == nullptr) {
            return false;
        }
        return xQueueSend(handle_, &item, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }

    /**
     * @brief  出队（xQueueReceive）并把元素复制到 item
     * @param[out] item: 成功时写入出队元素；失败时内容不变
     * @param  timeoutMs: 队列空时的等待毫秒数，0 表示不等待
     * @retval true 已取出
     * @retval false 未初始化、超时或队列空
     * @note   任务上下文；不可在 ISR 中调用（需 FromISR 版本）；timeoutMs>0 会阻塞
     */
    [[nodiscard]] bool receive(T &item, uint32_t timeoutMs) noexcept
    {
        if (handle_ == nullptr) {
            return false;
        }
        return xQueueReceive(handle_, &item, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }

    /**
     * @brief  队列是否已创建
     * @retval true 已初始化；false 未初始化或已 deinit
     */
    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr;
    }

private:
    StaticQueue_t queue_{};
    std::array<uint8_t, N * sizeof(T)> storage_{};
    QueueHandle_t handle_ = nullptr;
};

} // namespace rtos
