// gnss 命令组（ADR-0010 §2.7）：gnss status|fix|sats|probe|reset|rate <hz>|send <text>。
// 仅在 console REPL 任务调用，非 ISR；只读命令读 DataCenter Slot，改状态命令经 ioExecutor::call 投递。

#include "modules/gnss/GnssModule.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "board/BoardProfile.hpp"
#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "data/GnssOutput.hpp"
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"
#include "modules/gnss/GnssCmdPack.hpp"
#include "modules/gnss/GnssCmdsPrivate.hpp"
#include "modules/gnss/GnssConsoleValue.hpp"
#include "rtos/EspStatus.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

using detail::kAppMsgNames;

namespace {

/** 等待变更判定的轮询间隔与上限：须长于 GnssModule 的 kApplyTimeoutMs（5 s，频率重配含基础脚本）。 */
constexpr uint32_t kApplyPollMs = 50U;
constexpr uint32_t kApplyPollRounds = 120U;
constexpr int kOutSetArgc = 3;

/**
 * @brief 解析语句名或 all
 * @param name: 语句名或 all
 * @param outIdx: 单类下标输出；all 或失败时不改
 * @param isAll: 是否为 all；失败时为 false
 * @retval true 名称有效
 * @retval false 名称未知，outIdx 不变
 * @note 仅 console REPL 任务，不阻塞，非 ISR。
 */
bool parseMsgIndex(std::string_view name, uint8_t &outIdx, bool &isAll) noexcept
{
    isAll = false;
    if (name == "gsv") {
        name = "sats";
    }
    if (name == "all") {
        isAll = true;
        return true;
    }
    for (size_t i = 0U; i < kAppMsgNames.size(); ++i) {
        if (name == std::string_view{kAppMsgNames[i]}) {
            outIdx = static_cast<uint8_t>(i);
            return true;
        }
    }
    return false;
}

/**
 * @brief 把 GNSS 执行结果转成控制台诊断
 * @param res: 执行结果
 * @retval true 状态为 kOk
 * @retval false 执行失败，已打印错误
 * @note 仅 console REPL 任务；输出可阻塞，非 ISR。
 */
bool reportGnssResult(const fw::CommandResult &res) noexcept
{
    switch (res.status) {
    case fw::Result::kOk:
        return true;
    case fw::Result::kInvalidArg:
        (void)cli::fail("invalid argument");
        return false;
    case fw::Result::kInvalidState:
        (void)cli::fail("gnss not ready");
        return false;
    case fw::Result::kBusy:
        (void)cli::fail("gnss busy");
        return false;
    case fw::Result::kIoError:
        (void)cli::fail("gnss io error: %s", esp_err_to_name(res.detail));
        return false;
    default:
        (void)cli::fail("gnss error %u", static_cast<unsigned>(res.status));
        return false;
    }
}

bool submitGnss(const fw::Command &cmd) noexcept;

/**
 * @brief 打印本轮输出计划的逐项结果；成功返回 true。
 * @param before: 提交前状态，用于识别输出关闭
 * @param after: 提交后输出计划与应用结果
 * @retval true 本轮没有失败语句
 * @retval false 至少一条语句失败
 * @note 仅 console REPL 任务；输出可阻塞，非 ISR。
 */
bool reportOutputPlan(const GnssStatus &before, const GnssStatus &after) noexcept
{
    bool ok = true;
    for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
        const uint32_t bit = 1U << i;
        const char *state = "off";
        if ((after.unsupportedMask & bit) != 0U) {
            state = "unsupported";
        } else if ((after.failedMask & bit) != 0U) {
            state = "failed";
            ok = false;
        } else if ((((after.appliedMask | before.appliedMask) & bit) != 0U) &&
                   (after.rxPeriodMs[i] != 0U)) {
            // applied 位也包含已 ACK 的关闭命令（UNLOG），是否在输出看计划周期。
            constexpr uint32_t kMsPerSecond = 1000U;
            const uint32_t periodMs = after.rxPeriodMs[i];
            // 计划来自 int8 间隔或定位频率，频率或整秒周期可还原为 int8。
            const auto interval =
                periodMs <= kMsPerSecond
                    ? static_cast<int8_t>(kMsPerSecond / periodMs)
                    : static_cast<int8_t>(-static_cast<int32_t>(periodMs / kMsPerSecond));
            std::array<char, detail::kGnssRateTextBytes> rate{};
            detail::formatGnssHz(interval, rate);
            cli::outPrintf("  %-8s on %s\n", kAppMsgNames[i], rate.data());
            continue;
        }
        cli::outPrintf("  %-8s %s\n", kAppMsgNames[i], state);
    }
    return ok;
}

