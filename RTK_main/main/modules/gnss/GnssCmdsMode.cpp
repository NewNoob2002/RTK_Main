// GNSS 工作模式与基站参数控制台命令：gnss mode / gnss base / gnss out rtcm（ADR-0014 §2.5/§2.6）。
// 仅在 console REPL 任务调用，可写 NVS 与等执行器结论（有界等待），非 ISR。
#include "modules/gnss/GnssCmdsPrivate.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <span>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "def/GnssSubDef.hpp"
#include "modules/gnss/GnssBaseConfig.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/gnss/GnssModule.hpp"
#include "support/ConsoleSupport.hpp"

namespace app::detail {
namespace {

constexpr uint32_t kModePollMs = 50U;
constexpr uint32_t kModeWaitRounds = 120U; // 最长等 6 s 让控制器确认首步切换结果
constexpr uint16_t kMaxSvyToMarginS = 3600U;
constexpr int64_t kCmScale = 100LL;
constexpr uint8_t kLatLonDecimals = 9U;
constexpr uint8_t kHeightDecimals = 3U;
constexpr uint8_t kAccDecimals = 2U;
constexpr uint8_t kLatLonTextBytes = 24U;
constexpr uint8_t kMetersTextBytes = 20U;
constexpr uint8_t kRateTextBytes = 16U;

constexpr size_t kSurveyMinArgs = 4U;
constexpr size_t kSurveyMaxArgs = 5U;
constexpr size_t kFixedArgs = 5U;
constexpr size_t kRtcmDefaultArgs = 3U;
constexpr size_t kRtcmSetArgs = 4U;
constexpr size_t kArgSub = 2U;
constexpr size_t kArgVal = 3U;
constexpr size_t kArgExtra = 4U;

/**
 * @brief 保存 gnss.base 全部键到 NVS。
 * @param cfg: 已修改的配置快照。
 * @retval true: 保存成功。
 * @retval false: 写入失败（已打印 ERR）。
 */
bool persistBaseKeys(const GnssBaseConfig &cfg) noexcept
{
    appContext().gnss.writeBaseConfig(cfg);
    if (!fw::isOk(appContext().storage.saveKeys(gnss_store::kBaseKeys))) {
        (void)cli::fail("failed to save gnss.base settings");
        return false;
    }
    return true;
}

/**
 * @brief 若当前处于基站配置/运行状态，打印下次切换生效提示。
 * @param cfg: 当前 gnss.base 配置。
 */
void printApplyHint(const GnssBaseConfig &cfg) noexcept
{
    if ((cfg.mode == WorkModeKind::kBase) || appContext().data.gnssWorkMode.read().active) {
        cli::outPrintf("note: saved to NVS; use 'gnss mode base' to apply\n");
    }
}

/**
 * @brief 打印当前工作模式与实时状态（无参 gnss mode）。
 */
void printModeStatus() noexcept
{
    const auto cfg = appContext().gnss.readBaseConfig();
    const auto wm = appContext().data.gnssWorkMode.read();
    cli::printSection("gnss_mode");
    cli::printKv("config", "%s", workModeName(cfg.mode));
    cli::printKv("state", "%s", workModeStateName(wm.state));
    cli::printKv("base_type", "%s", baseTypeName(cfg.baseType));
    cli::printKv("elapsed", "%us", static_cast<unsigned>(wm.elapsedS));
    if (wm.hAccCm != ::gnss::kU16Unknown) {
        cli::printKv("hAcc", "%u cm", static_cast<unsigned>(wm.hAccCm));
    } else {
        cli::printKv("hAcc", "-");
    }
    if (wm.lastErr != WorkModeErr::kNone) {
        cli::printKv("last_err", "%s", workModeErrName(wm.lastErr));
    }
}

/**
 * @brief 打印当前基站配置详情（无参 gnss base）。
 * @param cfg: 当前 gnss.base 配置。
 */
void printBaseSummary(const GnssBaseConfig &cfg) noexcept
{
    std::array<char, kLatLonTextBytes> latBuf{};
    std::array<char, kLatLonTextBytes> lonBuf{};
    std::array<char, kMetersTextBytes> hBuf{};
    std::array<char, kMetersTextBytes> accBuf{};
    cli::printSection("gnss_base");
    cli::printKv("mode", "%s", workModeName(cfg.mode));
    cli::printKv("base_type", "%s", baseTypeName(cfg.baseType));
    cli::printKv("survey", "min=%us acc=%sm timeout=%us", static_cast<unsigned>(cfg.svyS),
                 formatScaledInt(cfg.svyAccCm, kAccDecimals, accBuf),
                 static_cast<unsigned>(cfg.svyToS));
    cli::printKv("fixed", "lat=%s lon=%s h=%sm",
                 formatScaledInt(cfg.fixLatE9, kLatLonDecimals, latBuf),
                 formatScaledInt(cfg.fixLonE9, kLatLonDecimals, lonBuf),
                 formatScaledInt(cfg.fixHeightMm, kHeightDecimals, hBuf));
    cli::printKv("msm", "%u", static_cast<unsigned>(cfg.msm));
}

/**
 * @brief 解析并更新 gnss base survey <s> <m> [timeout_s]。
 * @param args: 命令行参数切片。
 * @param cfg: 读入并更新的 gnss.base 配置。
 * @retval 0: 成功；非零: 已输出 ERR。
 */
int handleBaseSurvey(std::span<char *const> args, GnssBaseConfig &cfg) noexcept
{
    if ((args.size() != kSurveyMinArgs) && (args.size() != kSurveyMaxArgs)) {
        return cli::fail("usage: gnss base survey <s 5..3600> <m 0.1..10.0> [timeout_s]");
    }
    uint32_t svyS = 0U;
    if (!cli::parseU32(args[kArgSub], svyS) || (svyS < GnssBaseConfig::kMinSvyS) ||
        (svyS > GnssBaseConfig::kMaxSvyS)) {
        return cli::fail("invalid survey duration (5..3600)");
    }
    int64_t accCmSigned = 0;
    if (!parseScaledDecimal(std::string_view{args[kArgVal]}, kAccDecimals, accCmSigned) ||
        (accCmSigned < GnssBaseConfig::kMinSvyAccCm) ||
        (accCmSigned > GnssBaseConfig::kMaxSvyAccCm)) {
        return cli::fail("invalid survey accuracy (0.1..10.0 m)");
    }
    uint32_t svyToS = cfg.svyToS;
    if (args.size() == kSurveyMaxArgs) {
        if (!cli::parseU32(args[kArgExtra], svyToS)) {
            return cli::fail("invalid timeout_s");
        }
    }
    if ((svyToS < (svyS + GnssBaseConfig::kMinSvyToMarginS)) || (svyToS > kMaxSvyToMarginS)) {
        return cli::fail("invalid timeout_s (must be >= s+10 and <= 3600)");
    }
    cfg.baseType = BaseType::kSurvey;
    cfg.svyS = static_cast<uint16_t>(svyS);
    cfg.svyAccCm = static_cast<uint16_t>(accCmSigned);
    cfg.svyToS = static_cast<uint16_t>(svyToS);
    if (!persistBaseKeys(cfg)) {
        return cli::kCmdFail;
    }
    cli::outPrintf("survey=%us acc=%u.%02um timeout=%us\n", static_cast<unsigned>(cfg.svyS),
                   static_cast<unsigned>(cfg.svyAccCm / static_cast<uint16_t>(kCmScale)),
                   static_cast<unsigned>(cfg.svyAccCm % static_cast<uint16_t>(kCmScale)),
                   static_cast<unsigned>(cfg.svyToS));
    printApplyHint(cfg);
    return 0;
}

/**
 * @brief 解析并更新 gnss base fixed <lat> <lon> <h>。
 * @param args: 命令行参数切片。
 * @param cfg: 读入并更新的 gnss.base 配置。
 * @retval 0: 成功；非零: 已输出 ERR。
 */
int handleBaseFixed(std::span<char *const> args, GnssBaseConfig &cfg) noexcept
{
    if (args.size() != kFixedArgs) {
        return cli::fail("usage: gnss base fixed <lat> <lon> <h>");
    }
    int64_t latE9 = 0;
    int64_t lonE9 = 0;
    int64_t hMm = 0;
    if (!parseScaledDecimal(std::string_view{args[kArgSub]}, kLatLonDecimals, latE9) ||
        (latE9 < -GnssBaseConfig::kMaxLatE9) || (latE9 > GnssBaseConfig::kMaxLatE9)) {
        return cli::fail("invalid latitude (-90..90)");
    }
    if (!parseScaledDecimal(std::string_view{args[kArgVal]}, kLatLonDecimals, lonE9) ||
        (lonE9 < -GnssBaseConfig::kMaxLonE9) || (lonE9 > GnssBaseConfig::kMaxLonE9)) {
        return cli::fail("invalid longitude (-180..180)");
    }
    if (!parseScaledDecimal(std::string_view{args[kArgExtra]}, kHeightDecimals, hMm) ||
        (hMm < GnssBaseConfig::kMinHeightMm) || (hMm > GnssBaseConfig::kMaxHeightMm)) {
        return cli::fail("invalid height (-1000..100000 m)");
    }
    cfg.baseType = BaseType::kFixed;
    cfg.fixLatE9 = latE9;
    cfg.fixLonE9 = lonE9;
    cfg.fixHeightMm = static_cast<int32_t>(hMm);
    if (!persistBaseKeys(cfg)) {
        return cli::kCmdFail;
    }
    printApplyHint(cfg);
    return 0;
}

/**
 * @brief 等待 gnss mode 首阶段切换确认并持久化 NVS mode。
 * @param target: 目标模式。
 * @param beforeSeq: 下发前 WorkMode 序号。
 * @retval 0: 成功并已保存 NVS；非零: 拒绝/超时（已输出 ERR）。
 */
int waitAndPersistMode(WorkModeKind target, uint32_t beforeSeq) noexcept
{
    for (uint32_t round = 0U; round < kModeWaitRounds; ++round) {
        const auto cur = appContext().data.gnssWorkMode.read();
        if ((cur.seq != beforeSeq) && (cur.state != WorkModeState::kBaseConfig) &&
            (cur.state != WorkModeState::kFallback)) {
            if (cur.lastErr != WorkModeErr::kNone) {
                return cli::fail("%s", workModeErrName(cur.lastErr));
            }
            auto cfg = appContext().gnss.readBaseConfig();
            cfg.mode = target;
            if (!persistBaseKeys(cfg)) {
                return cli::kCmdFail;
            }
            cli::outPrintf("mode=%s state=%s\n", workModeName(target),
                           workModeStateName(cur.state));
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(kModePollMs));
    }
    return cli::fail("timeout");
}

/**
 * @brief 打印当前基站 RTCM 输出计划与支持矩阵。
 * @param cfg: 当前 gnss.base 配置。
 */
void printBaseRtcmShow(const GnssBaseConfig &cfg) noexcept
{
    cli::printSection("gnss_out_rtcm");
    cli::printKv("msm", "%u", static_cast<unsigned>(cfg.msm));
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        std::array<char, kRateTextBytes> rateBuf{};
        cli::outPrintf("  RTCM%-5u %s\n", static_cast<unsigned>(::gnss::kBaseRtcmIds[i]),
                       formatRtcmRate(cfg.rtcmOut[i], rateBuf));
    }
}

} // namespace

