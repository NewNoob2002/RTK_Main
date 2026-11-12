// SMP 临界区 RAII 守卫（任务上下文专用，不可在 ISR 中使用）。
// 构造即进入临界区、析构退出；临界区内不得调用 FreeRTOS API 或阻塞。
// 这是 CPP-010（构造函数不访问 RTOS）的唯一例外：守卫的语义就是“构造即加锁”。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）
#pragma once

#include "freertos/FreeRTOS.h"

namespace rtk::os {

class CriticalSection {
public:
    explicit CriticalSection(portMUX_TYPE &mux) noexcept : mux_(&mux)
    {
        portENTER_CRITICAL(mux_);
    }

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

} // namespace rtk::os
