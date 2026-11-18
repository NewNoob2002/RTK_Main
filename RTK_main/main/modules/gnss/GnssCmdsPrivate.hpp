// gnss 命令组内部契约：只在 modules/gnss/GnssCmds*.cpp 之间共享的格式与常量。
// 仅在 console REPL 任务调用，非 ISR；全部 inline 纯函数，不分配堆。

#pragma once

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <span>

#include "data/GnssOutput.hpp"
#include "gnss/Types.hpp"

namespace app::detail {

/** 经纬度定点（1e-9 度）的整数刻度。 */
inline constexpr int64_t kE9Scale = 1000000000LL;
/** 毫米到米换算。 */
inline constexpr int32_t kMmPerMeter = 1000;
/** 百分之一单位（DOP/航向）换算基数。 */
inline constexpr uint32_t kPercentBase = 100U;
/** 十分之一单位（航向 0.1°）换算基数。 */
inline constexpr uint32_t kTenthsBase = 10U;
/** `obs_ring` 未分配时报告给操作者的预留字节数。 */
inline constexpr uint32_t kStaticObsReservedBytes = 16384U;
/** 经纬度文本缓冲（形如 `-123.123456789`）。 */
inline constexpr size_t kLatLonBufBytes = 24U;
/** 米文本缓冲（形如 `-123.456`）。 */
inline constexpr size_t kMetersBufBytes = 20U;
/** DOP/航向文本缓冲（形如 `12.34`）。 */
inline constexpr size_t kDopBufBytes = 16U;
/** 应用消息名，下标与掩码位一致（kGnssAppMsgCount 项）。 */
inline constexpr std::array<const char *, data::kGnssAppMsgCount> kAppMsgNames = {
    {"gga", "sats", "heading", "gst", "rmc", "gsa"}};

/**
 * @brief 定位质量枚举转文本
 * @param q: 定位质量
 * @retval 静态字符串，不分配
 */
inline const char *fixQualityName(::gnss::FixQuality q) noexcept
{
    switch (q) {
    case ::gnss::FixQuality::kInvalid:
        return "invalid";
    case ::gnss::FixQuality::kSingle:
        return "single";
    case ::gnss::FixQuality::kDgps:
        return "dgps";
    case ::gnss::FixQuality::kPps:
        return "pps";
    case ::gnss::FixQuality::kRtkFixed:
        return "rtk-fixed";
    case ::gnss::FixQuality::kRtkFloat:
        return "rtk-float";
    case ::gnss::FixQuality::kDeadReckoning:
        return "dr";
    case ::gnss::FixQuality::kManual:
        return "manual";
    case ::gnss::FixQuality::kSimulator:
        return "sim";
    case ::gnss::FixQuality::kUnknown:
    default:
        return "unknown";
    }
}

/**
 * @brief 星座枚举转 NMEA 风格的短名
 * @param sys: 星座
 * @retval 静态字符串，不分配
 */
inline const char *constellationName(::gnss::Constellation sys) noexcept
{
    switch (sys) {
    case ::gnss::Constellation::kGps:
        return "GPS";
    case ::gnss::Constellation::kGlonass:
        return "GLO";
    case ::gnss::Constellation::kGalileo:
        return "GAL";
    case ::gnss::Constellation::kBeidou:
        return "BDS";
    case ::gnss::Constellation::kQzss:
        return "QZS";
    case ::gnss::Constellation::kNavic:
        return "IRN";
    case ::gnss::Constellation::kSbas:
        return "SBAS";
    case ::gnss::Constellation::kOther:
    default:
        return "OTH";
    }
}

/**
 * @brief 1e-9 度定点转 ±ddd.ddddddddd 文本
 * @param val: 定点值
 * @param[out] out: 输出缓冲，至少 kLatLonBufBytes
 * @note 仅供 console 打印，不参与实时路径。
 */
inline void formatE9(int64_t val, std::span<char, kLatLonBufBytes> out) noexcept
{
    const char *sign = (val < 0) ? "-" : "";
    const int64_t absVal = (val < 0) ? -val : val;
    const int64_t whole = absVal / kE9Scale;
    const int64_t frac = absVal % kE9Scale;
    (void)std::snprintf(out.data(), out.size(), "%s%lld.%09lld", sign,
                        static_cast<long long>(whole), static_cast<long long>(frac));
}

/**
 * @brief 毫米整型转 ±ddd.ddd 米的文本
 * @param mm: 毫米值
 * @param[out] out: 输出缓冲，至少 kMetersBufBytes
 * @note 仅供 console 打印，不参与实时路径。
 */
inline void formatMmAsMeters(int32_t mm, std::span<char, kMetersBufBytes> out) noexcept
{
    const char *sign = (mm < 0) ? "-" : "";
    const int64_t absMm = (mm < 0) ? -static_cast<int64_t>(mm) : static_cast<int64_t>(mm);
    const int64_t whole = absMm / kMmPerMeter;
    const int64_t frac = absMm % kMmPerMeter;
    (void)std::snprintf(out.data(), out.size(), "%s%lld.%03lld", sign,
                        static_cast<long long>(whole), static_cast<long long>(frac));
}

/**
 * @brief 百分之一单位转 `xx.yy` 文本（未知值打印 unknown）
 * @param val: 百分之一单位值
 * @param[out] out: 输出缓冲，至少 kDopBufBytes
 * @note 仅供 console 打印，不参与实时路径。
 */
inline void formatX100(uint16_t val, std::span<char, kDopBufBytes> out) noexcept
{
    if (val == ::gnss::kU16Unknown) {
        (void)std::snprintf(out.data(), out.size(), "unknown");
        return;
    }
    const uint32_t whole = static_cast<uint32_t>(val) / kPercentBase;
    const uint32_t frac = static_cast<uint32_t>(val) % kPercentBase;
    (void)std::snprintf(out.data(), out.size(), "%" PRIu32 ".%02" PRIu32, whole, frac);
}

} // namespace app::detail

