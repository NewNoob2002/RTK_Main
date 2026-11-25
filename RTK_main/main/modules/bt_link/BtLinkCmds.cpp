// 蓝牙状态、配对窗口与报警器白名单控制台。
// 仅 console REPL 任务，非 ISR。变更经 kLink 或 SPP worker 执行。

#include "modules/bt_link/BtLinkModule.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "AppContext.hpp"
#include "SppServerPairing.h"
#include "modules/bt_link/BtPairing.hpp"
#include "rtos/EspStatus.hpp"
#include "cli/Out.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

/**
 * @brief 显示蓝牙会话与链路状态
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int btStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: bt");
    }
    const auto &data = appContext().data;
    const auto sessions = data.sessions.read();
    const auto link = data.linkStatus.read();
    const auto sys = data.systemStatus.read();

    cli::printSection("bt");
    cli::printKv("stack", "%s", sys.btReady ? "ready" : "down");
    cli::printKv("link", "%s", sys.linkRunning ? "running" : "stopped");
    cli::printKv("name", "%s", sys.deviceName.data());
    // 会话数上限为 kMaxSessions，转换为 unsigned 无损。
    cli::printKv("sessions", "%u / %u", static_cast<unsigned>(sessions.count),
                 static_cast<unsigned>(kMaxSessions));
    std::array<char, kMacStrBuf> macBuf{};
    for (size_t idx = 0U; idx < sessions.count; ++idx) {
        const SessionSnapshot &session = sessions.entries[idx];
        formatMac(session.addr, macBuf);
        cli::printKv("session", "%-6s %s (handle 0x%08" PRIx32 ")", roleName(session.role),
                     macBuf.data(), session.handle);
    }
    if (link.pairing) {
        cli::printKv("pairing", "open");
    }
    return 0;
}

/**
 * @brief 显示报警状态与最近心跳
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdAlarm(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (!requireBluetooth()) {
        return cli::kCmdFail;
    }
    if (argc != 1) {
        return cli::fail("usage: alarm");
    }
    const auto &data = appContext().data;
    const auto alarm = data.alarmState.read();
    const auto heartbeat = data.heartbeat.read();

    cli::printSection("alarm");
    // 报警状态/级别均为 uint8_t，转换为 unsigned 无损。
    cli::printKv("state", "%u", static_cast<unsigned>(alarm.state));
    cli::printKv("level", "%u", static_cast<unsigned>(alarm.level));
    cli::printKv("override", "%s", alarm.override ? "yes" : "no");
    if (heartbeat.present) {
        cli::printKv("heartbeat", "seq %u, battery %u%%, state %u",
                     static_cast<unsigned>(heartbeat.value.seq),
                     static_cast<unsigned>(heartbeat.value.batteryPct),
                     static_cast<unsigned>(heartbeat.value.localState));
    } else {
        cli::printKv("heartbeat", "none");
    }
    return 0;
}

/**
 * @brief 向链路执行器投递配对窗口变更。
 * @param seconds: 开窗秒数，0 关闭。
 * @param confirm: 是否逐个确认。
 * @retval 0 成功；其他已输出 ERR。
 */
int setPairing(uint32_t seconds, bool confirm = false) noexcept
{
    const auto cmd = makePairingCommand(seconds, confirm);
    if (!submit(cmd, "pair")) {
        return cli::kCmdFail;
    }
    if (seconds == 0U) {
        cli::outPrintf("pairing window closed\n");
    } else {
        cli::outPrintf("pairing window open for %u s%s\n", static_cast<unsigned>(seconds),
                       confirm ? " (confirm)" : "");
    }
    return 0;
}

/**
 * @brief 显示窗口状态，或解析开窗、关闭和回答配对请求。
 * @param argc: 参数数。
 * @param argv: 参数数组。
 * @retval 0 成功；其他已输出 ERR。
 */
