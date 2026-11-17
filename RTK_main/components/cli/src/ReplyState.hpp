// 控制台回执状态的内部接口：只在 cli 组件内部（Console.cpp / Reply.cpp）使用，不属于冻结的公共头。
// 语义：fail() 置位，trampoline 在调用 handler 前 reset、返回后查询，据此避免重复打印 ERR。
// 状态属于执行命令的单个任务（REPL 任务串行执行命令），不得跨任务读取。
#pragma once

namespace cli {
namespace detail {

/** @brief 清除“本次命令已给出失败原因”标记。 @note trampoline 在调用 handler 之前调用，只在 REPL 任务。 */
void replyReset() noexcept;

/**
 * @brief  查询本次命令是否已由 fail() 打印过原因
 * @retval true  已打印，trampoline 不再补 "ERR <返回值>"
 * @retval false 未打印
 * @note   只在 REPL 任务内、同一次命令调用期间使用。
 */
[[nodiscard]] bool replyFailed() noexcept;

} // namespace detail
} // namespace cli
