// GNSS 控制台与主机测试共用的命令打包辅助（纯编码，不依赖 FreeRTOS/NVS）。
// 线程：纯函数，不访问共享状态或硬件，非 ISR 均可调用；不使用堆。
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "AppConfig.hpp"
#include "data/GnssOutput.hpp"
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "fw/Command.hpp"
#include "modules/gnss/GnssBaseConfig.hpp"
#include "modules/gnss/GnssMaintenance.hpp"

namespace app::detail {

/**
 * @brief 打包 2 字节小端 u16 到缓冲区起始位置。
 * @param v: 待写入的 16 位无符号值。
 * @param[out] out: 目标字节切片（至少 2 字节）。
 */
inline void packU16Le(uint16_t v, std::span<uint8_t> out) noexcept
{
    constexpr unsigned kShift = 8U;
    constexpr uint16_t kMask = 0xFFU;
    out[0U] = static_cast<uint8_t>(v & kMask);
    out[1U] = static_cast<uint8_t>((v >> kShift) & kMask);
}

/**
 * @brief 打包 4 字节小端 u32 到缓冲区起始位置。
 * @param v: 待写入的 32 位无符号值。
 * @param[out] out: 目标字节切片（至少 4 字节）。
 */
inline void packU32Le(uint32_t v, std::span<uint8_t> out) noexcept
{
    constexpr unsigned kShift = 8U;
    constexpr uint32_t kMask = 0xFFU;
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        out[i] = static_cast<uint8_t>((v >> (kShift * i)) & kMask);
    }
}

/**
 * @brief 打包 GNSS 维护命令（save / freset / baud）。
 * @param verb: 维护动作动词。
 * @param baud: 仅 kBaud 时有效的新波特率。
 * @retval cmd: 已填充目标模块、动词与参数的命令。
 */
[[nodiscard]] inline fw::Command packGnssMaintenance(GnssMaintenanceVerb verb,
                                                     uint32_t baud = 0U) noexcept
{
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(verb);
    if (verb == GnssMaintenanceVerb::kBaud) {
        cmd.argLen = static_cast<uint8_t>(sizeof(baud));
        packU32Le(baud, std::span{cmd.args}.first(sizeof(baud)));
    }
    return cmd;
}

/**
 * @brief 打包 gnss gate 质量门控配置命令（3 B：flags, enabled, minSats）。
 * @param setEnabled: 是否修改开关。
 * @param enabled: 新开关值。
 * @param setMinSats: 是否修改最低卫星数。
 * @param minSats: 新最低卫星数（0..40）。
 * @retval cmd: kSetGgaGate 命令。
 */
[[nodiscard]] inline fw::Command packGnssGate(bool setEnabled, bool enabled, bool setMinSats,
                                              uint8_t minSats) noexcept
{
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kSetGgaGate);
    cmd.argLen = kGgaGateCmdArgLen;
    uint8_t flags = 0U;
    if (setEnabled) {
        flags = static_cast<uint8_t>(flags | kGgaGateFlagEnabled);
    }
    if (setMinSats) {
        flags = static_cast<uint8_t>(flags | kGgaGateFlagMinSats);
    }
    cmd.args[0U] = flags;
    cmd.args[1U] = enabled ? 1U : 0U;
    cmd.args[2U] = minSats;
    return cmd;
}

/**
 * @brief 打包 gnss config 读回命令。
 * @retval cmd: kReadback 命令。
 */
[[nodiscard]] inline fw::Command packGnssReadback() noexcept
{
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kReadback);
    return cmd;
}

/**
 * @brief 打包 gnss out 输出配置命令。
 * @param cfg: 已校验的输出配置。
 * @param ok: 输出是否编码成功。
 * @retval cmd: kSetOut 命令（编码失败时返回空参数命令）。
 */
[[nodiscard]] inline fw::Command packGnssOut(const data::GnssOutConfig &cfg, bool &ok) noexcept
{
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kSetOut);
    std::array<uint8_t, data::GnssOutConfig::kBlobBytes> blob{};
    size_t blobLen = 0U;
    ok = fw::isOk(cfg.encode(std::span<uint8_t>{blob}, blobLen));
    static_assert(data::GnssOutConfig::kBlobBytes <= fw::kCommandArgBytes);
    if (ok) {
        std::memcpy(cmd.args.data(), blob.data(), blobLen);
        cmd.argLen = static_cast<uint8_t>(blobLen);
    }
    return cmd;
}

/**
 * @brief 打包 gnss mode 工作模式切换命令。
 * @param target: 目标工作模式。
 * @param saveOnSuccess: 切换成功后是否自动触发接收机保存。
 * @retval cmd: kMode 命令。
 */
