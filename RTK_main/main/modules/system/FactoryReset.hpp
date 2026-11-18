// 整机恢复出厂的控制台入口。仅 console REPL，非 ISR。
#pragma once

#include <span>

#include "cli/Console.hpp"

namespace app {

/** @brief 确认并执行整机重置。 @param argc: 参数数。 @param argv: 参数。 @param factory: 还擦除出厂 NVS。 @retval 0 成功；非零为 ERR。 */
[[nodiscard]] int confirmFactoryReset(int argc, char **argv, bool factory) noexcept;

/** @brief 获取恢复出厂命令表。 @retval 固件生命周期有效的命令表。 */
[[nodiscard]] std::span<const cli::Cmd> factoryResetCmds() noexcept;

} // namespace app
