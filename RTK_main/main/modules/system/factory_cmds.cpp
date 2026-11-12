// 隐藏的 factory 命令集（ADR-0009 §2.10）：改生产配置或强制状态的动作（SN、白名单、配对窗口、报警覆盖）。
// 不出现在 help 列表；除 status/unlock/lock 外必须先 `factory unlock <key>`（Kconfig RTK_FACTORY_KEY），
// 解锁状态只在内存中，重启或 `factory lock` 后恢复锁定。
// 线程：仅在 console REPL 任务调用（串行），解锁标志无需同步；改状态的动作经 Executor::call 投递。

#include "modules/system/system_module.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "sdkconfig.h"

#include "app_context.hpp"
#include "cli/out.hpp"
#include "modules/storage/storage_module.hpp"
#include "support/console_support.hpp"

namespace app {

namespace {

constexpr uint8_t kAlarmStateMax = 2U;
constexpr uint8_t kAlarmLevelMax = 3U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr size_t kTwoArgs = 2U;
constexpr size_t kThreeArgs = 3U;

bool s_unlocked = false; // 只在 console 任务读写

/** 受保护动词的统一入口检查；锁定时给出原因并返回 false。 */
bool require_unlocked() noexcept
{
    if (!s_unlocked) {
        static_cast<void>(cli::fail("locked (factory unlock <key>)"));
        return false;
    }
    return true;
}

int cmd_status(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: factory status");
    }
    cli::print_section("factory");
    cli::print_kv("access", "%s", s_unlocked ? "unlocked" : "locked");
    return 0;
}

int cmd_unlock(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kTwoArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory unlock <key>");
    }
    const std::span<char *const> args(argv, kTwoArgs);
    if (std::strcmp(args[1], CONFIG_RTK_FACTORY_KEY) != 0) {
        s_unlocked = false;
        return cli::fail("bad key");
    }
    s_unlocked = true;
    cli::out_printf("factory commands unlocked until reboot or 'factory lock'\n");
    return 0;
}

int cmd_lock(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: factory lock");
    }
    s_unlocked = false;
    return 0;
}

int cmd_sn(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (!require_unlocked()) {
        return cli::kCmdFail;
    }
    if ((argc != static_cast<int>(kTwoArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory sn <value>");
    }
    const std::span<char *const> args(argv, kTwoArgs);
    const size_t len = std::strlen(args[1]);
    if ((len == 0U) || (len > NVS_STORE_SN_MAX) || (std::strpbrk(args[1], "\r\n") != nullptr)) {
        return cli::fail("bad sn (1-%u bytes)", static_cast<unsigned>(NVS_STORE_SN_MAX));
    }
    fw::Command command{};
    command.target = id(ModuleKind::kStorage);
    command.verb = verb(StorageVerb::kSaveSn);
    // len 已校验在 1..NVS_STORE_SN_MAX(16) 范围，缩窄到 uint8_t 无损。
    command.arg_len = static_cast<uint8_t>(len);
    std::memcpy(command.args.data(), args[1], len);
    if (!submit(command, "factory sn")) {
        return cli::kCmdFail;
    }
    cli::out_printf("sn saved; reboot to apply\n");
    return 0;
}

int wl_edit(bool add, int argc, char **argv) noexcept
{
    const char *const op = add ? "wl-add" : "wl-del";
    if (!require_unlocked()) {
        return cli::kCmdFail;
    }
    if ((argc != static_cast<int>(kTwoArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory %s <MAC>", op);
    }
    const std::span<char *const> args(argv, kTwoArgs);
    std::array<uint8_t, cli::kMacLen> mac{};
    if (!cli::parse_mac(args[1], mac)) {
        return cli::fail("bad MAC");
    }
    static_assert(cli::kMacLen == core::kAddrLen, "MAC length mismatch");
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(add ? LinkVerb::kWhitelistAdd : LinkVerb::kWhitelistDel);
    std::copy(mac.begin(), mac.end(), command.args.begin());
    command.arg_len = static_cast<uint8_t>(core::kAddrLen);
    fw::CommandResult result{};
    if (!submit_raw(command, result)) {
        return cli::kCmdFail;
    }
    switch (result.status) {
    case fw::Result::kInvalidState:
        return cli::fail("already in whitelist");
    case fw::Result::kNotFound:
        return cli::fail("not in whitelist");
    case fw::Result::kNoSpace:
        return cli::fail("whitelist full (%u)", static_cast<unsigned>(kMaxAlarms));
    default:
        break;
    }
    return report(op, result) ? 0 : cli::kCmdFail;
}

int cmd_wl_add(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wl_edit(true, argc, argv);
}

int cmd_wl_del(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wl_edit(false, argc, argv);
}

int cmd_wl_clear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!require_unlocked()) {
        return cli::kCmdFail;
    }
    if (argc != 1) {
        return cli::fail("usage: factory wl-clear");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kWhitelistClear);
    return submit(command, "wl-clear") ? 0 : cli::kCmdFail;
}

int cmd_pair(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!require_unlocked()) {
        return cli::kCmdFail;
    }
    if (argc != 1) {
        return cli::fail("usage: factory pair");
    }
    // LinkManager::pair_open 接口冻结为固定窗口（kPairWindowMs）。
    constexpr uint32_t kPairSeconds = core::kPairWindowMs / kMsPerSecond;
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
    if (!report("pair", result)) {
        return cli::kCmdFail;
    }
    cli::out_printf("pairing window open for %u s\n", static_cast<unsigned>(kPairSeconds));
    return 0;
}

int cmd_alarm_set(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (!require_unlocked()) {
        return cli::kCmdFail;
    }
    if ((argc != static_cast<int>(kThreeArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory alarm-set <state 0-2> <level 0-3>");
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
    return submit(command, "alarm-set") ? 0 : cli::kCmdFail;
}

int cmd_alarm_clear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!require_unlocked()) {
        return cli::kCmdFail;
    }
    if (argc != 1) {
        return cli::fail("usage: factory alarm-clear");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kAlarmClear);
    return submit(command, "alarm-clear") ? 0 : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 10U> kFactoryVerbs{{
    {"status", nullptr, "show lock state", cmd_status},
    {"unlock", "<key>", "unlock factory commands until reboot", cmd_unlock},
    {"lock", nullptr, "lock factory commands", cmd_lock},
    {"sn", "<value>", "write device SN (1-16 chars, reboot to apply)", cmd_sn},
    {"wl-add", "<MAC>", "add alarm MAC to whitelist", cmd_wl_add},
    {"wl-del", "<MAC>", "remove alarm MAC from whitelist", cmd_wl_del},
    {"wl-clear", nullptr, "clear alarm whitelist", cmd_wl_clear},
    {"pair", nullptr, "open the alarm pairing window", cmd_pair},
    {"alarm-set", "<state> <level>", "force alarm state (0-2) and level (0-3)", cmd_alarm_set},
    {"alarm-clear", nullptr, "clear forced alarm state", cmd_alarm_clear},
}};

int cmd_factory(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kFactoryVerbs, ctx, argc, argv, "status");
}

constexpr std::array<cli::Cmd, 1U> kFactoryCmds{{
    {"factory", "factory <verb>: production settings (hidden, needs unlock)", "<verb>", cmd_factory, nullptr, true},
}};

} // namespace

std::span<const cli::Cmd> factory_cmds() noexcept
{
    return kFactoryCmds;
}

std::span<const cli::Verb> factory_verbs() noexcept
{
    return kFactoryVerbs;
}

} // namespace app
