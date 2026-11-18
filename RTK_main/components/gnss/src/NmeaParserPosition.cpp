// NMEA 定位语句解析：GGA（定位质量/高度/差分龄期）与 RMC（状态/速度/日期）。
// 线程安全：否（只由 gnss::NmeaParser::feed 在单任务上下文调用）。ISR 安全：否（写解析状态）。
// 堆与阻塞：不使用堆、不阻塞、不使用浮点（定点转整数，ADR-0010 §2.5）。
#include "NmeaParserPrivate.hpp"

namespace gnss {
namespace {

using detail::fieldEquals;
using detail::FieldIter;
using detail::NumResult;
using detail::parseAngle;
using detail::parseDate;
using detail::parseDiffAge;
using detail::parseDop;
using detail::parseFixed;
using detail::parseMm;
using detail::parseTime;
using detail::parseUint;
using detail::rescale;
using detail::roundDiv;

// GGA 数值上限。
constexpr uint32_t kMaxQuality = 8U;
constexpr uint32_t kMaxSatsUsed = 255U;
constexpr uint32_t kMaxDiffStation = 0xFFFFU;

// RMC：速度按节→mm/s，航向 0.01 度。
constexpr size_t kRmcMaxFields = 12U;
constexpr size_t kRmcMinFields = 10U; // 地址 + UTC + 状态 + 4 坐标 + 速度 + 航向 + 日期
constexpr size_t kRmcDateField = 9U;  // RMC 第 9 个字段是日期（ddmmyy）
constexpr uint8_t kMaxKnotFracDigits = 3U;
constexpr int64_t kMmPerNauticalMile = 1852;
constexpr int64_t kSecondsPerHour = 3600;
constexpr uint8_t kMaxCourseFracDigits = 4U;
constexpr uint8_t kCourseScale = 2U;
constexpr uint32_t kMaxCourseCdeg = 35999U;

// GST：lat_err/lon_err 按米→毫米，合成水平标准差后转厘米。
constexpr size_t kGstMaxFields = 9U;
constexpr size_t kGstMinFields = 8U;
constexpr size_t kGstLatErrField = 6U;
constexpr size_t kGstLonErrField = 7U;
constexpr uint8_t kGstMaxFracDigits = 6U;
constexpr uint8_t kGstMmScale = 3U;
constexpr int64_t kMaxGstAxisMm = 1000000LL; // 1 km：超过该值水平精度必已超出 uint16_t 厘米上限
constexpr uint64_t kMmPerCm = 10ULL;

/**
 * @brief 计算非负 64 位整数的整数平方根（向下取整）。
 * @param n: 被开方数。
 * @retval res: floor(sqrt(n))。
 */
uint64_t isqrt64(uint64_t n) noexcept
{
    constexpr unsigned kMaxIsqrtBitShift = 62U;
    uint64_t res = 0ULL;
    uint64_t bit = 1ULL << kMaxIsqrtBitShift;
    while (bit > n) {
        bit >>= 2U;
    }
    while (bit != 0ULL) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1U) + bit;
        } else {
            res >>= 1U;
        }
        bit >>= 2U;
    }
    return res;
}

/**
 * @brief 解析 GST 单轴误差字段为毫米（非负，饱和到 kMaxGstAxisMm）。
 * @param field: 误差字段（单位米）。
 * @param outMm: 输出毫米值（仅返回 kOk 时有效）。
 * @retval kEmpty 空字段；kOk 成功；kError 格式非法或为负。
 */
NumResult parseGstAxisMm(std::span<const uint8_t> field, int64_t &outMm) noexcept
{
    int64_t val = 0;
    uint8_t frac = 0;
    const NumResult r = parseFixed(field, kGstMaxFracDigits, val, frac);
    if (r != NumResult::kOk) {
        return r;
    }
    if (val < 0) {
        return NumResult::kError;
    }
    const int64_t mm = rescale(val, frac, kGstMmScale);
    outMm = (mm > kMaxGstAxisMm) ? kMaxGstAxisMm : mm;
    return NumResult::kOk;
}

/** @brief GGA 的 UTC 时间字段。
 * @param it: 字段迭代器
 * @param fix: 待更新的定位结果
 * @retval false 表示字段缺失或格式错误
 */
bool takeTime(FieldIter &it, Fix &fix) noexcept
{
    std::span<const uint8_t> f{};
    return it.next(f) && (parseTime(f, fix.utc) != NumResult::kError);
}

/** @brief GGA 的经纬度 + 半球字段（两者都空表示未知，保持原值）。
 * @param it: 字段迭代器
 * @param fix: 待更新的定位结果
 * @retval false 表示字段缺失或格式错误
 */
