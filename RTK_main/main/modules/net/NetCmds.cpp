// 4G 联网模块控制台命令组（ADR-0021 / ADR-0016）：net status（所有档位）；DEV 档 net power/reset/at。
// 仅在 console REPL 任务调用，非 ISR；不分配堆。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/net/NetModule.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "app/ConsoleProfile.hpp"
#include "board/BoardProfile.hpp"
#include "cellular/Policy.hpp"
#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "def/NetStatusText.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {
namespace {

constexpr uint32_t kAtPollMs = 20U; // ms，AT 响应轮询间隔
constexpr uint32_t kAtWaitMs =
    cellular::at_timeout::kDebugDefault + 1000U; // ms，含 kNet 取走与应答余量
/**
 * @brief 连接槽状态名
 * @param state: ConnSnapshot::state（LinkFsm::ConnState 数值）
 * @retval 状态名字符串（静态）
 * @note 纯函数，线程/ISR 安全。
 */
const char *connStateName(uint8_t state) noexcept
{
    // 与 cellular::LinkFsm::ConnState 数值顺序一致。
    static constexpr std::array<const char *, 6U> kNames{"idle", "want_open", "opening",
                                                         "open", "backoff",   "closing"};
    return (state < kNames.size()) ? kNames[state] : "?";
}

/**
 * @brief 显示 4G 链路状态（所有档位）；ICCID 打码，APN 之外的凭据不输出
 * @param argc: 参数个数（含动词）
 * @retval 0 已输出状态
 * @retval 非零 参数错误
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
int cmdNetStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: net status");
    }
    const auto &self = appContext();
    const NetConfigView cfg = self.data.netConfig.read();
    const cellular::LinkSnapshot snap = self.data.netStatus.read();
    cli::printSection("net");
    cli::printKv("config", "enable=%u apn=\"%s\" user=%s pass=%s%s", cfg.enable ? 1U : 0U,
                 cfg.apn.data(), cfg.user[0] != '\0' ? "set" : "empty",
                 cfg.passSet ? "set" : "empty", cfg.restartPending ? " (restart pending)" : "");
    if (!self.net.present()) {
        if (!board::hasFeature(board::current(), board::Feature::kCellular)) {
            cli::printKv("present", "no (board without cellular)");
        } else if (!cfg.enable) {
            cli::printKv("present", "no (disabled; set net.enable=1 and reboot)");
        } else {
            cli::printKv("present", "no (not started; reboot to apply, or radio mode excludes 4G)");
        }
        return 0;
    }
    std::array<char, cellular::kIccidBytes> masked{};
    net_status::maskIccid(snap.iccid, masked);
    cli::printKv("phase", "%s", net_status::phaseName(snap.phase));
    cli::printKv("sim", "%s", net_status::simName(snap.sim));
    cli::printKv("imei", "%s", snap.imei[0] != '\0' ? snap.imei.data() : "n/a");
    cli::printKv("iccid", "%s", masked[0] != '\0' ? masked.data() : "n/a");
    if (snap.csq == cellular::kCsqUnknown) {
        cli::printKv("csq", "n/a");
    } else {
        cli::printKv("csq", "%u", static_cast<unsigned>(snap.csq));
    }
    cli::printKv("reg", "%s", net_status::regName(snap.reg));
    cli::printKv("pdp", "%s", snap.pdpUp ? "up" : "down");
    cli::printKv("link", "baud=%" PRIu32 " channel=%s", snap.baud,
                 snap.channelDead ? "dead" : "ok");
    cli::printKv("recovery", "level=L%u count=%u backoff_ms=%" PRIu32,
                 static_cast<unsigned>(snap.level), static_cast<unsigned>(snap.recoveryCount),
                 snap.backoffRemainingMs);
    for (size_t i = 0U; i < snap.conn.size(); ++i) {
        std::array<char, 8U> key{};
        static_cast<void>(snprintf(key.data(), key.size(), "conn%u", static_cast<unsigned>(i)));
        cli::printKv(key.data(), "state=%s fails=%u remain_ms=%" PRIu32,
                     connStateName(snap.conn[i].state), static_cast<unsigned>(snap.conn[i].fails),
                     snap.conn[i].backoffRemainingMs);
    }
    cli::printKv("recv_dropped", "%u", static_cast<unsigned>(snap.droppedRecv));
    return 0;
}

/**
 * @brief 向 kNet 投递一条无参命令并报告结果
 * @param verbId: NetVerb 命令号
 * @param what: 失败提示用的动作名
 * @retval 0 命令已执行
 * @retval 非零 档位不允许、执行器不可用或命令失败，已输出 ERR
 * @note 仅 console REPL 任务，最多等待 kCommandTimeoutMs，非 ISR。
 */
int postNet(NetVerb verbId, const char *what) noexcept
{
    if constexpr (kConsoleProfile != ConsoleProfile::kDev) {
        return cli::fail("%s: DEV console only", what);
    } else {
        auto *executor = appContext().netExecutor;
        if (executor == nullptr || !appContext().net.present()) {
            return cli::fail("%s: net module not running (set net.enable=1 and reboot)", what);
        }
        fw::Command command{};
        command.target = id(ModuleKind::kNet);
        command.verb = verb(verbId);
        fw::CommandResult result{};
        if (!submitRawTo(*executor, ModuleKind::kNet, "net not running", command, result)) {
            return cli::kCmdFail;
        }
        if (result.status != fw::Result::kOk) {
            return cli::fail("%s failed: status=%d", what, static_cast<int>(result.status));
        }
        return 0;
    }
}

/**
 * @brief net power on|off：DEV 强制上电或优雅关机并保持关闭
 * @param argc: 参数个数（含动词）
 * @param argv: 参数数组（<on|off>）
 * @retval 0 已投递
 * @retval 非零 参数错误或命令失败
 * @note 仅 console REPL 任务，非 ISR。
 */
int cmdNetPower(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (argc != 2 || argv == nullptr) {
        return cli::fail("usage: net power <on|off>");
    }
    const std::span<char *> args(argv, static_cast<size_t>(argc));
    if (args[1] == nullptr) {
        return cli::fail("usage: net power <on|off>");
    }
    const std::string_view arg{args[1]};
    if (arg == "on") {
        return postNet(NetVerb::kPowerOn, "net power on");
    }
    if (arg == "off") {
        return postNet(NetVerb::kPowerOff, "net power off");
    }
    return cli::fail("usage: net power <on|off>");
}

/**
 * @brief net reset：手动触发 L3 模块级复位（断电保持后重新上电）
 * @param argc: 参数个数（含动词）
 * @retval 0 已投递
 * @retval 非零 参数错误或命令失败
 * @note 仅 console REPL 任务，非 ISR。
 */
int cmdNetReset(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: net reset");
    }
    return postNet(NetVerb::kReset, "net reset");
}