/**
 * @brief 控制台切换或查询 GNSS 工作模式（rover/base/smartbase）。
 * @param ctx: 未使用上下文指针。
 * @param argc: 参数个数。
 * @param argv: 参数向量。
 * @retval 0: 命令成功；非零: 已输出 ERR。
 */
int cmdGnssMode(void * /*ctx*/, int argc, char **argv) noexcept
{
    if (argc == 1) {
        printModeStatus();
        return 0;
    }
    if ((argc != 2) || (argv == nullptr)) {
        return cli::fail("usage: gnss mode [rover|base|smartbase]");
    }
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    const auto target = parseWorkMode(std::string_view{args[1U]});
    if (!target.has_value()) {
        return cli::fail("invalid mode (rover|base|smartbase)");
    }
    if (*target == WorkModeKind::kSmartBase) {
        return cli::fail("not supported yet");
    }
    if (appContext().ioExecutor == nullptr) {
        return cli::fail("gnss not running");
    }

    const auto beforeWm = appContext().data.gnssWorkMode.read();
    const auto ctrlState = appContext().data.gnssStatus.read().state;
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kMode);
    cmd.argLen = 2U;
    cmd.args[0U] = static_cast<uint8_t>(*target);
    cmd.args[1U] = 1U;
    fw::CommandResult res{};
    if (!submitRawTo(*appContext().ioExecutor, ModuleKind::kGnss, "gnss not running", cmd, res)) {
        return cli::kCmdFail;
    }
    if (res.status == fw::Result::kNotSupported) {
        return cli::fail("not supported yet");
    }
    if (!fw::isOk(res.status)) {
        return cli::fail("gnss mode failed (%s)", fw::toString(res.status));
    }

    if (ctrlState != ::gnss::State::kRunning) {
        auto cfg = appContext().gnss.readBaseConfig();
        cfg.mode = *target;
        if (!persistBaseKeys(cfg)) {
            return cli::kCmdFail;
        }
        cli::outPrintf("mode=%s queued (waiting gnss running)\n", workModeName(*target));
        return 0;
    }
    return waitAndPersistMode(*target, beforeWm.seq);
}