/**
 * @brief 提交命令并等待两阶段提交判定（applySeq 前进），再按 lastApply 给 OK/ERR。 仅在 console REPL 任务调用；阻塞轮询，最长 6 s（不阻塞 kIo）。
 * @param cmd: 待处理指令
 * @retval true 提交、应用与落盘均成功
 * @retval false 未运行、提交失败、被拒、落盘失败或等待超时
 * @note 仅 console REPL 任务；提交后最多轮询 6 s，不阻塞 kIo，非 ISR。
 */
bool submitGnssAndWait(const fw::Command &cmd) noexcept
{
    const GnssStatus before = appContext().data.gnssStatus.read();
    if (!submitGnss(cmd)) {
        return false;
    }
    for (uint32_t round = 0U; round < kApplyPollRounds; ++round) {
        const GnssStatus after = appContext().data.gnssStatus.read();
        if (after.applySeq != before.applySeq) {
            if (after.lastApply == GnssApply::kRestored) {
                (void)cli::fail("receiver rejected output change, reverted");
                return false;
            }
            if (after.lastApply != GnssApply::kOk) {
                (void)cli::fail("output applied but settings save failed");
                return false;
            }
            return reportOutputPlan(before, after);
        }
        vTaskDelay(pdMS_TO_TICKS(kApplyPollMs));
    }
    (void)cli::fail("gnss output apply timeout");
    return false;
}

/**
 * @brief 向 IO 执行器提交 GNSS 命令并打印失败原因
 * @param cmd: 待处理指令
 * @retval true 已提交且命令返回 kOk
 * @retval false 执行器不可用、提交失败或命令失败
 * @note 仅 console REPL 任务；等待执行器结果，非 ISR。
 */
bool submitGnss(const fw::Command &cmd) noexcept
{
    if (appContext().ioExecutor == nullptr) {
        (void)cli::fail("gnss not running");
        return false;
    }
    fw::CommandResult res{};
    if (!submitRawTo(*appContext().ioExecutor, ModuleKind::kGnss, "gnss not running", cmd, res)) {
        return false;
    }
    return reportGnssResult(res);
}

