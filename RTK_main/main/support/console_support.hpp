// 控制台公共辅助（ADR-0007 §4.3）：命令投递到执行器与结果回报、MAC/角色格式化。
// 线程模型：仅在 console REPL 任务调用，非 ISR。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once

#include "data/app_data.hpp"

namespace rtk::app {

[[nodiscard]] const char *role_name(core::Role role) noexcept;
void print_mac(const core::BtAddr &addr) noexcept;
[[nodiscard]] bool report(const char *what, const ::app::CommandResult &result) noexcept;
[[nodiscard]] bool submit(const ::app::Command &command, const char *what) noexcept;

} // namespace rtk::app
