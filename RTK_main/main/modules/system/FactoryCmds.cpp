// 隐藏工厂命令：SN、白名单、配对窗口与报警覆盖（ADR-0009 §2.10）。
// 仅 console REPL 任务，非 ISR；状态变更经执行器投递，可阻塞。
// 除 status/unlock/lock 外须解锁；标志仅在内存中，重启或 lock 恢复锁定；本文件不分配堆。

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
#include "modules/bt_link/BtPairing.hpp"
#include "modules/system/FactoryReset.hpp"
#include "modules/storage/StorageModule.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

constexpr uint8_t kAlarmStateMax = 2U;
constexpr uint8_t kAlarmLevelMax = 3U;
constexpr size_t kTwoArgs = 2U;
constexpr size_t kThreeArgs = 3U;

bool s_unlocked = false; // 只在 console 任务读写

/**
 * @brief 受保护动词的统一入口检查；锁定时给出原因并返回 false。
 * @retval true 工厂入口已解锁
 * @retval false 尚未解锁，已打印错误
 * @note 仅 console REPL 任务，非 ISR；输出可阻塞。
 */
bool requireUnlocked() noexcept
{
    if (!s_unlocked) {
        static_cast<void>(cli::fail("locked (factory unlock <key>)"));
        return false;
    }
    return true;
}

/**
 * @brief 显示工厂入口的解锁状态与当前身份
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: factory status");
    }
    cli::printSection("factory");
    cli::printKv("access", "%s", s_unlocked ? "unlocked" : "locked");
    return 0;
}

/**
 * @brief 校验口令并解锁工厂入口
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
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

/**
 * @brief 锁定工厂入口
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdLock(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: factory lock");
    }
    s_unlocked = false;
    return 0;
}

/**
 * @brief 查询或保存设备序列号
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdSn(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (!requireUnlocked()) {
        return cli::kCmdFail;
    }
    if ((argc != static_cast<int>(kTwoArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory sn <value>");
    }
    const std::span<char *const> args(argv, kTwoArgs);
    const std::string_view sn{args[1]};
    constexpr uint8_t kMinVisibleAscii = 0x21U;
    constexpr uint8_t kMaxVisibleAscii = 0x7EU;
    bool validSn = !sn.empty() && (sn.size() <= NVS_STORE_SN_MAX);
    for (const char raw : sn) {
        const auto ch = static_cast<uint8_t>(raw);
        if ((ch < kMinVisibleAscii) || (ch > kMaxVisibleAscii)) {
            validSn = false;
            break;
        }
    }
    if (!validSn) {
        return cli::fail("bad sn (1-%u bytes)", static_cast<unsigned>(NVS_STORE_SN_MAX));
    }
    fw::CommandResult result{};
    settings::Error err = settings::Error::kOk;
    if (!submitSetting("factory.sn", sn, s_unlocked, kCommandTimeoutMs, result, err)) {
        if (err == settings::Error::kTimeout) {
            return cli::fail("no response");
        }
        if (err == settings::Error::kUnsupported) {
            return cli::fail("settings unavailable");
        }
        if (err == settings::Error::kLocked) {
            return cli::fail("locked (factory unlock <key>)");
        }
        if (result.status != fw::Result::kOk) {
            static_cast<void>(report("factory sn", result));
            return cli::kCmdFail;
        }
        return cli::fail("invalid argument");
    }
    cli::outPrintf("sn saved; reboot to apply\n");
    return 0;
}

/**
 * @brief 校验地址并提交白名单增删命令
 * @param add: true 添加；false 删除
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval cli::kCmdFail 参数非法、未解锁或提交失败
 * @note 仅 console REPL 任务，可等待执行器/NVS，非 ISR。
 */
int wlEdit(bool add, int argc, char **argv) noexcept
{
    const char *const op = add ? "wl-add" : "wl-del";
    if (!requireBluetooth() || !requireUnlocked()) {
        return cli::kCmdFail;
    }
    if ((argc != static_cast<int>(kTwoArgs)) || (argv == nullptr)) {
        return cli::fail("usage: factory %s <MAC>", op);
    }
    const std::span<char *const> args(argv, kTwoArgs);
    return editTrustedAlarm(add, args[1]);
}

/**
 * @brief 提交白名单添加命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 * @note 命令表项：签名由 cli::Cmd 固定；添加/删除白名单由同一个 wlEdit 的常量参数区分。
 */
int cmdWlAdd(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wlEdit(true, argc, argv);
}

/**
 * @brief 提交白名单删除命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdWlDel(void * /*ctx*/, int argc, char **argv) noexcept
{
    return wlEdit(false, argc, argv);
}

/**
 * @brief 提交白名单清空命令
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdWlClear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!requireBluetooth() || !requireUnlocked()) {
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

/**
 * @brief 查询或设置配对窗口
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdPair(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!requireBluetooth() || !requireUnlocked()) {
        return cli::kCmdFail;
    }
    if (argc != 1) {
        return cli::fail("usage: factory pair");
    }
    return openDefaultPairing();
}

/**
 * @brief 提交报警状态设置命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdAlarmSet(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (!requireBluetooth() || !requireUnlocked()) {
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

/**
 * @brief 提交报警清除命令
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdAlarmClear(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!requireBluetooth() || !requireUnlocked()) {
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

/** @brief 执行已解锁且显式确认的全分区重置。 @param argc: 参数数。 @param argv: 参数。 @retval 0 成功。 */
int cmdErase(void *, int argc, char **argv) noexcept
{
    return requireUnlocked() ? confirmFactoryReset(argc, argv, true) : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 11U> kFactoryVerbs{{
    {"status", nullptr, "show lock state", cmdStatus},
    {"erase", "yes", "erase user and factory NVS, then restart", cmdErase},
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

/**
 * @brief 分派工厂设置子命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 * @note 命令表项：签名由 cli::Cmd 的处理器类型固定；默认动词 "status" 在此注入。
 */
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

bool factoryUnlocked() noexcept
{
    return s_unlocked;
}

} // namespace app
