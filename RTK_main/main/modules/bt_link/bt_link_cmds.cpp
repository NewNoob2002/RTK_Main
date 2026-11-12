// 链路模块控制台命令表（ADR-0009 §2.5）：bt / wl / alarm。
// 线程：仅在 console REPL 任务调用，非 ISR；只读动词直接读 DataCenter，改状态动词经 Executor::call 投递。

#include "modules/bt_link/bt_link_module.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "esp_err.h"

#include "app_context.hpp"
#include "cli/out.hpp"
#include "support/console_support.hpp"

namespace app {

namespace {

constexpr uint8_t kAlarmStateMax = 2U;
constexpr uint8_t kAlarmLevelMax = 3U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr size_t kTwoArgs = 2U;
constexpr size_t kThreeArgs = 3U;

int cmd_bt_list(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: bt list");
    }
    const auto &data = app_context().data;
    const auto sessions = data.sessions.read();
    const auto link = data.link_status.read();

    cli::print_section("bt");
    cli::print_kv("pairing", "%s", link.pairing ? "open" : "closed");
    // 会话数上限为 kMaxSessions，转换为 unsigned 无损。
    cli::print_kv("sessions", "%u / %u", static_cast<unsigned>(sessions.count), static_cast<unsigned>(kMaxSessions));
    std::array<char, kMacStrBuf> mac_buf{};
    for (size_t idx = 0U; idx < sessions.count; ++idx) {
        const SessionSnapshot &session = sessions.entries[idx];
        format_mac(session.addr, mac_buf);
        cli::print_kv("session", "handle=0x%08" PRIx32 " role=%s mac=%s", session.handle, role_name(session.role),
                      mac_buf.data());
    }
    return 0;
}

int cmd_bt_pair(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 1) || (argv == nullptr) || (argc > static_cast<int>(kTwoArgs))) {
        return cli::fail("usage: bt pair [sec]");
    }
    constexpr uint32_t kPairSeconds = core::kPairWindowMs / kMsPerSecond;
    if (argc == static_cast<int>(kTwoArgs)) {
        const std::span<char *const> args(argv, kTwoArgs);
        uint32_t seconds = 0U;
        if (!cli::parse_u32(args[1], seconds) || (seconds == 0U)) {
            return cli::fail("bad seconds");
        }
        if (seconds != kPairSeconds) {
            // LinkManager::pair_open 接口冻结为固定 60 s 窗口（kPairWindowMs）。
            return cli::fail("only %us supported", static_cast<unsigned>(kPairSeconds));
        }
    }
    if (app_context().data.link_status.read().pairing) {
        return cli::fail("pairing busy");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kPair);
    fw::CommandResult result{};
    if (!submit_raw(command, result)) {
        return cli::kCmdFail;
    }
    if (result.status == fw::Result::kBusy) {
        return cli::fail("pairing busy");
    }
    if (!report("bt pair", result)) {
        return cli::kCmdFail;
    }
    cli::out_printf("pairing window: %u s\n", static_cast<unsigned>(kPairSeconds));
    return 0;
}

int cmd_bt_policy(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: bt policy");
    }
    cli::print_section("bt");
    cli::print_kv("policy", "%s", policy_name(policy_from_config()));
    return 0;
}

int cmd_wl_list(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: wl list");
    }
    const auto &data = app_context().data;
    const auto link = data.link_status.read();
    const auto store = data.store_status.read();

    cli::print_section("wl");
    // 白名单条数上限为 kMaxAlarms，转换为 unsigned 无损。
    cli::print_kv("count", "%u / %u", static_cast<unsigned>(link.whitelist_count), static_cast<unsigned>(kMaxAlarms));
    cli::print_kv("pending", "%s", store.whitelist_pending ? "yes" : "no");
    cli::print_kv("store", "%s", esp_err_to_name(store.whitelist_error));
    std::array<char, kMacStrBuf> mac_buf{};
    for (size_t idx = 0U; idx < link.whitelist_count; ++idx) {
        format_mac(link.whitelist[idx], mac_buf);
        cli::print_kv("addr", "%s", mac_buf.data());
    }
    return 0;
}

int wl_single_entry_stub(const char *op, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kTwoArgs)) || (argv == nullptr)) {
        return cli::fail("usage: wl %s <MAC>", op);
    }
    const std::span<char *const> args(argv, kTwoArgs);
    std::array<uint8_t, cli::kMacLen> mac{};
    if (!cli::parse_mac(args[1], mac)) {
        return cli::fail("bad MAC");
    }
    // 冻结的 LinkManager 尚未提供单条白名单增删接口（T2 任务卡列为后续项）。
    return cli::fail("not supported");
}

