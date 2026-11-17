// 把 portMUX_TYPE 包装成 fw::LockOps，供 fw::Slot / fw::FanoutRing 在目标板上使用。
// 任务上下文专用（不可在 ISR 中使用）；临界区内只允许有界 memcpy。
// 冻结接口（ADR-0007 §3.5）。实现方不得修改。
#pragma once

#include "fw/Lock.hpp"
#include "freertos/FreeRTOS.h"

namespace rtos {

namespace detail {
/**
 * @brief  对 portMUX_TYPE 加锁（portENTER_CRITICAL）
 * @param  ctx: 指向 portMUX_TYPE 的上下文指针
 * @note   任务上下文专用；不可在 ISR 中调用
 */
inline void spinLock(void *ctx) noexcept
{
    portENTER_CRITICAL(static_cast<portMUX_TYPE *>(ctx));
}
/**
 * @brief  对 portMUX_TYPE 解锁（portEXIT_CRITICAL）
 * @param  ctx: 指向 portMUX_TYPE 的上下文指针
 * @note   必须与同一 ctx 的 spinLock 配对；不可在 ISR 中调用
 */
inline void spinUnlock(void *ctx) noexcept
{
    portEXIT_CRITICAL(static_cast<portMUX_TYPE *>(ctx));
}
} // namespace detail

/**
 * @brief  把 portMUX_TYPE 包装成 fw::LockOps，供 fw::Slot / fw::FanoutRing 在目标板上使用
 * @param  mux: 临界区互斥量，寿命必须覆盖所有使用返回值的对象
 * @retval 引用 mux 的锁操作表
 * @note   任务上下文专用；临界区内只允许有界 memcpy
 */
[[nodiscard]] inline fw::LockOps spinLockOps(portMUX_TYPE &mux) noexcept
{
    return fw::LockOps{&detail::spinLock, &detail::spinUnlock, &mux};
}

} // namespace rtos