/**
 * @brief 把 argv[1..] 以空格拼成 AT 命令文本
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @param[out] out: 目标缓冲（含结尾 0）
 * @retval true 拼接成功
 * @retval false 文本超出缓冲
 * @note 纯函数，线程/ISR 安全。
 */
bool joinArgs(int argc, char *const *argv, std::array<char, kNetAtCmdBytes> &out) noexcept
{
    size_t len = 0U;
    const std::span<char *const> args(argv, static_cast<size_t>(argc));
    for (size_t i = 1U; i < args.size(); ++i) {
        const std::string_view part{args[i]};
        const size_t need = part.size() + (i > 1U ? 1U : 0U);
        if (len + need >= out.size()) {
            return false;
        }
        if (i > 1U) {
            out[len++] = ' ';
        }
        std::memcpy(std::span(out).subspan(len).data(), part.data(), part.size());
        len += part.size();
    }
    out[len] = '\0';
    return len > 0U;
}

/**
 * @brief AT 提交失败原因文本
 * @param status: submitAt 的失败状态
 * @retval 静态字符串
 */
const char *submitErrorText(fw::Status status) noexcept
{
    if (status == fw::Status::kBusy) {
        return "busy";
    }
    return (status == fw::Status::kInvalidArg) ? "command rejected" : "module not powered";
}

/**
 * @brief 轮询等待 AT 邮箱完成，超时则释放邮箱
 * @param net: NetModule
 * @retval true 已完成；false 超时（已 releaseAt）
 * @note 仅 console REPL 任务，最长阻塞 kAtWaitMs，非 ISR。
 */