/**
 * @brief 提交人工 GNSS 重探命令
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdGnssProbe(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss probe");
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kProbe);
    return submitGnss(cmd) ? 0 : cli::kCmdFail;
}

/**
 * @brief 提交 GNSS 控制器重启命令
 * @param argc: 命令参数个数（含动词）
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdGnssReset(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss reset");
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kReset);
    return submitGnss(cmd) ? 0 : cli::kCmdFail;
}

/**
 * @brief 校验并提交 GNSS 定位频率变更
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdGnssRate(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != 2) || (argv == nullptr)) {
        return cli::fail("usage: gnss rate <1|2|5|10|20>");
    }
    const std::span<char *const> args{argv, 2U};
    uint8_t hz = 0U;
    if (!cli::parseU8(args[1], hz)) {
        return cli::fail("invalid rate: %s", args[1]);
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kSetRate);
    cmd.argLen = 1U;
    cmd.args[0] = hz;
    return submitGnssAndWait(cmd) ? 0 : cli::kCmdFail;
}

/**
 * @brief 拼接并提交原始 GNSS 命令文本
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdGnssSend(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 2) || (argv == nullptr)) {
        return cli::fail("usage: gnss send <text>");
    }
    // argc 已验证为正；转换为 size_t 不改变数值。
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kSend);
    size_t pos = 0U;
    for (size_t i = 1U; i < args.size(); ++i) {
        const size_t partLen = std::strlen(args[i]);
        const size_t need = (i > 1) ? (partLen + 1U) : partLen;
        if (need > (cmd.args.size() - pos)) {
            return cli::fail("command too long (max %u)", static_cast<unsigned>(cmd.args.size()));
        }
        if (i > 1) {
            cmd.args[pos] = static_cast<uint8_t>(' ');
            ++pos;
        }
        std::memcpy(std::span{cmd.args}.subspan(pos, partLen).data(), args[i], partLen);
        pos += partLen;
    }
    if (pos == 0U) {
        return cli::fail("empty command");
    }
    // 按字符查看命令字节。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *command = reinterpret_cast<const char *>(cmd.args.data());
    const std::string_view text{command, pos};
    if (detail::changesConnectedGnssBaud(text, board::gnssReceiverPort())) {
        return cli::fail("use gnss baud for the connected port");
    }
    cmd.argLen = static_cast<uint8_t>(pos);
    return submitGnss(cmd) ? 0 : cli::kCmdFail;
}

/**
 * @brief 打印当前默认输出配置与接收机落地状态（`gnss out` / `gnss out show`）
 * @param cfg: GNSS 输出配置
 * @note 仅 console REPL 任务；配置与状态各一次快照。
 */
void printGnssOutShow(const data::GnssOutConfig &cfg) noexcept
{
    cli::printSection("gnss_out");
    cli::printKv("mask", "0x%02X", static_cast<unsigned>(cfg.mask));
    const GnssStatus st = appContext().data.gnssStatus.read();
    cli::outPrintf("  msg      rate          receiver\n");
    for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
        const bool en = (cfg.mask & (1U << i)) != 0U;
        std::array<char, detail::kGnssRateTextBytes> rate{};
        if (en && data::followsFixRate(cfg, i)) {
            // 默认间隔的定位类语句按 `gnss rate` 下发，显示数值会与实际不符。
            std::snprintf(rate.data(), rate.size(), "fix rate");
        } else {
            detail::formatGnssHz(en ? cfg.intervals[i] : int8_t{0}, rate);
        }
        uint32_t bit = 1U << i;
        constexpr uint32_t kSatsBit = 1U << static_cast<uint32_t>(::gnss::OutputMsg::kSatsInfo);
        constexpr uint32_t kGsvBit = 1U << static_cast<uint32_t>(::gnss::OutputMsg::kGsv);
        if (bit == kSatsBit && (st.unsupportedMask & kSatsBit) != 0U) {
            bit = kGsvBit;
        }
        const char *applied = "-";
        if ((st.appliedMask & bit) != 0U) {
            applied = en ? "on" : "off";
        }
        if ((st.unsupportedMask & bit) != 0U) {
            applied = "unsupported";
        } else if ((st.failedMask & bit) != 0U) {
            applied = "failed";
        }
        cli::outPrintf("  %-8s %-13s %s\n", kAppMsgNames[i], rate.data(), applied);
    }
}

/**
 * @brief 解析 `gnss out` 的写入形式并更新配置（default 或 <msg|all> <Hz|off>）
 * @param args: 完整参数（含命令名）
 * @param cfg: 输出，更新后的配置
 * @return 0: 成功；其他: 已打印 ERR 的失败码
 * @note 仅 console REPL 任务；纯解析，不访问模块。
 */
