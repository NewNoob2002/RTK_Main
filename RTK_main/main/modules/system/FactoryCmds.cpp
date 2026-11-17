// 隐藏的 factory 命令集（ADR-0009 §2.10）：改生产配置或强制状态的动作（SN、白名单、配对窗口、报警覆盖）。
// 不出现在 help 列表；除 status/unlock/lock 外必须先 `factory unlock <key>`（Kconfig RTK_FACTORY_KEY），
// 解锁状态只在内存中，重启或 `factory lock` 后恢复锁定。
// 线程：仅在 console REPL 任务调用（串行），解锁标志无需同步；改状态的动作经 Executor::call 投递。

#include "modules/system/SystemModule.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "sdkconfig.h"

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "modules/storage/StorageModule.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

constexpr uint8_t kAlarmStateMax = 2U;
constexpr uint8_t kAlarmLevelMax = 3U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr size_t kTwoArgs = 2U;
constexpr size_t kThreeArgs = 3U;

bool s_unlocked = false; // 只在 console 任务读写

/** 受保护动词的统一入口检查；锁定时给出原因并返回 false。 */
bool requireUnlocked() noexcept
{
    if (!s_unlocked) {
        static_cast<void>(cli::fail("locked (factory unlock <key>)"));
        return false;
    }
    return true;
}

int cmdStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: factory status");
    }
    cli::printSection("factory");
    cli::printKv("access", "%s", s_unlocked ? "unlocked" : "locked");
    return 0;
}

int cmdUnlock(void * /*ctx*/, int argc, char **argv) noexcept
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
    cli::outPrintf("factory commands unlocked until reboot or 'factory lock'\n");
    return 0;
}

int cmdLock(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: factory lock");
    }
    s_unlocked = false;
    return 0;
}

int cmdSn(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (!requireUnlocked()) {
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
    command.argLen = static_cast<uint8_t>(len);
    std::memcpy(command.args.data(), args[1], len);
    if (!submit(command, "factory sn")) {
        return cli::kCmdFail;
    }
    cli::outPrintf("sn saved; reboot to apply\n");
    return 0;
}

int wlEdit(bool add, int argc, char **argv) noexcept
{
    const char *const op = add ? "wl-add" : "wl-del";
    if (!requireUnlocked()) {
        return cli::kCmdFail;
    }
    if ((argc != static_cast<int>(kTwoArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory %s <MAC>", op);
    }
    const std::span<char *const> args(argv, kTwoArgs);
    std::array<uint8_t, cli::kMacLen> mac{};
    if (!cli::parseMac(args[1], mac)) {
        return cli::fail("bad MAC");
    }
    static_assert(cli::kMacLen == core::kAddrLen, "MAC length mismatch");
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(add ? LinkVerb::kWhitelistAdd : LinkVerb::kWhitelistDel);
    std::copy(mac.begin(), mac.end(), command.args.begin());
    command.argLen = static_cast<uint8_t>(core::kAddrLen);
    fw::CommandResult result{};
    if (!submitRaw(command, result)) {
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

// 命令表项：签名由 cli::Cmd 固定；添加/删除白名单由同一个 wlEdit 的常量参数区分。
int cmdWlAdd(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wlEdit(true, argc, argv);
}

int cmdWlDel(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wlEdit(false, argc, argv);
}

int cmdWlClear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!requireUnlocked()) {
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

int cmdPair(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!requireUnlocked()) {
        return cli::kCmdFail;
    }
    if (argc != 1) {
        return cli::fail("usage: factory pair");
    }
    // LinkManager::pairOpen 接口冻结为固定窗口（kPairWindowMs）。
    constexpr uint32_t kPairSeconds = core::kPairWindowMs / kMsPerSecond;
    if (appContext().data.linkStatus.read().pairing) {
        return cli::fail("pairing busy");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kPair);
    fw::CommandResult result{};
    if (!submitRaw(command, result)) {
        return cli::kCmdFail;
    }
    if (result.status == fw::Result::kBusy) {
        return cli::fail("pairing busy");
    }
    if (!report("pair", result)) {
        return cli::kCmdFail;
    }
    cli::outPrintf("pairing window open for %u s\n", static_cast<unsigned>(kPairSeconds));
    return 0;
}

int cmdAlarmSet(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (!requireUnlocked()) {
        return cli::kCmdFail;
    }
    if ((argc != static_cast<int>(kThreeArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory alarm-set <state 0-2> <level 0-3>");
    }
    const std::span<char *const> args(argv, kThreeArgs);
    uint8_t state = 0U;
    if (!cli::parseU8(args[1], state) || (state > kAlarmStateMax)) {
        return cli::fail("state 0-2");
    }
    uint8_t level = 0U;
    if (!cli::parseU8(args[2], level) || (level > kAlarmLevelMax)) {
        return cli::fail("level 0-3");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kBtLink);
    command.verb = verb(LinkVerb::kAlarmSet);
    command.args[0] = state;
    command.args[1] = level;
    command.argLen = 2U;
    return submit(command, "alarm-set") ? 0 : cli::kCmdFail;
}

int cmdAlarmClear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!requireUnlocked()) {
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
    {"status", nullptr, "show lock state", cmdStatus},
    {"unlock", "<key>", "unlock factory commands until reboot", cmdUnlock},
    {"lock", nullptr, "lock factory commands", cmdLock},
    {"sn", "<value>", "write device SN (1-16 chars, reboot to apply)", cmdSn},
    {"wl-add", "<MAC>", "add alarm MAC to whitelist", cmdWlAdd},
    {"wl-del", "<MAC>", "remove alarm MAC from whitelist", cmdWlDel},
    {"wl-clear", nullptr, "clear alarm whitelist", cmdWlClear},
    {"pair", nullptr, "open the alarm pairing window", cmdPair},
    {"alarm-set", "<state> <level>", "force alarm state (0-2) and level (0-3)", cmdAlarmSet},
    {"alarm-clear", nullptr, "clear forced alarm state", cmdAlarmClear},
}};

// 命令表项：签名由 cli::Cmd 的处理器类型固定；默认动词 "status" 在此注入。
int cmdFactory(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kFactoryVerbs, ctx, argc, argv, "status");
}

constexpr std::array<cli::Cmd, 1U> kFactoryCmds{{
    {"factory", "factory <verb>: production settings (hidden, needs unlock)", "<verb>", cmdFactory,
     nullptr, true},
}};

} // namespace

std::span<const cli::Cmd> factoryCmds() noexcept
{
    return kFactoryCmds;
}

std::span<const cli::Verb> factoryVerbs() noexcept
{
    return kFactoryVerbs;
}

} // namespace app
