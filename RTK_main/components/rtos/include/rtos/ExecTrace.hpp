// 执行器追踪事件码：轮次超时与命令投递失败（ADR-0009；main/def/SystemDef.hpp 以此为准导出）。
// 线程：trace_exec 只写 trace 环，可在任务上下文调用；不可在 ISR 调用。不分配内存。
#pragma once

#include <cstdint>

#include "fw/Trace.hpp"

namespace rtos {

// 事件码是 trace 的 uint8_t 负载，取值由本工程冻结定义（T1b），顺序即控制台文本表的顺序。
enum class ExecTrace : uint8_t {
    kRoundOverrun = 0, // a = 本轮耗时 ms，b = 轮预算 ms
    kCallDrop = 1,     // a = 目标模块 id，b = 调用方给定的超时 ms
    kResultDrop = 2,   // 结果队列腾出空位后仍入队失败：a = 命令序号，b = 本轮已丢弃的旧结果数
    kWdtResetFail = 3, // 任务看门狗喂狗失败（此后本任务不再喂狗）：a = esp_err_t
};

// session 恒为 0：执行器与蓝牙会话无关。
inline void trace_exec(ExecTrace code, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kExec, static_cast<uint8_t>(code), 0, a, b);
}

} // namespace rtos