int applyGnssOutArgs(std::span<char *const> args, data::GnssOutConfig &cfg) noexcept
{
    if ((args.size() == 2U) && (std::string_view{args[1]} == "default")) {
        cfg = data::GnssOutConfig::defaults();
    } else if (args.size() == static_cast<size_t>(kOutSetArgc)) {
        uint8_t idx = 0U;
        bool isAll = false;
        if (!parseMsgIndex(std::string_view{args[1]}, idx, isAll)) {
            return cli::fail("invalid msg (gga|sats|heading|gst|rmc|gsa|all)");
        }
        const std::string_view argVal{args[2]};
        int8_t iv = 0;
        if (!detail::parseGnssHz(argVal, iv)) {
            return cli::fail("invalid rate (Hz: 1/2/5/10/20 or 1/n)");
        }
        // +1 是旧配置中随定位频率的缺省值，显式 1 Hz 用等价的 1 秒周期表示。
        if (iv == 1) {
            iv = -1;
        }
        const size_t begin = isAll ? 0U : idx;
        const size_t end = isAll ? data::kGnssAppMsgCount : (static_cast<size_t>(idx) + 1U);
        for (size_t i = begin; i < end; ++i) {
            if (!fw::isOk(cfg.applyInterval(i, iv))) {
                return cli::fail("unsupported interval");
            }
        }
    } else {
        return cli::fail("usage: gnss out [show|default|<msg|all> <Hz|off>]");
    }

    return 0;
}

/**
 * @brief 把输出配置编码成 kSetOut 指令并等待模块结论
 * @param cfg: 待下发的配置
 * @return 0: 成功；其他: cli::kCmdFail
 * @note 仅 console REPL 任务，阻塞等待（最长 6 s）。
 */
int submitGnssOut(const data::GnssOutConfig &cfg) noexcept
{
    bool ok = false;
    const fw::Command cmd = detail::packGnssOut(cfg, ok);
    if (!ok) {
        return cli::fail("encode out config failed");
    }
    return submitGnssAndWait(cmd) ? 0 : cli::kCmdFail;
}

/**
 * @brief 查询或设置 GNSS 输出配置
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 表示成功
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
int cmdGnssOut(void * /*ctx*/, int argc, char **argv) noexcept
{
    const std::span<char *const> args{(argv != nullptr) ? argv : nullptr,
                                      ((argv != nullptr) && (argc > 0)) ? static_cast<size_t>(argc)
                                                                        : 0U};
    if ((args.size() >= 2U) && (std::string_view{args[1]} == "rtcm")) {
        return detail::cmdGnssOutRtcm(args);
    }
    auto cfg = appContext().data.gnssOutCfg.read();
    if ((args.size() <= 1U) || ((args.size() == 2U) && (std::string_view{args[1]} == "show"))) {
        printGnssOutShow(cfg);
        return 0;
    }
    const int rc = applyGnssOutArgs(args, cfg);
    if (rc != 0) {
        return rc;
    }
    return submitGnssOut(cfg);
}

/**
 * @brief 打印一次读回结论（ERR not supported / ERR busy / 文本 + lines + OK）
 * @param rb: 已发布的读回结论
 * @return 0: 打印成功（由分发器补上 OK）；其他: cli::kCmdFail
 */
int printReadback(const GnssReadback &rb) noexcept
{
    if (!rb.supported) {
        return cli::fail("not supported");
    }
    if (rb.busy) {
        return cli::fail("busy");
    }
    if (rb.len != 0U) {
        // 文本为若干 "...\n" 行，不以 '\0' 结尾，因此按长度整块写出。
        cli::outWrite(appContext().data.gnssReadbackText.data(), rb.len);
    }
    cli::outPrintf("lines=%u%s\n", static_cast<unsigned>(rb.lines),
                   rb.truncated ? " truncated" : "");
    return 0;
}