int cmdPair(void *, int argc, char **argv) noexcept
{
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    const auto tail = args.subspan(1U);
    if (tail.empty()) {
        const auto window = appContext().bt_link.pairStatus(rtos::nowMs());
        if (window.remainingMs == 0U) {
            cli::outPrintf("pairing window closed\n");
        } else {
            const auto seconds = (window.remainingMs + kPairMsPerSecond - 1U) / kPairMsPerSecond;
            cli::outPrintf("pairing window open, %u s remaining%s\n",
                           static_cast<unsigned>(seconds), window.confirm ? " (confirm)" : "");
        }
        return 0;
    }
    const std::string_view verb{tail.front()};
    if (verb == "open") {
        auto options = tail.subspan(1U);
        const bool confirm = !options.empty() && std::string_view{options.back()} == "confirm";
        if (confirm) {
            options = options.first(options.size() - 1U);
        }
        uint32_t seconds = core::kPairWindowMs / kPairMsPerSecond;
        if (options.size() > 1U || (!options.empty() && !cli::parseU32(options.front(), seconds)) ||
            seconds < kPairMinSeconds || seconds > kPairMaxSeconds) {
            return cli::fail("usage: bt pair open [10..600] [confirm]");
        }
        return setPairing(seconds, confirm);
    }
    if (tail.size() == 1U && verb == "close") {
        return setPairing(0U);
    }
    if (tail.size() == 1U && (verb == "accept" || verb == "reject")) {
        return spp_server_pair_decide(verb == "accept") ? 0 : cli::fail("no pending pair request");
    }
    return cli::fail("usage: bt pair [open [s] [confirm]|close|accept|reject]");
}

/** @brief 添加报警器信任地址。 @param argc: 参数数。 @param argv: 参数。 @retval 0 成功。 */
int cmdTrust(void *, int argc, char **argv) noexcept
{
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    return args.size() == 2U ? editTrustedAlarm(true, args.back())
                             : cli::fail("usage: bt trust <MAC>");
}

/** @brief 删除报警器信任地址。 @param argc: 参数数。 @param argv: 参数。 @retval 0 成功。 */
int cmdUntrust(void *, int argc, char **argv) noexcept
{
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    return args.size() == 2U ? editTrustedAlarm(false, args.back())
                             : cli::fail("usage: bt untrust <MAC>");
}

constexpr std::array<cli::Verb, 4U> kBtVerbs{{
    {"status", nullptr, "show BT stack and sessions", btStatus},
    {"pair", "[open [s] [confirm]|close|accept|reject]", "manage pairing authorization", cmdPair},
    {"trust", "<MAC>", "add alarm to whitelist", cmdTrust},
    {"untrust", "<MAC>", "remove alarm from whitelist", cmdUntrust},
}};

/** @brief 分派蓝牙动词。 @param ctx: 控制台上下文。 @param argc: 参数数。 @param argv: 参数。 @retval 0 成功。 */
int cmdBt(void *ctx, int argc, char **argv) noexcept
{
    return requireBluetooth() ? cli::dispatch(kBtVerbs, ctx, argc, argv, "status") : cli::kCmdFail;
}

constexpr std::array<cli::Cmd, 2U> kLinkCmds{{
    {"bt", "bt [status|pair|trust|untrust]: Bluetooth and pairing", "[verb]", cmdBt, nullptr,
     false},
    {"alarm", "alarm: alarm state and latest heartbeat", nullptr, cmdAlarm, nullptr, false},
}};

} // namespace

int openDefaultPairing() noexcept
{
    return setPairing(core::kPairWindowMs / kPairMsPerSecond);
}

int editTrustedAlarm(bool add, const char *macText) noexcept
{
    std::array<uint8_t, cli::kMacLen> mac{};
    if (!cli::parseMac(macText, mac)) {
        return cli::fail("bad MAC");
    }
    static_assert(cli::kMacLen == core::kAddrLen);
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kBtLink);
    cmd.verb = verb(add ? LinkVerb::kWhitelistAdd : LinkVerb::kWhitelistDel);
    std::ranges::copy(mac, cmd.args.begin());
    cmd.argLen = core::kAddrLen;
    fw::CommandResult result{};
    if (!submitRaw(cmd, result)) {
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
        return report("whitelist", result) ? 0 : cli::kCmdFail;
    }
}

std::span<const cli::Verb> btVerbs() noexcept
{
    return kBtVerbs;
}

std::span<const cli::Cmd> linkCmds() noexcept
{
    return kLinkCmds;
}

} // namespace app
