// app_fw 组件私有：Trace.cpp 与 Tlog.cpp 共享的时钟读取（不对外暴露，也不进入冻结头 fw/Trace.hpp）。
// 时间戳必须来自同一个 trace_install 注入的时钟，trace 事件与文本记录才能在读者任务里按时间合并
// （ADR-0009 §2.11）。线程：返回的函数指针在 trace_install 之后不可变；取时间本身由调用方保证非 ISR。
#pragma once

#include "fw/Trace.hpp"

namespace fw::detail {

/** 已安装的时钟；未安装时返回 nullptr（log_install 据此返回 kInvalidState）。 */
[[nodiscard]] TraceClockFn installed_clock() noexcept;

} // namespace fw::detail