/**
 * @brief `gnss config`：读回模块当前配置并逐行打印（ADR-0012 §2.7）
 * @param argc: 参数个数（只允许 1）
 * @param argv: 参数向量
 * @return 0: 成功；其他: cli::kCmdFail（ERR 原因已打印）
 * @note 仅 console REPL 任务，阻塞等待模块结论，最长 6 s。
 */
int cmdGnssConfig(void * /*ctx*/, int argc, char **argv) noexcept
{
    const std::span<char *const> args{(argv != nullptr) ? argv : nullptr,
                                      ((argv != nullptr) && (argc > 0)) ? static_cast<size_t>(argc)
                                                                        : 0U};
    if (args.size() > 1U) {
        return cli::fail("usage: gnss config");
    }
    if (appContext().ioExecutor == nullptr) {
        return cli::fail("gnss not running");
    }
    const uint32_t before = appContext().data.gnssReadback.read().seq;
    const fw::Command cmd = detail::packGnssReadback();
    fw::CommandResult res{};
    if (!submitRawTo(*appContext().ioExecutor, ModuleKind::kGnss, "gnss not running", cmd, res)) {
        return cli::kCmdFail;
    }
    // 模块在 onCommand 里同步发布被拒结论，成功则等读回窗口收尾（≤ 5 s，这里给 6 s 余量）。
    for (uint32_t round = 0U; round < kApplyPollRounds; ++round) {
        const GnssReadback rb = appContext().data.gnssReadback.read();
        if (rb.seq != before) {
            return printReadback(rb);
        }
        vTaskDelay(pdMS_TO_TICKS(kApplyPollMs));
    }
    return cli::fail("timeout");
}

// clang-format off
constexpr std::array<cli::Verb, 14U> kGnssVerbs{{
    {"status", nullptr, "GNSS status, stats and pipe", &cmdGnssStatus},
    {"fix", nullptr, "latest position/quality fix", &cmdGnssFix},
    {"sats", nullptr, "visible satellites table", &cmdGnssSats},
    {"probe", nullptr, "re-probe without power cycle", &cmdGnssProbe},
    {"reset", nullptr, "power cycle and re-probe", &cmdGnssReset},
    {"rate", "<hz>", "shortcut: position messages rate (see gnss out)", &cmdGnssRate},
    {"out", "[show|default|rtcm|<msg|all> <val>]", "set/show receiver & base RTCM output rates", &cmdGnssOut},
    {"mode", "[rover|base|smartbase]", "show or switch work mode", &detail::cmdGnssMode},
    {"base", "[survey|fixed]", "show or configure base station parameters", &detail::cmdGnssBase},
    {"config", nullptr, "read back receiver config", &cmdGnssConfig},
    {"baud", "<bps>", "change connected port and ESP32 UART together, store in ESP32 NVS", &detail::cmdGnssBaud},
    {"save", nullptr, "manually persist the current receiver configuration", &detail::cmdGnssSave},
    {"freset", "yes", "reset receiver and GNSS settings", &detail::cmdGnssFactoryReset},
    {"send", "<text>", "configure other COM ports, then gnss save; use gnss baud for the connected port", &cmdGnssSend},
}};
// clang-format on

/**
 * @brief 分派 GNSS 子命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 * @note 命令表项：签名由 cli::Cmd 的处理器类型固定；缺省动词 "status" 在此注入。
 */
int cmdGnss(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kGnssVerbs, ctx, argc, argv, "status");
}

constexpr std::array<cli::Cmd, 1U> kGnssCmds{{
    {
        .name = "gnss",
        .help = "GNSS receiver control and telemetry",
        .hint = "[status|fix|sats|probe|reset|rate|out|config|send]",
        .handler = &cmdGnss,
        .ctx = nullptr,
    },
}};

} // namespace

std::span<const cli::Cmd> gnssCmds() noexcept
{
    return kGnssCmds;
}

std::span<const cli::Verb> gnssVerbs() noexcept
{
    return kGnssVerbs;
}

} // namespace app