bool waitAtDone(NetModule &net) noexcept
{
    uint32_t waited = 0U;
    while (net.atState() != NetModule::AtState::kDone) {
        if (waited >= kAtWaitMs) {
            net.releaseAt();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kAtPollMs));
        waited += kAtPollMs;
    }
    return true;
}

/**
 * @brief net at <cmd>：DEV AT 透传（拒绝 ATE1/AT+IPR/AT+QISWTMD），打印模块应答
 * @param argc: 参数个数（含动词）
 * @param argv: 参数数组（<AT 命令...>，含引号的命令用 \" 转义）
 * @retval 0 模块返回 OK
 * @retval 非零 档位不允许、禁止命令、忙、超时或模块返回错误
 * @note 仅 console REPL 任务，最长阻塞 kAtWaitMs，非 ISR。
 */
int cmdNetAt(void * /*ctx*/, int argc, char **argv) noexcept
{
    if constexpr (kConsoleProfile != ConsoleProfile::kDev) {
        return cli::fail("net at: DEV console only");
    } else {
        if (argc < 2 || argv == nullptr) {
            return cli::fail("usage: net at <AT command>");
        }
        auto &net = appContext().net;
        if (!net.present()) {
            return cli::fail("net at: net module not running (set net.enable=1 and reboot)");
        }
        std::array<char, kNetAtCmdBytes> text{};
        const std::span<char *> args(argv, static_cast<size_t>(argc));
        if (!joinArgs(argc, args.data(), text)) {
            return cli::fail("net at: command too long (max %u)",
                             static_cast<unsigned>(kNetAtCmdBytes - 1U));
        }
        const fw::Status submitted = net.submitAt(text.data());
        if (!fw::isOk(submitted)) {
            return cli::fail("net at: %s", submitErrorText(submitted));
        }
        if (!waitAtDone(net)) {
            return cli::fail("net at: timeout");
        }
        bool ok = false;
        const std::string_view rsp = net.atResponse(ok);
        if (!rsp.empty()) {
            cli::outPrintf("%.*s", static_cast<int>(rsp.size()), rsp.data());
        }
        net.releaseAt();
        return ok ? 0 : cli::fail("net at: module returned error");
    }
}

constexpr cli::Verb kNetStatusVerb{
    "status", nullptr, "show 4G link phase, SIM, signal, registration and recovery", &cmdNetStatus};

// DEV 档才含调试动词；PRODUCTION 档 help/usage 只暴露 status，避免公布不可用命令。
constexpr std::array<cli::Verb, 4U> kNetVerbsDev{{
    kNetStatusVerb,
    {"power", "<on|off>", "DEV: force 4G module power on, or graceful off and stay off",
     &cmdNetPower},
    {"reset", nullptr, "DEV: manual L3 module power-cycle", &cmdNetReset},
    {"at", "<AT cmd>",
     "DEV: send AT command (echo/baud/mode/APN-profile/data commands rejected; escape quotes as \\\")",
     &cmdNetAt},
}};
constexpr std::array<cli::Verb, 1U> kNetVerbsProd{{kNetStatusVerb}};

/**
 * @brief 分派 net 子命令，缺省显示 net status
 * @param ctx: 控制台上下文
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 子命令成功
 * @retval 非零 参数或执行错误
 * @note 仅 console REPL 任务，非 ISR。
 */
int cmdNet(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(netVerbs(), ctx, argc, argv, "status");
}

constexpr std::array<cli::Cmd, 1U> kNetCmds{{
    {"net",
     kConsoleProfile == ConsoleProfile::kDev
         ? "net [status|power <on|off>|reset|at <AT cmd>]: 4G link status and DEV tools"
         : "net [status]: 4G link status",
     kConsoleProfile == ConsoleProfile::kDev ? "[status|power <on|off>|reset|at <AT cmd>]"
                                             : "[status]",
     &cmdNet, nullptr},
}};

} // namespace

std::span<const cli::Cmd> netCmds() noexcept
{
    return kNetCmds;
}

std::span<const cli::Verb> netVerbs() noexcept
{
    if constexpr (kConsoleProfile == ConsoleProfile::kDev) {
        return kNetVerbsDev;
    } else {
        return kNetVerbsProd;
    }
}

} // namespace app