[[nodiscard]] inline fw::Command packGnssMode(WorkModeKind target,
                                              bool saveOnSuccess = true) noexcept
{
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kMode);
    cmd.argLen = 2U;
    cmd.args[0U] = static_cast<uint8_t>(target);
    cmd.args[1U] = saveOnSuccess ? 1U : 0U;
    return cmd;
}

/**
 * @brief 打包仅持久化 gnss.base/mode 的 kSetBase 子包（2 B）。
 * @param mode: 新工作模式。
 * @retval cmd: kSetBase(kMode) 命令。
 */
[[nodiscard]] inline fw::Command packGnssBaseMode(WorkModeKind mode) noexcept
{
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kSetBase);
    cmd.argLen = 2U;
    cmd.args[0U] = static_cast<uint8_t>(BasePatchKind::kMode);
    cmd.args[1U] = static_cast<uint8_t>(mode);
    return cmd;
}

/**
 * @brief 打包 gnss base survey 参数的 kSetBase 子包（8 B）。
 * @param svyS: 最短自测时间（s）。
 * @param svyAccCm: 自测精度门限（cm）。
 * @param svyToS: 总超时（s）。
 * @retval cmd: kSetBase(kSurvey) 命令。
 */
[[nodiscard]] inline fw::Command packGnssBaseSurvey(uint16_t svyS, uint16_t svyAccCm,
                                                    uint16_t svyToS) noexcept
{
    constexpr size_t kSurveyPacketBytes = 8U;
    constexpr size_t kSvySOffset = 2U;
    constexpr size_t kSvyAccOffset = 4U;
    constexpr size_t kSvyToOffset = 6U;
    constexpr size_t kU16Bytes = sizeof(uint16_t);
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kSetBase);
    cmd.argLen = static_cast<uint8_t>(kSurveyPacketBytes);
    const auto args = std::span{cmd.args};
    args[0U] = static_cast<uint8_t>(BasePatchKind::kSurvey);
    args[1U] = static_cast<uint8_t>(BaseType::kSurvey);
    packU16Le(svyS, args.subspan(kSvySOffset, kU16Bytes));
    packU16Le(svyAccCm, args.subspan(kSvyAccOffset, kU16Bytes));
    packU16Le(svyToS, args.subspan(kSvyToOffset, kU16Bytes));
    return cmd;
}

/**
 * @brief 打包 gnss base fixed 坐标的 kSetBase 子包（22 B）。
 * @param fixLatE9: 纬度（1e-9 deg）。
 * @param fixLonE9: 经度（1e-9 deg）。
 * @param fixHeightMm: 椭球高（mm）。
 * @retval cmd: kSetBase(kFixed) 命令。
 */
[[nodiscard]] inline fw::Command packGnssBaseFixed(int64_t fixLatE9, int64_t fixLonE9,
                                                   int32_t fixHeightMm) noexcept
{
    constexpr size_t kLatOffset = 2U;
    constexpr size_t kLonOffset = kLatOffset + GnssBaseConfig::kFixCoordBlobBytes;
    constexpr size_t kHeightOffset = kLonOffset + GnssBaseConfig::kFixCoordBlobBytes;
    constexpr size_t kFixedPacketBytes = kHeightOffset + GnssBaseConfig::kFixHeightBlobBytes;
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kSetBase);
    cmd.argLen = static_cast<uint8_t>(kFixedPacketBytes);
    const auto args = std::span{cmd.args};
    args[0U] = static_cast<uint8_t>(BasePatchKind::kFixed);
    args[1U] = static_cast<uint8_t>(BaseType::kFixed);
    encodeI64Le(fixLatE9, args.subspan(kLatOffset, GnssBaseConfig::kFixCoordBlobBytes));
    encodeI64Le(fixLonE9, args.subspan(kLonOffset, GnssBaseConfig::kFixCoordBlobBytes));
    encodeI32Le(fixHeightMm, args.subspan(kHeightOffset, GnssBaseConfig::kFixHeightBlobBytes));
    return cmd;
}

using BaseRtcmRateSpan = std::span<const int8_t, ::gnss::kBaseRtcmCount>;

/**
 * @brief 打包 gnss out rtcm 表的 kSetBase 子包（14 B）。
 * @param msm: MSM 版本（4 或 7）。
 * @param rates: 12 路基站 RTCM 速率编码。
 * @retval cmd: kSetBase(kRtcm) 命令。
 */
[[nodiscard]] inline fw::Command packGnssBaseRtcm(uint8_t msm, BaseRtcmRateSpan rates) noexcept
{
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kSetBase);
    cmd.argLen = static_cast<uint8_t>(2U + ::gnss::kBaseRtcmCount);
    cmd.args[0U] = static_cast<uint8_t>(BasePatchKind::kRtcm);
    cmd.args[1U] = msm;
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        cmd.args[2U + i] = static_cast<uint8_t>(rates[i]);
    }
    return cmd;
}

} // namespace app::detail
