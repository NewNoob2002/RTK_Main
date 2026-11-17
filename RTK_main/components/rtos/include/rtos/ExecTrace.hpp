// 执行器追踪事件码：轮次超时、命令投递失败等（ADR-0009；main/def/SystemDef.hpp 以此为准导出）。
// traceExec 只写 trace 环，可在任务/链路上下文调用；不可在 ISR 调用；不分配内存。
#pragma once

#include <cstdint>

#include "fw/Trace.hpp"

namespace rtos {

// 事件码是 trace 的 uint8_t 负载，取值与顺序由 ADR-0009 §2.2 冻结，顺序即控制台文本表的顺序。
enum class ExecTrace : uint8_t {
    kRoundOverrun = 0, // a = 本轮耗时 ms，b = 轮预算 ms
    kCallDrop = 1,     // a = 目标模块 id，b = 调用方给定的超时 ms
    kResultDrop = 2,   // 结果队列腾出空位后仍入队失败：a = 命令序号，b = 本轮已丢弃的旧结果数
    kWdtResetFail = 3, // 任务看门狗喂狗失败（此后本任务不再喂狗）：a = esp_err_t
};

// session 恒为 0：执行器与蓝牙会话无关。
/**
 * @brief  写一条执行器 trace 事件
 * @param  code: 事件码
 * @param  a: 事件负载 a，含义随 code
 * @param  b: 事件负载 b，含义随 code
 * @note   任务上下文调用；不可在 ISR 调用；不阻塞、不分配
 */
inline void traceExec(ExecTrace code, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kExec, static_cast<uint8_t>(code), 0, a, b);
}

} // namespace rtos
