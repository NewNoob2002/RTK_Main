// DEV 控制台 OLED 状态命令表（ADR-0016、ADR-0020）。
// 仅启动任务登记命令，命令处理由控制台任务执行，非 ISR。
#pragma once

#include <span>

#include "cli/Console.hpp"

namespace app {

/**
 * @brief 取得 OLED 控制台命令表（DEV 档注册）
 * @retval 固件生命周期有效的静态只读表视图
 * @note 仅启动登记时读取，不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> oledCmds() noexcept;

/**
 * @brief 取得 OLED 帮助动词表（DEV 档注册）
 * @retval 固件生命周期有效的静态只读表视图
 * @note 仅启动登记时读取，不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> oledVerbs() noexcept;

} // namespace app
