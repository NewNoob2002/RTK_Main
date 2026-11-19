// ADR-0016 的命令及 help 动词组清单，供固件登记与主机断言共用。
// 纯 constexpr 查询，无外部依赖，可在任意任务或 ISR 调用。
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

#include "app/ConsoleProfile.hpp"

namespace app {

namespace console_registration {

using namespace std::string_view_literals;

/** @brief 判断命令能否在 kConsoleOnly 阶段执行
 * @param name: 顶层命令名
 * @retval true 允许诊断
 * @retval false 仅在正常运行阶段可用
 */
[[nodiscard]] constexpr bool isSafe(std::string_view name) noexcept
{
    constexpr std::array kSafeNames{"status"sv, "log"sv, "sys"sv, "config"sv, "radio"sv};
    return std::ranges::find(kSafeNames, name) != kSafeNames.end();
}

/**
 * @brief 取得档位的顶层命令名字表，顺序与登记一致
 * @param profile: 构建档位
 * @param gnssEnabled: 是否编译 GNSS 模块
 * @param traceEnabled: 是否编译追踪模块
 * @param wifiConfigEnabled: 是否允许切换到 Wi-Fi 配置模式
 * @retval 定长名字表，空项表示该命令关闭
 */
[[nodiscard]] constexpr auto commandNames(ConsoleProfile profile, bool gnssEnabled,
                                          bool traceEnabled, bool wifiConfigEnabled) noexcept
{
    constexpr std::string_view kDisabled{};
    const bool repl = profile != ConsoleProfile::kNone;
    const bool dev = profile == ConsoleProfile::kDev;
    return std::array{
        repl ? "status"sv : kDisabled,
        repl ? "log"sv : kDisabled,
        repl ? "sys"sv : kDisabled,
        dev ? "bt"sv : kDisabled,
        dev ? "alarm"sv : kDisabled,
        repl ? "config"sv : kDisabled,
        dev ? "rtcm"sv : kDisabled,
        dev && gnssEnabled ? "gnss"sv : kDisabled,
        dev && traceEnabled ? "debug"sv : kDisabled,
        repl ? "factory"sv : kDisabled,
        repl ? "freset"sv : kDisabled,
        dev && wifiConfigEnabled ? "radio"sv : kDisabled,
    };
}

/**
 * @brief 取得供 help 查询的动词组名字表，保留原展示顺序
 * @param profile: 构建档位
 * @param gnssEnabled: 是否编译 GNSS 模块
 * @param traceEnabled: 是否编译追踪模块
 * @param wifiConfigEnabled: 是否允许切换到 Wi-Fi 配置模式
 * @retval 定长名字表，空项表示该动词组关闭
 */
[[nodiscard]] constexpr auto verbGroupNames(ConsoleProfile profile, bool gnssEnabled,
                                            bool traceEnabled, bool wifiConfigEnabled) noexcept
{
    constexpr std::string_view kDisabled{};
    const bool repl = profile != ConsoleProfile::kNone;
    const bool dev = profile == ConsoleProfile::kDev;
    return std::array{
        repl ? "status"sv : kDisabled,
        repl ? "log"sv : kDisabled,
        repl ? "sys"sv : kDisabled,
        dev ? "rtcm"sv : kDisabled,
        dev && gnssEnabled ? "gnss"sv : kDisabled,
        dev && traceEnabled ? "debug"sv : kDisabled,
        dev ? "bt"sv : kDisabled,
        repl ? "factory"sv : kDisabled,
        dev && wifiConfigEnabled ? "radio"sv : kDisabled,
    };
}

} // namespace console_registration
} // namespace app
