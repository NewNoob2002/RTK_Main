// GNSS 基站与工作模式 NVS 配置编解码、默认值与定点数解析实现。
// 线程：纯函数，无全局可变状态，非 ISR。不使用堆、不使用浮点。
#include "modules/gnss/GnssBaseConfig.hpp"

#include <cstdio>
#include <limits>

namespace app {
namespace {

constexpr uint8_t kMsm4Start = 4U;
constexpr uint8_t kMsm7Start = 8U;
constexpr size_t kMsmGroupLen = 4U;
constexpr size_t kSlot1005 = 0U;
constexpr size_t kSlot1006 = 1U;
constexpr size_t kSlot1033 = 2U;
constexpr size_t kSlot1230 = 3U;
constexpr int8_t kDefaultMsmRate = 1;
constexpr int8_t kRate01HzCode = -10;
constexpr int8_t kRate02HzCode = -5;
constexpr int8_t kRate05HzCode = -2;
constexpr int8_t kRate1HzCode = 1;
constexpr int8_t kRate2HzCode = 2;
constexpr int8_t kRate5HzCode = 5;
constexpr uint32_t kMsPerSec = 1000U;
constexpr uint32_t kByteMask = 0xFFU;
constexpr unsigned kBitsPerByte = 8U;
constexpr uint64_t kDecimalBase = 10ULL;
constexpr uint64_t kRoundHalfDigit = 5ULL;
constexpr uint8_t kMaxScaledDecimals = 9U;

/**
 * @brief 将 uint64_t 按 10^decimals 上界做安全加法，溢出返回 false。
 * @param acc: 累加器。
 * @param digit: 当前十进制数字（0..9）。
 * @param limit: 上界。
 * @retval true: 未溢出。
 * @retval false: 溢出。
 */
bool mulAddBounded(uint64_t &acc, uint64_t digit, uint64_t limit) noexcept
{
    if (acc > ((limit - digit) / kDecimalBase)) {
        return false;
    }
    acc = (acc * kDecimalBase) + digit;
    return true;
}

/**
 * @brief 消耗十进制定点数文本前导符号位，并校验非空正文。
 * @param text: 原始输入文本。
 * @param idx: 输出正文起始下标。
 * @param neg: 输出是否为负号。
 * @retval true: 存在符号后的有效起点。
 * @retval false: 空串或只有符号。
 */
bool parseDecimalSign(std::string_view text, size_t &idx, bool &neg) noexcept
{
    idx = 0U;
    neg = false;
    if (text.empty()) {
        return false;
    }
    if (text[idx] == '-') {
        neg = true;
        ++idx;
    } else if (text[idx] == '+') {
        ++idx;
    }
    return idx < text.size();
}

/**
 * @brief 扫描整数和小数数字，按 fracDigits 缩放并返回无符号绝对值及进位标志。
 * @param body: 去掉正负号后的文本视图。
 * @param fracDigits: 目标小数位数（0..9）。
 * @param magOut: 放大后的绝对值（截断多余小数前）。
 * @param roundUpOut: 第 fracDigits+1 位是否 >= 5。
 * @retval true: 合法数字文本且未溢出 int64。
 * @retval false: 含非法字符、多个小数点或溢出。
 */
bool scanDecimalDigits(std::string_view body, uint8_t fracDigits, uint64_t &magOut,
                       bool &roundUpOut) noexcept
{
    constexpr uint64_t kMaxMag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t mag = 0ULL;
    bool seenDigit = false;
    bool seenDot = false;
    uint8_t fracCount = 0U;
    roundUpOut = false;

    for (const char ch : body) {
        if (ch == '.') {
            if (seenDot) {
                return false;
            }
            seenDot = true;
            continue;
        }
        if ((ch < '0') || (ch > '9')) {
            return false;
        }
        seenDigit = true;
        const auto d = static_cast<uint64_t>(ch - '0');
        if (!seenDot) {
            if (!mulAddBounded(mag, d, kMaxMag)) {
                return false;
            }
        } else if (fracCount < fracDigits) {
            if (!mulAddBounded(mag, d, kMaxMag)) {
                return false;
            }
            ++fracCount;
        } else if (fracCount == fracDigits) {
            roundUpOut = (d >= kRoundHalfDigit);
            ++fracCount;
        }
    }
    if (!seenDigit) {
        return false;
    }
    while (fracCount < fracDigits) {
        if (!mulAddBounded(mag, 0ULL, kMaxMag)) {
            return false;
        }
        ++fracCount;
    }
    magOut = mag;
    return true;
}

} // namespace

GnssBaseConfig GnssBaseConfig::defaults() noexcept
{
    GnssBaseConfig cfg{};
    cfg.mode = WorkModeKind::kRover;
    cfg.baseType = BaseType::kSurvey;
    cfg.svyS = kDefaultSvyS;
    cfg.svyAccCm = kDefaultSvyAccCm;
    cfg.svyToS = kDefaultSvyToS;
    cfg.fixLatE9 = 0;
    cfg.fixLonE9 = 0;
    cfg.fixHeightMm = 0;
    cfg.msm = kMsm4;
    cfg.rtcmOut = defaultRtcmOut(kMsm4);
    return cfg;
}

/**
 * @brief 生成指定 MSM 版本对应的默认 RTCM 输出率数组。
 * @param msm: 4 或 7。
 * @retval rates: 1005/1033/1230 及 MSM4 或 MSM7 置默认率，其余关闭。
 */
std::array<int8_t, ::gnss::kBaseRtcmCount> GnssBaseConfig::defaultRtcmOut(uint8_t msm) noexcept
{
    std::array<int8_t, ::gnss::kBaseRtcmCount> rates{};
    rates[kSlot1005] = kRate1HzCode;
    rates[kSlot1006] = 0;
    rates[kSlot1033] = kRate01HzCode;
    rates[kSlot1230] = kRate01HzCode;
    const size_t baseIdx = (msm == kMsm7) ? kMsm7Start : kMsm4Start;
    for (size_t i = 0U; i < kMsmGroupLen; ++i) {
        rates[baseIdx + i] = kDefaultMsmRate;
    }
    return rates;
}

void GnssBaseConfig::applyMsm(uint8_t newMsm) noexcept
{
    if ((newMsm != kMsm4) && (newMsm != kMsm7)) {
        return;
    }
    const size_t fromStart = (newMsm == kMsm7) ? kMsm4Start : kMsm7Start;
    const size_t toStart = (newMsm == kMsm7) ? kMsm7Start : kMsm4Start;
    bool anyActive = false;
    for (size_t i = 0U; i < kMsmGroupLen; ++i) {
        if (rtcmOut[toStart + i] != 0) {
            anyActive = true;
        }
    }
    bool copied = false;
    for (size_t i = 0U; i < kMsmGroupLen; ++i) {
        const int8_t prev = rtcmOut[fromStart + i];
        rtcmOut[fromStart + i] = 0;
        if (prev != 0) {
            rtcmOut[toStart + i] = prev;
            copied = true;
        }
    }
    if (!copied && !anyActive) {
        for (size_t i = 0U; i < kMsmGroupLen; ++i) {
            rtcmOut[toStart + i] = kDefaultMsmRate;
        }
    }
    msm = newMsm;
}

uint32_t GnssBaseConfig::rateToPeriodMs(int8_t encoded) noexcept
{
    if (encoded > 0) {
        return kMsPerSec / static_cast<uint32_t>(encoded);
    }
    if (encoded < 0) {
        return static_cast<uint32_t>(-static_cast<int32_t>(encoded)) * kMsPerSec;
    }
    return 0U;
}

bool GnssBaseConfig::isValidRateCode(int8_t encoded) noexcept
{
    return (encoded == 0) || (encoded == kRate1HzCode) || (encoded == kRate2HzCode) ||
           (encoded == kRate5HzCode) || (encoded == kRate05HzCode) || (encoded == kRate02HzCode) ||
           (encoded == kRate01HzCode);
}

std::optional<size_t> GnssBaseConfig::rtcmSlot(uint16_t id) noexcept
{
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        if (::gnss::kBaseRtcmIds[i] == id) {
            return i;
        }
    }
    return std::nullopt;
}

