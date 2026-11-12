// 控制台公共辅助（ADR-0007 §4.3）：命令投递到执行器与结果回报、MAC/角色格式化。
// 线程模型：仅在 console REPL 任务调用，非 ISR。
#pragma once

#include <cstddef>
#include <span>

#include "data/app_data.hpp"

namespace app {

inline constexpr size_t kMacStrBuf = 18U;

/** 角色的可读名（NONE/PHONE/ALARM/PENDING）。 */
[[nodiscard]] const char *role_name(core::Role role) noexcept;
/** 打印 6 字节地址（大写 hex，冒号分隔）。 */
void print_mac(const core::BtAddr &addr) noexcept;
/** 格式化 6 字节地址到固定缓冲（"AA:BB:CC:DD:EE:FF"）。纯函数，线程/ISR 安全。 */
void format_mac(const core::BtAddr &addr, std::span<char, kMacStrBuf> out) noexcept;
/** 把命令结果转为 cli::fail 报错（kOk 静默返回 true，由 trampoline 打印 OK）。 */
[[nodiscard]] bool report(const char *what, const fw::CommandResult &result) noexcept;
/** 把命令投递给指定执行器并等待原始结果（调用方自行处理 out.status）；仅 console REPL 任务，非 ISR。 */
[[nodiscard]] bool submit_raw_to(rtos::Executor &executor, ModuleKind module, const char *down_msg,
                                 const fw::Command &command, fw::CommandResult &out) noexcept;
/** 把命令投递给链路执行器并等待原始结果（调用方自行处理 out.status）；仅 console REPL 任务，非 ISR。 */
[[nodiscard]] bool submit_raw(const fw::Command &command, fw::CommandResult &out) noexcept;
/** 把命令投递给链路执行器并等待结果；仅 console REPL 任务调用，非 ISR。 */
[[nodiscard]] bool submit(const fw::Command &command, const char *what) noexcept;

} // namespace app