bool takePosition(FieldIter &it, Fix &fix) noexcept
{
    std::span<const uint8_t> latField{};
    std::span<const uint8_t> nsField{};
    std::span<const uint8_t> lonField{};
    std::span<const uint8_t> ewField{};
    if (!it.next(latField) || !it.next(nsField) || !it.next(lonField) || !it.next(ewField)) {
        return false;
    }
    if (!latField.empty() || !nsField.empty()) {
        int64_t lat = 0;
        if (parseAngle(latField, false, lat) != NumResult::kOk) {
            return false;
        }
        const int sign = fieldEquals(nsField, 'N') ? 1 : (fieldEquals(nsField, 'S') ? -1 : 0);
        if (sign == 0) {
            return false;
        }
        fix.latE9 = static_cast<int64_t>(sign) * lat;
    }
    if (!lonField.empty() || !ewField.empty()) {
        int64_t lon = 0;
        if (parseAngle(lonField, true, lon) != NumResult::kOk) {
            return false;
        }
        const int sign = fieldEquals(ewField, 'E') ? 1 : (fieldEquals(ewField, 'W') ? -1 : 0);
        if (sign == 0) {
            return false;
        }
        fix.lonE9 = static_cast<int64_t>(sign) * lon;
    }
    return true;
}

/** @brief GGA 的定位质量与使用卫星数。
 * @param it: 字段迭代器
 * @param fix: 待更新的定位结果
 * @retval false 表示字段缺失或格式错误
 */
bool takeQuality(FieldIter &it, Fix &fix) noexcept
{
    std::span<const uint8_t> f{};
    if (!it.next(f)) {
        return false;
    }
    uint32_t qualityRaw = 0;
    const NumResult qr = parseUint(f, qualityRaw);
    if (qr == NumResult::kError) {
        return false;
    }
    if (qr == NumResult::kOk) {
        fix.quality = (qualityRaw <= kMaxQuality)
                          ? static_cast<FixQuality>(static_cast<uint8_t>(qualityRaw))
                          : FixQuality::kUnknown;
        fix.valid = fix.quality != FixQuality::kInvalid && fix.quality != FixQuality::kUnknown;
    } else {
        fix.quality = FixQuality::kInvalid;
        fix.valid = false;
    }

    uint32_t satsRaw = 0;
    if (!it.next(f)) {
        return false;
    }
    const NumResult sr = parseUint(f, satsRaw);
    if (sr == NumResult::kError) {
        return false;
    }
    if (sr == NumResult::kOk) {
        fix.satsUsed = static_cast<uint8_t>(satsRaw > kMaxSatsUsed ? kMaxSatsUsed : satsRaw);
    }
    return true;
}

/** @brief GGA 的 HDOP 字段。
 * @param it: 字段迭代器
 * @param fix: 待更新的定位结果
 * @retval false 表示字段缺失或格式错误
 */
bool takeHdop(FieldIter &it, Fix &fix) noexcept
{
    std::span<const uint8_t> f{};
    return it.next(f) && (parseDop(f, fix.hdopX100) != NumResult::kError);
}

/** @brief GGA 的“数值 + 单位 M”字段对（高度/水准面差距）。
 * @param it: 字段迭代器
 * @param outMm: 输出的毫米值
 * @retval false 表示字段缺失、格式错误或单位不是 'M'
 */
bool takeAltitude(FieldIter &it, int32_t &outMm) noexcept
{
    std::span<const uint8_t> f{};
    if (!it.next(f) || (parseMm(f, outMm) == NumResult::kError)) {
        return false;
    }
    return it.next(f) && (f.empty() || fieldEquals(f, 'M'));
}

/** @brief GGA 末尾的差分龄期与基准站号（部分接收机省略）。
 * @param it: 字段迭代器
 * @param fix: 待更新的定位结果
 * @retval false 表示字段存在但格式错误
 */
bool takeDiff(FieldIter &it, Fix &fix) noexcept
{
    std::span<const uint8_t> f{};
    if (it.next(f) && (parseDiffAge(f, fix.diffAgeDs) == NumResult::kError)) {
        return false;
    }
    if (it.next(f)) {
        uint32_t station = 0;
        const NumResult st = parseUint(f, station);
        if (st == NumResult::kError) {
            return false;
        }
        fix.diffStation =
            (st == NumResult::kOk)
                ? static_cast<uint16_t>(station > kMaxDiffStation ? kMaxDiffStation : station)
                : kU16Unknown;
    }
    return true;
}

} // namespace

/**
 * @brief  解析 GGA：UTC、经纬度、质量、HDOP、海拔/大地水准面、差分龄期
 * @param  parser: 解析器状态
 * @param  body: 去掉 '$'、校验和与行尾后的正文
 * @param  nowMs: 当前时间（写入定位时间戳）
 * @retval kFix 已更新定位；kSatsReady 同时完成卫星表发布；kMalformed 字段不合法
 */
