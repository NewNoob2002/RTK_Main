// 控制台回执状态的内部接口：只在 cli 组件内部（console.cpp / reply.cpp）使用，不属于冻结的公共头。
// 语义：fail() 置位，trampoline 在调用 handler 前 reset、返回后查询，据此避免重复打印 ERR。
// 状态属于执行命令的单个任务（REPL 任务串行执行命令），不得跨任务读取。
#pragma once

namespace cli {
namespace detail {

/** 清除“本次命令已给出失败原因”标记；trampoline 在调用 handler 之前调用。 */
void reply_reset() noexcept;

/** 本次命令是否已由 fail() 打印过原因。 */
[[nodiscard]] bool reply_failed() noexcept;

} // namespace detail
} // namespace cli
