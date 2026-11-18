// Unicore 流动站配置脚本：命令生成（cmd_*）与脚本步骤（step_*）（ADR-0010 §2.4），只写一条完整命令/一步脚本，不关心发送与应答等待。
// 线程安全：是（纯函数，无全局可变状态）。ISR 安全：是（无锁、不阻塞）。
// 堆与阻塞：不使用堆（命令写入调用方的 CmdBuf）、不阻塞；不使用浮点（频率参数按手册写死为文本）。
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "gnss/drivers/UnicoreReceiver.hpp"

#include "../AsciiCmd.hpp"
#include "UnicoreKeys.hpp"

namespace gnss {
namespace {

using unicore::kConfigTimeoutMs;

constexpr uint8_t kMinUnicorePort = 1U;
constexpr uint8_t kMaxUnicorePort = 4U;
constexpr uint64_t kE9Scale = 1000000000ULL;
constexpr uint64_t kMmScale = 1000ULL;
constexpr uint8_t kLatLonDecimals = 9U;
constexpr uint8_t kHeightDecimals = 3U;

// Unicore 支持的基站 RTCM 报文下标（跳过 kBaseRtcmIds[3] = 1230）。
constexpr std::array<size_t, 11U> kUnicoreRtcmIndices{
    0U, 1U, 2U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U,
};

/**
 * @brief 把带符号定点整数按指定小数位追加到命令缓冲（无浮点）。
 * @param out: 目标命令缓冲。
 * @param value: 放大 scale 倍后的有符号值。
 * @param scale: 10^decimals。
 * @param decimals: 小数位数。
 * @retval true: 追加成功。
 * @retval false: 缓冲空间不足。
 */
bool appendSignedFixed(CmdBuf &out, int64_t value, uint64_t scale, uint8_t decimals) noexcept
{
    uint64_t mag = 0ULL;
    if (value < 0) {
        if (!out.append("-")) {
            return false;
        }
        mag = static_cast<uint64_t>(-(value + 1)) + 1ULL;
    } else {
        mag = static_cast<uint64_t>(value);
    }
    const uint64_t whole = mag / scale;
    const uint64_t frac = mag % scale;
    if (whole == 0ULL) {
        if (!out.append("0")) {
            return false;
        }
    } else if (!ascii::appendUint(out, static_cast<uint32_t>(whole))) {
        return false;
    }
    if (!out.append(".")) {
        return false;
    }
    uint64_t div = scale / ascii::kDecimalBase;
    for (uint8_t i = 0U; i < decimals; ++i) {
        const auto digit = static_cast<uint8_t>('0' + ((frac / div) % ascii::kDecimalBase));
        if (!out.append(std::span<const uint8_t>(&digit, 1U))) {
            return false;
        }
        div /= ascii::kDecimalBase;
    }
    return true;
}

/**
 * @brief 生成单条基站 RTCM 启用/关闭步骤（"RTCM<id> COMn <period>" 或 "UNLOG COMn RTCM<id>"）。
 * @param req: 工作模式请求。
 * @param rtcmSlot: kBaseRtcmIds 中的槽位下标。
 * @param forceOff: true 时强制关闭该 RTCM 报文。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 周期或缓冲不合法。
 */
bool buildUnicoreRtcmStep(const ModeRequest &req, size_t rtcmSlot, bool forceOff,
                          ConfigStep &out) noexcept
{
    const uint16_t id = kBaseRtcmIds[rtcmSlot];
    const uint32_t periodMs = forceOff ? 0U : req.rtcmPeriodMs[rtcmSlot];
    out = ConfigStep{.timeoutMs = kConfigTimeoutMs};
    if (periodMs == 0U) {
        return out.cmd.append("UNLOG COM") && ascii::appendUint(out.cmd, req.port) &&
               out.cmd.append(" RTCM") && ascii::appendUint(out.cmd, id) && out.cmd.append("\r\n");
    }
    return out.cmd.append("RTCM") && ascii::appendUint(out.cmd, id) && out.cmd.append(" COM") &&
           ascii::appendUint(out.cmd, req.port) && out.cmd.append(" ") &&
           ascii::appendPeriod(out.cmd, periodMs) && out.cmd.append("\r\n");
}

} // namespace

// ---- 命令库：每个函数只把一条完整命令（含 "\r\n"）写进 out，不关心什么时候发 ----

/**
 * @brief 关闭该口全部报文输出："UNLOG COMn"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号（1..4）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdUnlog(CmdBuf &out, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::appendUint(out, port) && out.append("\r\n");
}

/**
 * @brief 切到流动站模式："MODE ROVER"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdModeRover(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("MODE ROVER\r\n");
}

/**
 * @brief 改端口波特率："CONFIG COMn <baud>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号。
 * @param baud: 目标波特率（如 921600）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdConfigBaud(CmdBuf &out, uint8_t port, uint32_t baud) noexcept
{
    out.clear();
    return out.append("CONFIG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           ascii::appendUint(out, baud) && out.append("\r\n");
}

/**
 * @brief 以给定周期开启一类报文："<name> COMn <period>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGGA"）。
 * @param port: 接收机端口号。
 * @param periodMs: 周期（毫秒），必须是 10 ms 整数倍。
 * @retval true: 已写入完整命令。
 * @retval false: 周期粒度不合法或缓冲空间不足。
 */
bool UnicoreReceiver::cmdLogMsg(CmdBuf &out, std::string_view name, uint8_t port,
                                uint32_t periodMs) noexcept
{
    out.clear();
    return out.append(name) && out.append(" COM") && ascii::appendUint(out, port) &&
           out.append(" ") && ascii::appendPeriod(out, periodMs) && out.append("\r\n");
}

/**
 * @brief 关闭一类报文："UNLOG COMn <name>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGSV"）。
 * @param port: 接收机端口号。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdUnlogMsg(CmdBuf &out, std::string_view name, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           out.append(name) && out.append("\r\n");
}

/**
 * @brief 探测用的版本查询："VERSIONA"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdVersionQuery(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("VERSIONA\r\n");
}

// ---- 基础脚本的步骤与启用条件 ----

/**
 * @brief 步骤启用条件：总是。
 * @param cfg: 流动站配置（本判断不使用）。
 * @retval true: 恒为 true。
 */
bool UnicoreReceiver::stepAlways(const RoverConfig &cfg) noexcept
{
    static_cast<void>(cfg);
    return true;
}

/**
 * @brief 步骤启用条件：cfg.portBaud != 0（为 0 时脚本提前结束）。
 * @param cfg: 流动站配置。
 * @retval true: 需要改波特率。
 * @retval false: 不改波特率。
 */
bool UnicoreReceiver::stepIfPortBaud(const RoverConfig &cfg) noexcept
{
    return cfg.portBaud != 0U;
}

/**
 * @brief 第 0 步：先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。
 * @param cfg: 流动站配置（用端口号）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepUnlogAll(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmdUnlog(out.cmd, cfg.port)) {
        return false;
    }
    out.expectReply = true;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = 0;
    return true;
}

/**
 * @brief 第 1 步：切到流动站模式。
 * @param cfg: 流动站配置（本步不使用）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepModeRover(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    static_cast<void>(cfg);
    if (!cmdModeRover(out.cmd)) {
        return false;
    }
    out.expectReply = true;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = 0;
    return true;
}

/**
 * @brief 第 2 步：改端口波特率；手册要求切换后不再按旧速率等待应答。
 * @param cfg: 流动站配置（用端口号与目标波特率）。
 * @param out: 输出步骤（switchBaud = 目标波特率）。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepPortBaud(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmdConfigBaud(out.cmd, cfg.port, cfg.portBaud)) {
        return false;
    }
    out.expectReply = false;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = cfg.portBaud;
    return true;
}

bool UnicoreReceiver::saveStep(ConfigStep &out) const noexcept
{
    constexpr uint16_t kSaveTimeoutMs = 1800U;
    out = ConfigStep{.timeoutMs = kSaveTimeoutMs};
    return out.cmd.append("SAVECONFIG\r\n");
}

bool UnicoreReceiver::factoryResetStep(uint8_t port, const ProbeResult &,
                                       ConfigStep &out) const noexcept
{
    static_cast<void>(port);
    constexpr uint32_t kFactoryBaud = 115200U;
    out = ConfigStep{.expectReply = false, .switchBaud = kFactoryBaud};
    return out.cmd.append("FRESET\r\n");
}

bool UnicoreReceiver::baudStep(uint8_t port, uint32_t baud, ConfigStep &out) const noexcept
{
    out = ConfigStep{.expectReply = false, .switchBaud = baud};
    return cmdConfigBaud(out.cmd, port, baud);
}

bool UnicoreReceiver::modeStep(const ModeRequest &req, uint8_t idx, ConfigStep &out) const noexcept
{
    if ((req.port < kMinUnicorePort) || (req.port > kMaxUnicorePort)) {
        return false;
    }
    if (idx == 0U) {
        out = ConfigStep{.timeoutMs = kConfigTimeoutMs};
        switch (req.action) {
        case ModeAction::kRover:
        case ModeAction::kBasePrep:
            return cmdModeRover(out.cmd);
        case ModeAction::kBaseSurvey:
            return (req.surveySeconds != 0U) && out.cmd.append("MODE BASE TIME ") &&
                   ascii::appendUint(out.cmd, req.surveySeconds) && out.cmd.append("\r\n");
        case ModeAction::kBaseFixed:
            return out.cmd.append("MODE BASE ") &&
                   appendSignedFixed(out.cmd, req.fixLatE9, kE9Scale, kLatLonDecimals) &&
                   out.cmd.append(" ") &&
                   appendSignedFixed(out.cmd, req.fixLonE9, kE9Scale, kLatLonDecimals) &&
                   out.cmd.append(" ") &&
                   appendSignedFixed(out.cmd, req.fixHeightMm, kMmScale, kHeightDecimals) &&
                   out.cmd.append("\r\n");
        default:
            return false;
        }
    }
    if (req.action == ModeAction::kBaseSurvey) {
        return false;
    }
    const size_t rtcmIdx = static_cast<size_t>(idx - 1U);
    if (rtcmIdx >= kUnicoreRtcmIndices.size()) {
        return false;
    }
    return buildUnicoreRtcmStep(req, kUnicoreRtcmIndices[rtcmIdx], req.action == ModeAction::kRover,
                                out);
}

} // namespace gnss