int cmd_wl_add(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wl_single_entry_stub("add", argc, argv);
}

int cmd_wl_del(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wl_single_entry_stub("del", argc, argv);
}

int cmd_wl_clear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: wl clear");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kWhitelistClear);
    return submit(command, "wl clear") ? 0 : cli::kCmdFail;
}

int cmd_alarm_show(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: alarm show");
    }
    const auto &data = app_context().data;
    const auto alarm = data.alarm_state.read();
    const auto heartbeat = data.heartbeat.read();

    cli::print_section("alarm");
    // 报警状态/级别均为 uint8_t，转换为 unsigned 无损。
    cli::print_kv("state", "%u", static_cast<unsigned>(alarm.state));
    cli::print_kv("level", "%u", static_cast<unsigned>(alarm.level));
    cli::print_kv("override", "%s", alarm.override ? "yes" : "no");
    if (heartbeat.present) {
        cli::print_kv("heartbeat", "seq=%u battery=%u%% state=%u", static_cast<unsigned>(heartbeat.value.seq),
                      static_cast<unsigned>(heartbeat.value.battery_pct),
                      static_cast<unsigned>(heartbeat.value.local_state));
    } else {
        cli::print_kv("heartbeat", "none");
    }
    return 0;
}

int cmd_alarm_set(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kThreeArgs)) || (argv == nullptr)) {
        return cli::fail("usage: alarm set <state 0-2> <level 0-3>");
    }
    const std::span<char *const> args(argv, kThreeArgs);
    uint8_t state = 0U;
    if (!cli::parse_u8(args[1], state) || (state > kAlarmStateMax)) {
        return cli::fail("state 0-2");
    }
    uint8_t level = 0U;
    if (!cli::parse_u8(args[2], level) || (level > kAlarmLevelMax)) {
        return cli::fail("level 0-3");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kAlarmSet);
    command.args[0] = state;
    command.args[1] = level;
    command.arg_len = 2U;
    return submit(command, "alarm set") ? 0 : cli::kCmdFail;
}

int cmd_alarm_clear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: alarm clear");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kAlarmClear);
    return submit(command, "alarm clear") ? 0 : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 3U> kBtVerbs{{
    {"list", nullptr, "list active BT sessions and pairing state", cmd_bt_list},
    {"pair", "[sec]", "open 60 s pairing window", cmd_bt_pair},
    {"policy", nullptr, "show link policy", cmd_bt_policy},
}};

constexpr std::array<cli::Verb, 4U> kWlVerbs{{
    {"list", nullptr, "list paired alarm whitelist", cmd_wl_list},
    {"add", "<MAC>", "add alarm MAC (not supported yet)", cmd_wl_add},
    {"del", "<MAC>", "remove alarm MAC (not supported yet)", cmd_wl_del},
    {"clear", nullptr, "clear alarm whitelist", cmd_wl_clear},
}};

constexpr std::array<cli::Verb, 3U> kAlarmVerbs{{
    {"show", nullptr, "show alarm state and latest heartbeat", cmd_alarm_show},
    {"set", "<state> <level>", "set alarm override (state 0-2, level 0-3)", cmd_alarm_set},
    {"clear", nullptr, "clear alarm override", cmd_alarm_clear},
}};

int cmd_bt(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kBtVerbs, ctx, argc, argv);
}

int cmd_wl(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kWlVerbs, ctx, argc, argv);
}

int cmd_alarm(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kAlarmVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 3U> kLinkCmds{{
    {"bt", "bt <list|pair|policy>: BT sessions, pairing and policy", "<list|pair [sec]|policy>", cmd_bt, nullptr},
    {"wl", "wl <list|add|del|clear>: alarm whitelist control", "<list|add <MAC>|del <MAC>|clear>", cmd_wl, nullptr},
    {"alarm", "alarm <show|set|clear>: alarm state and override", "<show|set <state> <level>|clear>", cmd_alarm,
     nullptr},
}};

} // namespace

std::span<const cli::Cmd> link_cmds() noexcept
{
    return kLinkCmds;
}

std::span<const cli::Verb> bt_verbs() noexcept
{
    return kBtVerbs;
}

std::span<const cli::Verb> wl_verbs() noexcept
{
    return kWlVerbs;
}

std::span<const cli::Verb> alarm_verbs() noexcept
{
    return kAlarmVerbs;
}

} // namespace app
