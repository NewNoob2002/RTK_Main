// DEV 控制台的 OTA 命令表（ADR-0019 §2.7）。
// 仅启动任务登记命令，命令处理由控制台任务执行，非 ISR。
#pragma once

#include <span>

#include "cli/Console.hpp"

namespace app {

/** @brief 取得 OTA 命令表。 @retval 固件生命周期有效的只读定义。 @note 仅启动登记时调用。 */
[[nodiscard]] std::span<const cli::Cmd> otaCmds() noexcept;

/** @brief 取得 OTA help 动词表。 @retval 固件生命周期有效的只读定义。 @note 仅启动登记时调用。 */
[[nodiscard]] std::span<const cli::Verb> otaVerbs() noexcept;

} // namespace app
