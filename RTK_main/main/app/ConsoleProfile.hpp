// 控制台档位及启动降级策略（ADR-0016）。
// 档位可从任意线程读取；降级策略仅在启动任务调用，可随控制台钩子阻塞，非 ISR。
#pragma once

#include <cstdint>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

namespace app {

enum class ConsoleProfile : uint8_t { kDev, kProduction, kNone };

#if defined(CONFIG_RTK_CONSOLE_PROFILE_PRODUCTION)
inline constexpr auto kConsoleProfile = ConsoleProfile::kProduction;
inline constexpr const char *kConsoleProfileName = "production";
#elif defined(CONFIG_RTK_CONSOLE_PROFILE_NONE)
inline constexpr auto kConsoleProfile = ConsoleProfile::kNone;
inline constexpr const char *kConsoleProfileName = "none";
#else
inline constexpr auto kConsoleProfile = ConsoleProfile::kDev;
inline constexpr const char *kConsoleProfileName = "dev";
#endif

/**
 * @brief 在启动失败后放行档位允许的诊断 REPL
 * @param consoleReached: 步骤表是否已执行到 console
 * @note 仅启动任务；Init 建立控制台，Attach 切换日志通道，Run 放行 REPL。
 */
template <ConsoleProfile Profile, auto Init, auto Attach, auto Run>
void finishDegradedConsole(bool consoleReached) noexcept
{
    if constexpr (Profile != ConsoleProfile::kNone) {
        // 更早失败时补做控制台初始化，便于在 USB 供电下诊断。
        if (consoleReached || Init()) {
            Attach();
            Run();
        }
    }
}

} // namespace app
