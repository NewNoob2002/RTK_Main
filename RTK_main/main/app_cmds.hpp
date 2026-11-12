// 各模块控制台命令表导出声明（ADR-0007 §3.3）。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once

#include <span>

#include "app_ids.hpp" // IWYU pragma: keep（须在 namespace rtk::app 出现前解析冻结 OS 头中的 app::）
#include "rtk/console/console.hpp"

namespace rtk::app {

[[nodiscard]] std::span<const rtk::console::Cmd> system_cmds() noexcept;
[[nodiscard]] std::span<const rtk::console::Cmd> link_cmds() noexcept;
[[nodiscard]] std::span<const rtk::console::Cmd> storage_cmds() noexcept;
[[nodiscard]] std::span<const rtk::console::Cmd> rtcm_cmds() noexcept;

} // namespace rtk::app