std::array<uint32_t, ::gnss::kBaseRtcmCount> GnssBaseConfig::toPeriodsMs() const noexcept
{
    std::array<uint32_t, ::gnss::kBaseRtcmCount> periods{};
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        periods[i] = rateToPeriodMs(rtcmOut[i]);
    }
    return periods;
}

bool GnssBaseConfig::encodeRtcmOut(std::span<uint8_t> out, size_t &len) const noexcept
{
    if (out.size() < kRtcmOutBlobBytes) {
        return false;
    }
    out[0U] = static_cast<uint8_t>(kRtcmOutVersion);
    out[1U] = 0U;
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        out[kRtcmOutHeaderBytes + i] = static_cast<uint8_t>(rtcmOut[i]);
    }
    len = kRtcmOutBlobBytes;
    return true;
}

bool GnssBaseConfig::decodeRtcmOut(std::span<const uint8_t> in) noexcept
{
    if ((in.size() != kRtcmOutBlobBytes) || (in[0U] != kRtcmOutVersion)) {
        return false;
    }
    std::array<int8_t, ::gnss::kBaseRtcmCount> parsed{};
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        const auto code = static_cast<int8_t>(in[kRtcmOutHeaderBytes + i]);
        if (!isValidRateCode(code)) {
            return false;
        }
        parsed[i] = code;
    }
    rtcmOut = parsed;
    return true;
}

