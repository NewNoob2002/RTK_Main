// 控制台公共辅助（ADR-0007 §4.3）：命令投递到执行器与结果回报、MAC/角色格式化。
// 线程模型：仅在 console REPL 任务调用，非 ISR。
#pragma once

#include "data/app_data.hpp"

namespace app {

/** 角色的可读名（NONE/PHONE/ALARM/PENDING）。 */
[[nodiscard]] const char *role_name(core::Role role) noexcept;
/** 打印 6 字节地址（大写 hex，冒号分隔）。 */
void print_mac(const core::BtAddr &addr) noexcept;
/** 把命令结果输出成一行日志；成功返回 true。 */
[[nodiscard]] bool report(const char *what, const fw::CommandResult &result) noexcept;
/** 把命令投递给链路执行器并等待结果；仅 console REPL 任务调用，非 ISR。 */
[[nodiscard]] bool submit(const fw::Command &command, const char *what) noexcept;

} // namespace app