NmeaResult NmeaImpl::parseGga(NmeaParser &parser, std::span<const uint8_t> body,
                              uint32_t nowMs) noexcept
{
    FieldIter it{body};
    std::span<const uint8_t> f{};
    if (!it.next(f)) { // 地址字段（talker + GGA）
        return NmeaResult::kMalformed;
    }

    Fix fix = parser.fix_;
    if (!takeTime(it, fix) || !takePosition(it, fix) || !takeQuality(it, fix) ||
        !takeHdop(it, fix) || !takeAltitude(it, fix.altMm) || !takeAltitude(it, fix.geoidMm) ||
        !takeDiff(it, fix)) {
        return NmeaResult::kMalformed;
    }

    fix.updateMs = nowMs;
    parser.fix_ = fix;
    ++parser.stats_.gga;
    return NmeaImpl::publish(parser, nowMs) ? NmeaResult::kSatsReady : NmeaResult::kFix;
}

/**
 * @brief  解析 RMC：日期、速度、航向（GGA 缺失时补齐定位时间）
 * @param  parser: 解析器状态
 * @param  body: 去掉 '$'、校验和与行尾后的正文
 * @retval kAux 已更新辅助字段；kMalformed 字段不合法
 */
NmeaResult NmeaImpl::parseRmc(NmeaParser &parser, std::span<const uint8_t> body) noexcept
{
    std::array<std::span<const uint8_t>, kRmcMaxFields> fields{};
    size_t count = 0;
    FieldIter it{body};
    while (count < fields.size() && it.next(fields[count])) {
        ++count;
    }
    if (count < kRmcMinFields) {
        return NmeaResult::kMalformed;
    }

    Fix fix = parser.fix_;
    if (parseTime(fields[1], fix.utc) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (!fields[2].empty() && !fieldEquals(fields[2], 'A') && !fieldEquals(fields[2], 'V')) {
        return NmeaResult::kMalformed;
    }

    int64_t knots = 0;
    uint8_t frac = 0;
    const NumResult sp = parseFixed(fields[7], kMaxKnotFracDigits, knots, frac);
    if (sp == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (sp == NumResult::kOk) {
        const int64_t milliKnots = rescale(knots, frac, kMaxKnotFracDigits);
        const int64_t mmps = roundDiv(milliKnots * kMmPerNauticalMile, kSecondsPerHour);
        fix.speedMmps = (mmps < 0) ? 0U : static_cast<uint32_t>(mmps);
    }

    int64_t course = 0;
    const NumResult cr = parseFixed(fields[8], kMaxCourseFracDigits, course, frac);
    if (cr == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (cr == NumResult::kOk) {
        const int64_t cdeg = rescale(course, frac, kCourseScale);
        fix.courseCdeg =
            (cdeg < 0) ? 0U : static_cast<uint16_t>(cdeg > kMaxCourseCdeg ? kMaxCourseCdeg : cdeg);
    }

    if (parseDate(fields[kRmcDateField], fix.utc) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }

    parser.fix_ = fix;
    ++parser.stats_.rmc;
    return NmeaResult::kAux;
}

/**
 * @brief 解析 GST：水平定位误差标准差（sqrt(lat^2 + lon^2) → 厘米）。
 * @param parser: 解析器状态。
 * @param body: 去掉 '$'、校验和与行尾后的正文。
 * @retval kAux 已更新辅助字段；kMalformed 字段不合法。
 */
NmeaResult NmeaImpl::parseGst(NmeaParser &parser, std::span<const uint8_t> body) noexcept
{
    std::array<std::span<const uint8_t>, kGstMaxFields> fields{};
    size_t count = 0;
    FieldIter it{body};
    while (count < fields.size() && it.next(fields[count])) {
        ++count;
    }
    if (count < kGstMinFields) {
        return NmeaResult::kMalformed;
    }
    int64_t latMm = 0;
    int64_t lonMm = 0;
    const NumResult rLat = parseGstAxisMm(fields[kGstLatErrField], latMm);
    const NumResult rLon = parseGstAxisMm(fields[kGstLonErrField], lonMm);
    if (rLat == NumResult::kError || rLon == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (rLat == NumResult::kOk && rLon == NumResult::kOk) {
        const uint64_t sq = (static_cast<uint64_t>(latMm) * static_cast<uint64_t>(latMm)) +
                            (static_cast<uint64_t>(lonMm) * static_cast<uint64_t>(lonMm));
        const uint64_t mm = isqrt64(sq);
        const uint64_t cm = (mm + (kMmPerCm / 2ULL)) / kMmPerCm;
        constexpr uint64_t kMaxValidCm = static_cast<uint64_t>(kU16Unknown) - 1ULL;
        parser.fix_.hAccCm = static_cast<uint16_t>(cm > kMaxValidCm ? kMaxValidCm : cm);
    } else {
        parser.fix_.hAccCm = kU16Unknown;
    }
    ++parser.stats_.gst;
    return NmeaResult::kAux;
}

} // namespace gnss
