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

    [[nodiscard]] fw::Status init() noexcept
    {
        if (handle_ != nullptr) {
            return fw::Status::kBusy;
        }
        handle_ = xEventGroupCreateStatic(&group_);
        return (handle_ != nullptr) ? fw::Status::kOk : fw::Status::kNoSpace;
    }

    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vEventGroupDelete(handle_);
            handle_ = nullptr;
        }
    }

    /**
     * 等待事件位；成功返回等待返回时的事件组快照（可能含本次未请求的位），超时/未满足返回 0。
     * 未初始化或 bits 为 0 返回 0；waitAll 为 true 要求 bits 全部置位，为 false 要求至少一位。
     * xEventGroupWaitBits 超时时返回当前已置位的事件位（可能是请求位的真子集），因此这里按语义判定。
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

    void set(EventBits_t bits) noexcept
    {
        if (handle_ != nullptr) {
            xEventGroupSetBits(handle_, bits);
        }
    }

    void clear(EventBits_t bits) noexcept
    {
        if (handle_ != nullptr) {
            xEventGroupClearBits(handle_, bits);
        }
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr;
    }

private:
    StaticEventGroup_t group_{};
    EventGroupHandle_t handle_ = nullptr;
};

} // namespace rtos
