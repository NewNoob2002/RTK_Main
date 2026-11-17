// SMP 临界区 RAII 守卫（任务上下文专用，不可在 ISR 中使用）。
// 构造即进入临界区、析构退出；临界区内不得调用 FreeRTOS API 或阻塞。
// 这是 CPP-010（构造函数不访问 RTOS）的唯一例外：守卫的语义就是“构造即加锁”。
#pragma once

#include "freertos/FreeRTOS.h"

namespace rtos {

/** portMUX 临界区守卫：构造进入、析构退出；任务上下文专用，不可在 ISR 使用。 */
class CriticalSection {
public:
    /**
     * @brief  构造即进入临界区（portENTER_CRITICAL）
     * @param  mux: 临界区互斥量，寿命必须覆盖守卫的生命期
     */
    explicit CriticalSection(portMUX_TYPE &mux) noexcept : mux_(&mux)
    {
        portENTER_CRITICAL(mux_);
    }

    /** @brief 析构即退出临界区（portEXIT_CRITICAL）。 */
    ~CriticalSection() noexcept
    {
        portEXIT_CRITICAL(mux_);
    }

    CriticalSection(const CriticalSection &) = delete;
    CriticalSection &operator=(const CriticalSection &) = delete;
    CriticalSection(CriticalSection &&) = delete;
    CriticalSection &operator=(CriticalSection &&) = delete;

private:
    portMUX_TYPE *mux_;
};

} // namespace rtos