/**
 * @brief 控制台查询或配置基站参数（survey/fixed）。
 * @param ctx: 未使用上下文指针。
 * @param argc: 参数个数。
 * @param argv: 参数向量。
 * @retval 0: 命令成功；非零: 已输出 ERR。
 */
int cmdGnssBase(void * /*ctx*/, int argc, char **argv) noexcept
{
    auto cfg = appContext().gnss.readBaseConfig();
    if (argc == 1) {
        printBaseSummary(cfg);
        return 0;
    }
    if ((argv == nullptr) || (argc < 2)) {
        return cli::fail("usage: gnss base [survey <s> <m> [timeout_s]|fixed <lat> <lon> <h>]");
    }
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    const std::string_view sub{args[1U]};
    if (sub == "survey") {
        return handleBaseSurvey(args, cfg);
    }
    if (sub == "fixed") {
        return handleBaseFixed(args, cfg);
    }
    return cli::fail("usage: gnss base [survey <s> <m> [timeout_s]|fixed <lat> <lon> <h>]");
}

/**
 * @brief 控制台查询或配置基站 RTCM 输出速率及 MSM 版本。
 * @param args: 命令行参数切片（含动词）。
 * @retval 0: 命令成功；非零: 已输出 ERR。
 */
int cmdGnssOutRtcm(std::span<char *const> args) noexcept
{
    auto cfg = appContext().gnss.readBaseConfig();
    if ((args.size() == 2U) ||
        ((args.size() == kRtcmDefaultArgs) && (std::string_view{args[kArgSub]} == "show"))) {
        printBaseRtcmShow(cfg);
        return 0;
    }
    if ((args.size() == kRtcmDefaultArgs) && (std::string_view{args[kArgSub]} == "default")) {
        cfg.msm = GnssBaseConfig::kMsm4;
        cfg.rtcmOut = GnssBaseConfig::defaultRtcmOut(cfg.msm);
        if (!persistBaseKeys(cfg)) {
            return cli::kCmdFail;
        }
        printBaseRtcmShow(cfg);
        printApplyHint(cfg);
        return 0;
    }
    if ((args.size() == kRtcmSetArgs) && (std::string_view{args[kArgSub]} == "msm")) {
        uint8_t ver = 0U;
        if (!cli::parseU8(args[kArgVal], ver) ||
            ((ver != GnssBaseConfig::kMsm4) && (ver != GnssBaseConfig::kMsm7))) {
            return cli::fail("invalid msm version (4|7)");
        }
        cfg.applyMsm(ver);
        if (!persistBaseKeys(cfg)) {
            return cli::kCmdFail;
        }
        cli::outPrintf("msm=%u\n", static_cast<unsigned>(cfg.msm));
        printApplyHint(cfg);
        return 0;
    }
    if (args.size() == kRtcmSetArgs) {
        uint32_t id = 0U;
        if (!cli::parseU32(args[kArgSub], id) || (id > UINT16_MAX)) {
            return cli::fail("invalid rtcm message id");
        }
        const auto slot = GnssBaseConfig::rtcmSlot(static_cast<uint16_t>(id));
        if (!slot.has_value()) {
            return cli::fail("unsupported rtcm id");
        }
        int8_t code = 0;
        if (!parseRtcmRateArg(std::string_view{args[kArgVal]}, code)) {
            return cli::fail("invalid rate (off|0.1|0.2|0.5|1|2|5)");
        }
        cfg.rtcmOut[*slot] = code;
        if (!persistBaseKeys(cfg)) {
            return cli::kCmdFail;
        }
        printApplyHint(cfg);
        return 0;
    }
    return cli::fail("usage: gnss out rtcm [show|default|msm <4|7>|<id> <hz|off>]");
}

} // namespace app::detail
