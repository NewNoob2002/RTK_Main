// 主机回归，核对 ADR-0016 的命令、help 动词组及 console-only 安全集合。
// 编译期断言，单线程测试入口，非 ISR。
#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

#include "app/ConsoleRegistration.hpp"

namespace {

using app::ConsoleProfile;
using app::console_registration::commandNames;
using app::console_registration::isSafe;
using app::console_registration::verbGroupNames;
using namespace std::string_view_literals;

constexpr std::array kDevCommands{"status"sv, "log"sv,     "sys"sv,   "bt"sv,
                                  "alarm"sv,  "config"sv,  "rtcm"sv,  "gnss"sv,
                                  "debug"sv,  "factory"sv, "freset"sv};
constexpr std::array kProdCommands{"status"sv, "log"sv,     "sys"sv,
                                   "config"sv, "factory"sv, "freset"sv};
constexpr std::array kDevGroups{"status"sv, "log"sv,   "sys"sv, "rtcm"sv,
                                "gnss"sv,   "debug"sv, "bt"sv,  "factory"sv};
constexpr std::array kProdGroups{"status"sv, "log"sv, "sys"sv, "factory"sv};
constexpr std::array kDevGroupsWithoutGnssOrTrace{"status"sv, "log"sv, "sys"sv,
                                                  "rtcm"sv,   "bt"sv,  "factory"sv};
constexpr std::array kSafe{"status"sv, "log"sv, "sys"sv, "config"sv};
constexpr std::array<std::string_view, 0U> kNone{};

constexpr auto kRegistered =
    std::views::filter([](std::string_view name) { return !name.empty(); });

static_assert(app::kConsoleProfile == ConsoleProfile::EXPECTED_CONSOLE_PROFILE);
static_assert(std::string_view{app::kConsoleProfileName} == EXPECTED_CONSOLE_PROFILE_NAME);

static_assert(std::ranges::equal(commandNames(ConsoleProfile::kDev, true, true) | kRegistered,
                                 kDevCommands));
static_assert(std::ranges::equal(commandNames(ConsoleProfile::kProduction, true, true) |
                                     kRegistered,
                                 kProdCommands));
static_assert(std::ranges::equal(commandNames(ConsoleProfile::kNone, true, true) | kRegistered,
                                 kNone));
static_assert(std::ranges::equal(verbGroupNames(ConsoleProfile::kDev, true, true) | kRegistered,
                                 kDevGroups));
static_assert(std::ranges::equal(verbGroupNames(ConsoleProfile::kProduction, false, false) |
                                     kRegistered,
                                 kProdGroups));
static_assert(std::ranges::equal(verbGroupNames(ConsoleProfile::kProduction, true, false) |
                                     kRegistered,
                                 kProdGroups));
static_assert(std::ranges::equal(verbGroupNames(ConsoleProfile::kProduction, true, true) |
                                     kRegistered,
                                 kProdGroups));
static_assert(std::ranges::equal(verbGroupNames(ConsoleProfile::kDev, false, false) | kRegistered,
                                 kDevGroupsWithoutGnssOrTrace));
static_assert(std::ranges::equal(verbGroupNames(ConsoleProfile::kNone, true, true) | kRegistered,
                                 kNone));
static_assert(std::ranges::equal(kDevCommands | std::views::filter(isSafe), kSafe));
static_assert(std::ranges::equal(kProdCommands | std::views::filter(isSafe), kSafe));
static_assert(!isSafe("unknown"));
static_assert(std::ranges::equal(commandNames(ConsoleProfile::kDev, false, false) | kRegistered,
                                 std::array{"status"sv, "log"sv, "sys"sv, "bt"sv, "alarm"sv,
                                            "config"sv, "rtcm"sv, "factory"sv, "freset"sv}));

} // namespace

/**
 * @brief 提供 CTest 入口，命令断言在编译期执行
 * @retval 0 全部编译期断言通过
 * @note 单线程主机测试，非 ISR。
 */
int main()
{
    return 0;
}
