// 把 portMUX_TYPE 包装成 fw::LockOps，供 fw::Slot / fw::FanoutRing 在目标板上使用。
// 任务上下文专用（不可在 ISR 中使用）；临界区内只允许有界 memcpy。
// 冻结接口（ADR-0007 §3.5）。实现方不得修改。
#pragma once

#include "rtk/fw/lock.hpp"
#include "freertos/FreeRTOS.h"

namespace rtk::os {

namespace detail {
inline void spin_lock(void *ctx) noexcept
{
    portENTER_CRITICAL(static_cast<portMUX_TYPE *>(ctx));
}
inline void spin_unlock(void *ctx) noexcept
{
    portEXIT_CRITICAL(static_cast<portMUX_TYPE *>(ctx));
}
} // namespace detail

/** mux 的生命周期必须覆盖所有使用返回值的对象（通常是 constinit 全局）。 */
[[nodiscard]] inline fw::LockOps spin_lock_ops(portMUX_TYPE &mux) noexcept
{
    return fw::LockOps{&detail::spin_lock, &detail::spin_unlock, &mux};
}

} // namespace rtk::os
