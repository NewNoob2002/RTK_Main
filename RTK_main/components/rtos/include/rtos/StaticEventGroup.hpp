// 静态事件组包装：控制块在对象内部，init() 后不分配堆。
// 构造 constexpr、不访问 RTOS；线程安全：事件组本身可多任务/ISR 使用。
#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "fw/Status.hpp"

namespace rtos {

/** 静态分配的事件组 RAII 包装；任务上下文使用，wait 按毫秒阻塞，非 ISR。 */
class StaticEventGroup {
public:
    constexpr StaticEventGroup() noexcept = default;

    StaticEventGroup(const StaticEventGroup &) = delete;
    StaticEventGroup &operator=(const StaticEventGroup &) = delete;
    StaticEventGroup(StaticEventGroup &&) = delete;
    StaticEventGroup &operator=(StaticEventGroup &&) = delete;
    ~StaticEventGroup() = default;

    /**
     * @brief  创建静态事件组（xEventGroupCreateStatic）
     * @retval fw::Status::kOk 创建成功
     * @retval fw::Status::kBusy 已初始化
     * @retval fw::Status::kNoSpace 创建失败
     * @note   启动期调用；不分配堆
     */
    [[nodiscard]] fw::Status init() noexcept
    {
        if (handle_ != nullptr) {
            return fw::Status::kBusy;
        }
        handle_ = xEventGroupCreateStatic(&group_);
        return (handle_ != nullptr) ? fw::Status::kOk : fw::Status::kNoSpace;
    }

    /** @brief 删除事件组（vEventGroupDelete）；有任务在 wait 时不得调用。 */
    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vEventGroupDelete(handle_);
            handle_ = nullptr;
        }
    }

    /**
     * @brief  等待事件位（xEventGroupWaitBits）并返回事件组快照
     * @param  bits: 关心的事件位，必须非 0
     * @param  clearOnExit: true 时等待成功后清除这些位
     * @param  waitAll: true 要求 bits 全部置位；false 要求至少一位
     * @param  timeoutMs: 最长等待毫秒数，0 表示只检查一次
     * @retval 非 0 等待返回时的事件组快照（可能含本次未请求的位）
     * @retval 0 未初始化、bits 为 0、超时或等待条件未满足
     * @note   任务上下文，timeoutMs>0 会阻塞；不可在 ISR 中调用
     */
    [[nodiscard]] EventBits_t wait(EventBits_t bits, bool clearOnExit, bool waitAll,
                                   uint32_t timeoutMs) noexcept
    {
        if ((handle_ == nullptr) || (bits == 0U)) {
            return 0;
        }
        const EventBits_t got =
            xEventGroupWaitBits(handle_, bits, clearOnExit ? pdTRUE : pdFALSE,
                                waitAll ? pdTRUE : pdFALSE, pdMS_TO_TICKS(timeoutMs));
        const EventBits_t wanted = got & bits;
        const bool satisfied = waitAll ? (wanted == bits) : (wanted != 0U);
        // 成功时保留完整快照 got；只有等待条件未满足才返回 0。
        return satisfied ? got : 0U;
    }

    /**
     * @brief  置位事件位（xEventGroupSetBits）
     * @param  bits: 要置位的位掩码
     * @note   任务上下文；不可在 ISR 中调用（ISR 需 xEventGroupSetBitsFromISR）
     */
    void set(EventBits_t bits) noexcept
    {
        if (handle_ != nullptr) {
            xEventGroupSetBits(handle_, bits);
        }
    }

    /**
     * @brief  清除事件位（xEventGroupClearBits）
     * @param  bits: 要清除的位掩码
     * @note   任务上下文；不可在 ISR 中调用
     */
    void clear(EventBits_t bits) noexcept
    {
        if (handle_ != nullptr) {
            xEventGroupClearBits(handle_, bits);
        }
    }

    /**
     * @brief  事件组是否已创建
     * @retval true 已初始化；false 未初始化或已 deinit
     */
    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr;
    }

private:
    StaticEventGroup_t group_{};
    EventGroupHandle_t handle_ = nullptr;
};

} // namespace rtos
