// 蓝牙配对窗口边界、控制台与工厂命令共用入口。
#pragma once

#include <cstdint>
#include <span>

#include "cli/Console.hpp"

namespace app {

inline constexpr uint16_t kPairMinSeconds = 10U;
inline constexpr uint16_t kPairMaxSeconds = 600U;
inline constexpr uint32_t kPairMsPerSecond = 1000U;

/** @brief 登记 BT 动词。 @retval 全生命周期有效的动词表。 */
[[nodiscard]] std::span<const cli::Verb> btVerbs() noexcept;
/** @brief 打开默认配对窗口。 @retval 0 成功；其他为 ERR。 @note 控制台任务，可阻塞。 */
[[nodiscard]] int openDefaultPairing() noexcept;
/** @brief 修改报警器白名单。 @param add: 添加或删除。 @param macText: 对端 MAC。 @retval 0 成功；其他为 ERR。 */
[[nodiscard]] int editTrustedAlarm(bool add, const char *macText) noexcept;

} // namespace app