/**
 * @brief 编码 64 位小端有符号整数（fix_lat / fix_lon）。
 * @param val: 待编码整数。
 * @param out: 目标字节切片（至少 8 字节）。
 */
void encodeI64Le(int64_t val, std::span<uint8_t> out) noexcept
{
    if (out.size() < sizeof(int64_t)) {
        return;
    }
    const auto u = static_cast<uint64_t>(val);
    for (size_t i = 0U; i < sizeof(int64_t); ++i) {
        out[i] = static_cast<uint8_t>((u >> (i * kBitsPerByte)) & kByteMask);
    }
}

/**
 * @brief 解码 64 位小端有符号整数（fix_lat / fix_lon）。
 * @param in: 输入字节切片（至少 8 字节）。
 * @retval val: 解码结果，输入不足返回 0。
 */
int64_t decodeI64Le(std::span<const uint8_t> in) noexcept
{
    if (in.size() < sizeof(int64_t)) {
        return 0;
    }
    uint64_t u = 0ULL;
    for (size_t i = 0U; i < sizeof(int64_t); ++i) {
        u |= static_cast<uint64_t>(in[i]) << (i * kBitsPerByte);
    }
    return static_cast<int64_t>(u);
}

/**
 * @brief 编码 32 位小端有符号整数（fix_h_mm）。
 * @param val: 待编码整数。
 * @param out: 目标字节切片（至少 4 字节）。
 */
void encodeI32Le(int32_t val, std::span<uint8_t> out) noexcept
{
    if (out.size() < sizeof(int32_t)) {
        return;
    }
    const auto u = static_cast<uint32_t>(val);
    for (size_t i = 0U; i < sizeof(int32_t); ++i) {
        out[i] = static_cast<uint8_t>((u >> (i * kBitsPerByte)) & kByteMask);
    }
}

/**
 * @brief 解码 32 位小端有符号整数（fix_h_mm）。
 * @param in: 输入字节切片（至少 4 字节）。
 * @retval val: 解码结果，输入不足返回 0。
 */
int32_t decodeI32Le(std::span<const uint8_t> in) noexcept
{
    if (in.size() < sizeof(int32_t)) {
        return 0;
    }
    uint32_t u = 0U;
    for (size_t i = 0U; i < sizeof(int32_t); ++i) {
        u |= static_cast<uint32_t>(in[i]) << (i * kBitsPerByte);
    }
    return static_cast<int32_t>(u);
}

bool parseScaledDecimal(std::string_view text, uint8_t fracDigits, int64_t &outVal) noexcept
{
    if (fracDigits > kMaxScaledDecimals) {
        return false;
    }
    size_t bodyStart = 0U;
    bool neg = false;
    if (!parseDecimalSign(text, bodyStart, neg)) {
        return false;
    }
    uint64_t mag = 0ULL;
    bool roundUp = false;
    if (!scanDecimalDigits(text.substr(bodyStart), fracDigits, mag, roundUp)) {
        return false;
    }
    constexpr uint64_t kMaxMag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (roundUp && (mag < kMaxMag)) {
        ++mag;
    }
    outVal = neg ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
    return true;
}

std::optional<WorkModeKind> parseWorkMode(std::string_view text) noexcept
{
    if (text == "rover") {
        return WorkModeKind::kRover;
    }
    if (text == "base") {
        return WorkModeKind::kBase;
    }
    if (text == "smartbase") {
        return WorkModeKind::kSmartBase;
    }
    return std::nullopt;
}

const char *workModeName(WorkModeKind mode) noexcept
{
    switch (mode) {
    case WorkModeKind::kRover:
        return "rover";
    case WorkModeKind::kBase:
        return "base";
    case WorkModeKind::kSmartBase:
        return "smartbase";
    default:
        return "unknown";
    }
}

const char *baseTypeName(BaseType type) noexcept
{
    return (type == BaseType::kFixed) ? "fixed" : "survey";
}

const char *workModeStateName(WorkModeState state) noexcept
{
    switch (state) {
    case WorkModeState::kRover:
        return "rover";
    case WorkModeState::kBaseConfig:
        return "base-config";
    case WorkModeState::kSurveyWait:
        return "survey-wait";
    case WorkModeState::kSurvey:
        return "survey";
    case WorkModeState::kBaseTx:
        return "base-tx";
    case WorkModeState::kFallback:
        return "fallback";
    default:
        return "unknown";
    }
}

