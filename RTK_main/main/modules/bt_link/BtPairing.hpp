// 蓝牙配对窗口边界、控制台与工厂命令共用入口。
#pragma once

#include <cstdint>
#include <span>

#include "cli/Console.hpp"
#include "def/LinkDef.hpp"
#include "fw/Command.hpp"

namespace app {

inline constexpr uint16_t kPairMinSeconds = 10U;
inline constexpr uint16_t kPairMaxSeconds = 600U;
inline constexpr uint32_t kPairMsPerSecond = 1000U;

/**
 * @brief 构造配对窗口命令。
 * @param seconds: 通过校验的秒数，0 关闭。
 * @param confirm: 打开窗口时是否逐次确认。
 * @retval kLink 使用的内部命令。
 */
[[nodiscard]] inline fw::Command makePairingCommand(uint32_t seconds, bool confirm) noexcept
{
    constexpr int kBitsPerByte = 8;
    constexpr uint8_t kModeIndex = sizeof(uint16_t);
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kBtLink);
    cmd.verb = verb(seconds == 0U ? LinkVerb::kPairClose : LinkVerb::kPair);
    if (seconds != 0U) {
        cmd.args[0] = static_cast<uint8_t>(seconds);
        cmd.args[1] = static_cast<uint8_t>(seconds >> kBitsPerByte); // 秒数最大 600。
        cmd.args[kModeIndex] = confirm ? 1U : 0U;
        cmd.argLen = kModeIndex + 1U;
    }
    return cmd;
}

/** @brief 登记 BT 动词。 @retval 全生命周期有效的动词表。 */
[[nodiscard]] std::span<const cli::Verb> btVerbs() noexcept;
/** @brief 打开默认配对窗口。 @retval 0 成功；其他为 ERR。 @note 控制台任务，可阻塞。 */
[[nodiscard]] int openDefaultPairing() noexcept;
/** @brief 修改报警器白名单。 @param add: 添加或删除。 @param macText: 对端 MAC。 @retval 0 成功；其他为 ERR。 */
[[nodiscard]] int editTrustedAlarm(bool add, const char *macText) noexcept;

} // namespace app
