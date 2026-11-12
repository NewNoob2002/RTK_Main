// 链路模块控制台命令表：alarm、pair、wl、policy。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "support/console_support.hpp"
#include "modules/bt_link/bt_link_module.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

namespace rtk::app {

namespace {

/* 协议报警语义：state 0..2（解除/预警/报警），level 0..3（严重程度）。 */
constexpr uint8_t kAlarmStateMax = 2U;
constexpr uint8_t kAlarmLevelMax = 3U;
/* alarm set 的参数下标（argv[0] 是命令名）。 */
constexpr size_t kAlarmStateArg = 2U;
constexpr size_t kAlarmLevelArg = 3U;

/* esp_console_cmd_func_with_context_t 固定为 int(void *, int, char **)：
 * 签名不可改为 char *const *，因此豁免 const 化；函数体只按下标读取 argv。 */
// NOLINTNEXTLINE(misc-const-correctness)
int cmd_alarm(void * /*ctx*/, int argc, char **argv) noexcept
{
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);

    /* argc 由 esp_console 统计且 >= 0；缩窄到 size_t 安全。args 长度即参数个数（含命令名）。 */
    const std::span<char *> args(argv, static_cast<size_t>(argc));

    if ((args.size() == 2U) && (strcmp(args[1], "clear") == 0)) {
        command.verb = verb(LinkVerb::kAlarmClear);
        return submit(command, "alarm clear") ? 0 : 1;
    }
    if ((args.size() != 4U) || (strcmp(args[1], "set") != 0)) {
        printf("usage: alarm set <state 0-2> <level 0-3> | alarm clear\n");
        return 1;
    }

    uint8_t state = 0U;
    uint8_t level = 0U;
    if ((!rtk::console::parse_u8(args[kAlarmStateArg], state)) || (state > kAlarmStateMax) ||
        (!rtk::console::parse_u8(args[kAlarmLevelArg], level)) || (level > kAlarmLevelMax)) {
        printf("invalid state/level\n");
        return 1;
    }
    command.args[0] = state;
    command.args[1] = level;
    command.verb = verb(LinkVerb::kAlarmSet);
    command.arg_len = 2U;
    return submit(command, "alarm set") ? 0 : 1;
}

int cmd_pair(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        printf("usage: pair\n");
        return 1;
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kPair);
    return submit(command, "pair") ? 0 : 1;
}

// NOLINTNEXTLINE(misc-const-correctness)
int cmd_wl(void * /*ctx*/, int argc, char **argv) noexcept
{
    const std::span<char *> args(argv, static_cast<size_t>(argc));

    if ((args.size() != 2U) || (strcmp(args[1], "clear") != 0)) {
        printf("usage: wl clear\n");
        return 1;
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kWhitelistClear);
    return submit(command, "wl clear") ? 0 : 1;
}

int cmd_policy(void * /*ctx*/, int /*argc*/, char ** /*argv*/) noexcept
{
    printf("link policy: %s\n", policy_name(policy_from_config()));
    return 0;
}

constexpr std::array<rtk::console::Cmd, 4> kLinkCmds{{
    {"alarm", "alarm set <state 0-2> <level 0-3> | alarm clear", nullptr, cmd_alarm, nullptr},
    {"pair", "open 60 s pairing window", nullptr, cmd_pair, nullptr},
    {"wl", "wl clear", nullptr, cmd_wl, nullptr},
    {"policy", "print current link policy", nullptr, cmd_policy, nullptr},
}};

} // namespace

std::span<const rtk::console::Cmd> link_cmds() noexcept
{
    return kLinkCmds;
}

} // namespace rtk::app