namespace app {

/**
 * @brief 显示 GNSS 状态
 * @param ctx: 回调上下文
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 已显示
 * @retval 非零 参数非法，已输出 ERR
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
int cmdGnssStatus(void *ctx, int argc, char **argv) noexcept;
/**
 * @brief 显示最近定位解
 * @param ctx: 回调上下文
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 已显示
 * @retval 非零 参数非法，已输出 ERR
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
int cmdGnssFix(void *ctx, int argc, char **argv) noexcept;
/**
 * @brief 显示卫星列表
 * @param ctx: 回调上下文
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 已显示
 * @retval 非零 参数非法，已输出 ERR
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
int cmdGnssSats(void *ctx, int argc, char **argv) noexcept;

namespace detail {
/** @brief 控制台保存一次接收机配置。 @param argc: 参数数。 @retval 0 成功；其他为 ERR。 */
int cmdGnssSave(void *, int argc, char **) noexcept;
/** @brief 控制台恢复 GNSS 缺省。 @param argc: 参数数。 @param argv: 参数。 @retval 0 成功；其他为 ERR。 */
int cmdGnssFactoryReset(void *, int argc, char **argv) noexcept;
/** @brief 控制台同步修改 GNSS 波特率。 @param argc: 参数数。 @param argv: 参数。 @retval 0 成功；其他为 ERR。 */
int cmdGnssBaud(void *, int argc, char **argv) noexcept;
/** @brief 控制台切换或查询 GNSS 工作模式（rover/base/smartbase）。 */
int cmdGnssMode(void *, int argc, char **argv) noexcept;
/** @brief 控制台查询或配置基站参数（survey/fixed）。 */
int cmdGnssBase(void *, int argc, char **argv) noexcept;
/** @brief 控制台查询或配置基站 RTCM 输出速率及 MSM 版本。 */
int cmdGnssOutRtcm(std::span<char *const> args) noexcept;

} // namespace detail

} // namespace app