const char *workModeErrName(WorkModeErr err) noexcept
{
    switch (err) {
    case WorkModeErr::kNone:
        return "none";
    case WorkModeErr::kUnsupported:
        return "not supported";
    case WorkModeErr::kNack:
        return "receiver rejected command";
    case WorkModeErr::kTimeout:
        return "timeout";
    case WorkModeErr::kSurveyTimeout:
        return "survey timed out";
    case WorkModeErr::kInvalidConfig:
        return "invalid config";
    default:
        return "unknown";
    }
}

bool parseRtcmRateArg(std::string_view text, int8_t &outCode) noexcept
{
    if ((text == "off") || (text == "0")) {
        outCode = 0;
        return true;
    }
    if (text == "0.1") {
        outCode = kRate01HzCode;
        return true;
    }
    if (text == "0.2") {
        outCode = kRate02HzCode;
        return true;
    }
    if (text == "0.5") {
        outCode = kRate05HzCode;
        return true;
    }
    if (text == "1") {
        outCode = kRate1HzCode;
        return true;
    }
    if (text == "2") {
        outCode = kRate2HzCode;
        return true;
    }
    if (text == "5") {
        outCode = kRate5HzCode;
        return true;
    }
    return false;
}

const char *formatRtcmRate(int8_t code, std::span<char> buf) noexcept
{
    if (buf.empty()) {
        return "";
    }
    if (code == 0) {
        std::snprintf(buf.data(), buf.size(), "off");
    } else if (code == kRate01HzCode) {
        std::snprintf(buf.data(), buf.size(), "0.1 Hz");
    } else if (code == kRate02HzCode) {
        std::snprintf(buf.data(), buf.size(), "0.2 Hz");
    } else if (code == kRate05HzCode) {
        std::snprintf(buf.data(), buf.size(), "0.5 Hz");
    } else if (code > 0) {
        std::snprintf(buf.data(), buf.size(), "%d Hz", static_cast<int>(code));
    } else {
        std::snprintf(buf.data(), buf.size(), "1/%ds", -static_cast<int>(code));
    }
    return buf.data();
}

const char *formatScaledInt(int64_t val, uint8_t decimals, std::span<char> buf) noexcept
{
    if (buf.empty()) {
        return "";
    }
    uint64_t scale = 1ULL;
    for (uint8_t i = 0U; i < decimals; ++i) {
        scale *= kDecimalBase;
    }
    const bool neg = val < 0;
    const uint64_t mag =
        neg ? (static_cast<uint64_t>(-(val + 1)) + 1ULL) : static_cast<uint64_t>(val);
    const auto whole = static_cast<unsigned long long>(mag / scale);
    const auto frac = static_cast<unsigned long long>(mag % scale);
    if (decimals == 0U) {
        std::snprintf(buf.data(), buf.size(), "%s%llu", neg ? "-" : "", whole);
    } else {
        std::snprintf(buf.data(), buf.size(), "%s%llu.%0*llu", neg ? "-" : "", whole,
                      static_cast<int>(decimals), frac);
    }
    return buf.data();
}

/**
 * @brief 判断 RTCM3 基站报文是否受当前接收机族支持（1033 仅 Unicore/Optisens，1230 仅 u-blox）。
 * @param id: RTCM3 报文号。
 * @param family: 已探测接收机族。
 * @retval true: 受支持或尚未探测到接收机族；false: 另一族专属报文。
 */
bool isBaseRtcmSupportedForFamily(uint16_t id, ::gnss::Family family) noexcept
{
    if (id == ::gnss::kBaseRtcmIds[kSlot1033]) {
        // Optisens CM680 复用 Unicore 基站 RTCM 脚本，支持 1033；此处仅有家族上下文（无型号），保留家族比较。
        return (family == ::gnss::Family::kUnknown) || (family == ::gnss::Family::kUnicore) ||
               (family == ::gnss::Family::kOptisens);
    }
    if (id == ::gnss::kBaseRtcmIds[kSlot1230]) {
        return (family == ::gnss::Family::kUnknown) || (family == ::gnss::Family::kUblox);
    }
    return true;
}

/**
 * @brief 未探测到接收机族时为族专用 RTCM3 报文返回适用族标签。
 * @param id: RTCM3 报文号。
 * @param family: 当前接收机族。
 * @retval 适用族标签或空串。
 */
const char *baseRtcmFamilyTag(uint16_t id, ::gnss::Family family) noexcept
{
    if (family != ::gnss::Family::kUnknown) {
        return "";
    }
    if (id == ::gnss::kBaseRtcmIds[kSlot1033]) {
        return " (Unicore)";
    }
    if (id == ::gnss::kBaseRtcmIds[kSlot1230]) {
        return " (u-blox)";
    }
    return "";
}

} // namespace app
