// NMEA 0183 解析实现：GGA/RMC/GSA/GSV → gnss::Fix / gnss::SatTable（ADR-0010 §2.5）。
// 线程/调用约束：非线程安全，仅由一个任务顺序调用；不可在 ISR 中使用；不可阻塞。
// 堆：不使用（全部状态在对象内定长数组）。浮点：不使用（十进制定点解析）。
#include "gnss/NmeaParser.hpp"

#include <array>
#include <limits>
#include <string_view>

namespace gnss {
namespace {

// 十进制定点解析结果；“空字段”与“格式错误”必须区分：前者保持未知，后者整句拒绝。
enum class NumResult : uint8_t { kEmpty, kOk, kError };

// NMEA 语句骨架：'$' + 地址 + 正文 + '*' + 2 位校验 + CRLF；最短语句 11 字节。
constexpr size_t kMinSentenceBytes = 11U;
constexpr size_t kSentenceOverhead = 6U; // '$' + '*'hh + CRLF
// 消息名固定 3 个字母（GGA/RMC/GSA/GSV），地址含 2 字母 talker。
constexpr size_t kAddrSuffixLen = 3U;
constexpr size_t kMinAddrLen = 5U;

// 十进制解析基数与位宽上限。
constexpr int64_t kDecimalRadix = 10;
constexpr uint32_t kDecimalRadix32 = 10U;
constexpr uint32_t kMaxUint32 = 0xFFFFFFFFU;
constexpr uint32_t kMaxDigit32 = 9U;
constexpr int64_t kMaxDigit64 = 9;
// 定点幅值上限 4e15：保证后续 ×100 不溢出 int64。
constexpr int64_t kMagnitudeCap = 4000000000000000LL;

// hhmmss.sss：时分秒各 2 位，毫秒不超过 59.999 s。
constexpr int64_t kTimeHourDivisor = 10000;
constexpr int64_t kTimeMinuteDivisor = 100;
constexpr int64_t kMaxHour = 23;
constexpr int64_t kMaxMinute = 59;
constexpr int64_t kMaxSecond = 59;
constexpr int64_t kMsPerSecond = 1000;
constexpr int64_t kMaxMsInMinute = 59999; // 59.9995 s 进位后越界，按格式错误处理
constexpr uint8_t kMaxTimeFracDigits = 9U;

// ddmmyy：日/月/年各 2 位。
constexpr size_t kDateFieldLen = 6U;
constexpr uint32_t kDateDayDivisor = 10000U;
constexpr uint32_t kDateMonthDivisor = 100U;
constexpr uint32_t kMaxDay = 31U;
constexpr uint32_t kMaxMonth = 12U;
constexpr uint16_t kYearBase = 2000U;

// ddmm.mmm：度分换算与经纬度上限；1e-9 度定点，分数位最多 7（ADR-0010 §2.5，UM980 GGA 上限）。
constexpr uint8_t kMaxAngleFracDigits = 7U;
constexpr int64_t kDegreeUnit = 100;
constexpr int64_t kMinutesPerDegree = 60;
constexpr int64_t kE9PerDegree = 1000000000LL;
constexpr int64_t kMaxLatitudeDeg = 90;
constexpr int64_t kMaxLongitudeDeg = 180;

// DOP（×100）与差分龄期（0.1 s）。
constexpr uint8_t kMaxDopFracDigits = 4U;
constexpr uint8_t kDopScale = 2U;
constexpr uint8_t kMaxDiffAgeFracDigits = 4U;
constexpr uint8_t kDiffAgeScale = 1U;

// 高度/水准面差距 → mm。
constexpr uint8_t kMaxMmFracDigits = 9U;
constexpr uint8_t kMmScale = 3U;

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

// GSA：12 个 PRN 槽 + 3 个 DOP + 可选 System ID。
constexpr size_t kGsaMaxFields = 20U;
constexpr size_t kGsaMinFields = 18U;
constexpr size_t kGsaPrnFirst = 3U;
constexpr size_t kGsaPrnCount = 12U;
constexpr size_t kGsaDopFirst = 15U;
constexpr size_t kGsaSystemIdIndex = 18U;

// GSV：每句最多 4 组卫星块（PRN/仰角/方位/SNR）+ 可选 System ID，最多 9 句。
constexpr size_t kGsvMaxFields = 24U;
constexpr size_t kGsvMinFields = 4U;
constexpr size_t kGsvHeaderFields = 4U;
constexpr size_t kGsvBlockFields = 4U;
constexpr size_t kGsvInViewField = 3U; // 头部第 3 个字段是可见卫星数
constexpr size_t kGsvSnrField = 3U;    // 每块第 3 个字段是信噪比
constexpr size_t kGsvMaxBlocks = 4U;
constexpr uint32_t kGsvMaxSentences = 9U;
constexpr uint32_t kMaxPrn = 255U;
constexpr uint32_t kMaxElevDeg = 90U;
constexpr uint32_t kAzimModulo = 360U;
constexpr uint32_t kMaxSnrDbhz = 255U;
/** 文末 "*hh\r\n" 中 '*' 距末尾的字节数（$ 正文 * hh \r \n）。 */
constexpr size_t kStarOffsetFromEnd = 5U;
// NMEA 4.11 System ID（§2.3）。
constexpr uint32_t kSysIdGps = 1U;
constexpr uint32_t kSysIdGlonass = 2U;
constexpr uint32_t kSysIdGalileo = 3U;
constexpr uint32_t kSysIdBeidou = 4U;
constexpr uint32_t kSysIdQzss = 5U;
constexpr uint32_t kSysIdNavic = 6U;
// used_/pending_ 的卫星键 = system << 8 | prn。
constexpr uint32_t kSatKeySystemShift = 8U;

/** @brief 把 used_/pending_ 的键拼成 uint16。
 * @param system: 星座
 * @param prn: 卫星编号
 * @retval 键值（system << 8 | prn）
 */
uint16_t satKey(Constellation system, uint8_t prn) noexcept
{
    // 星座枚举 < 8、prn < 256，合进 uint16 不会溢出。
    return static_cast<uint16_t>((static_cast<uint32_t>(system) << kSatKeySystemShift) |
                                 static_cast<uint32_t>(prn));
}

/** @brief 判断地址字段是否以给定 3 字母消息名结尾。
 * @param addr: 地址字段（talker + 消息名）
 * @param suffix: 3 字母消息名（GGA/RMC/GSA/GSV）
 * @retval true 表示匹配
 */
bool addrIs(std::span<const uint8_t> addr, std::string_view suffix) noexcept
{
    for (size_t i = 0; i < kAddrSuffixLen; ++i) {
        if (addr[addr.size() - kAddrSuffixLen + i] != static_cast<uint8_t>(suffix[i])) {
            return false;
        }
    }
    return true;
}

/** @brief 判断字节是否为 ASCII 数字。
 * @param c: 待判字节
 * @retval true 表示 '0'..'9'
 */
bool isDigit(uint8_t c) noexcept
{
    return c >= static_cast<uint8_t>('0') && c <= static_cast<uint8_t>('9');
}

/** @brief 计算 10^n（int64）。
 * @param n: 指数（调用方保证 n 很小）
 * @retval 10 的 n 次幂
 */
int64_t pow10(uint8_t n) noexcept
{
    int64_t r = 1;
    for (uint8_t i = 0; i < n; ++i) {
        r *= kDecimalRadix;
    }
    return r;
}

/** @brief 四舍五入（远离 0）的整数除法，用于定点缩放，避免浮点。
 * @param a: 被除数
 * @param b: 除数（必须为正）
 * @retval a/b 的四舍五入结果
 */
int64_t roundDiv(int64_t a, int64_t b) noexcept
{
    if (a >= 0) {
        return (a + (b / 2)) / b;
    }
    return -((-a + (b / 2)) / b);
}

/** @brief 定标点数的分数位换算。
 * @param value: 带 fromFrac 位小数的整数表示
 * @param fromFrac: 当前小数位数
 * @param toFrac: 目标小数位数
 * @retval 带 toFrac 位小数的整数表示
 */
int64_t rescale(int64_t value, uint8_t fromFrac, uint8_t toFrac) noexcept
{
    if (fromFrac == toFrac) {
        return value;
    }
    if (fromFrac < toFrac) {
        return value * pow10(static_cast<uint8_t>(toFrac - fromFrac));
    }
    return roundDiv(value, pow10(static_cast<uint8_t>(fromFrac - toFrac)));
}

/** @brief 解析 [+-]?digits[.digits]，返回 10^frac_digits 定点值。
 * @param field: 单个 NMEA 字段
 * @param maxFrac: 允许的最大小数位数
 * @param value: 输出定点值
 * @param frac: 输出实际小数位数
 * @retval kEmpty 空字段；kOk 成功；kError 格式错误
 */
NumResult parseFixed(std::span<const uint8_t> field, uint8_t maxFrac, int64_t &value,
                     uint8_t &frac) noexcept
{
    if (field.empty()) {
        return NumResult::kEmpty;
    }
    size_t pos = 0;
    bool negative = false;
    if (field[0] == static_cast<uint8_t>('-') || field[0] == static_cast<uint8_t>('+')) {
        negative = field[0] == static_cast<uint8_t>('-');
        pos = 1;
    }
    if (pos == field.size()) {
        return NumResult::kError;
    }
    int64_t v = 0;
    uint8_t fracCount = 0;
    bool seenDot = false;
    bool seenDigit = false;
    for (; pos < field.size(); ++pos) {
        const uint8_t c = field[pos];
        if (c == static_cast<uint8_t>('.')) {
            if (seenDot) {
                return NumResult::kError;
            }
            seenDot = true;
            continue;
        }
        if (!isDigit(c)) {
            return NumResult::kError;
        }
        seenDigit = true;
        if (seenDot) {
            if (fracCount >= maxFrac) {
                return NumResult::kError;
            }
            ++fracCount;
        }
        if (v > (kMagnitudeCap - kMaxDigit64) / kDecimalRadix) {
            return NumResult::kError;
        }
        v = (v * kDecimalRadix) + static_cast<int64_t>(c - static_cast<uint8_t>('0'));
    }
    if (!seenDigit) {
        return NumResult::kError;
    }
    value = negative ? -v : v;
    frac = fracCount;
    return NumResult::kOk;
}

/** @brief 解析无符号十进制整数。
 * @param field: 单个 NMEA 字段
 * @param value: 输出值
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseUint(std::span<const uint8_t> field, uint32_t &value) noexcept
{
    if (field.empty()) {
        return NumResult::kEmpty;
    }
    uint32_t v = 0;
    for (const uint8_t c : field) {
        if (!isDigit(c)) {
            return NumResult::kError;
        }
        if (v > (kMaxUint32 - kMaxDigit32) / kDecimalRadix32) {
            return NumResult::kError;
        }
        v = (v * kDecimalRadix32) + static_cast<uint32_t>(c - static_cast<uint8_t>('0'));
    }
    value = v;
    return NumResult::kOk;
}

// 以逗号切分语句主体（不含 '$' 与 "*hh\r\n"）。返回的每个字段不含分隔符。
struct FieldIter {
    std::span<const uint8_t> body;
    size_t pos = 0;

    /** @brief 取下一个逗号分隔字段。
     * @param out: 输出字段（不含分隔符）
     * @retval true 表示还有字段
     */
    bool next(std::span<const uint8_t> &out) noexcept
    {
        if (pos > body.size()) {
            return false;
        }
        size_t comma = pos;
        while (comma < body.size() && body[comma] != static_cast<uint8_t>(',')) {
            ++comma;
        }
        out = body.subspan(pos, comma - pos);
        pos = (comma < body.size()) ? (comma + 1) : (body.size() + 1);
        return true;
    }
};

/** @brief 判断字段是否等于单个字符。
 * @param field: 待判字段
 * @param c: 期望字符
 * @retval true 表示长度 1 且字符相同
 */
bool fieldEquals(std::span<const uint8_t> field, char c) noexcept
{
    return field.size() == 1 && field[0] == static_cast<uint8_t>(c);
}

/**
 * @brief  按 NMEA 4.11 System ID 映射星座
 * @param  id: System ID 字段值（1..6）
 * @param  out: 输出星座
 * @retval true 认识该 ID；false 未知
 */
bool sysidToConstellation(uint32_t id, Constellation &out) noexcept
{
    switch (id) {
    case kSysIdGps:
        out = Constellation::kGps;
        return true;
    case kSysIdGlonass:
        out = Constellation::kGlonass;
        return true;
    case kSysIdGalileo:
        out = Constellation::kGalileo;
        return true;
    case kSysIdBeidou:
        out = Constellation::kBeidou;
        return true;
    case kSysIdQzss:
        out = Constellation::kQzss;
        return true;
    case kSysIdNavic:
        out = Constellation::kNavic;
        return true;
    default:
        return false;
    }
}

/**
 * @brief  按 NMEA 0183 talker（GP/GL/GA/GB/BD/GQ/GI）映射星座
 * @param  addr: 地址字段（talker + 消息名）
 * @param  out: 输出星座
 * @retval true 认识该 talker；false 未知
 */
bool talkerToConstellation(std::span<const uint8_t> addr, Constellation &out) noexcept
{
    if (addr.size() < kMinAddrLen) {
        return false;
    }
    const uint8_t a = addr[0];
    const uint8_t b = addr[1];
    if (a == static_cast<uint8_t>('G') && b == static_cast<uint8_t>('P')) {
        out = Constellation::kGps;
        return true;
    }
    if (a == static_cast<uint8_t>('G') && b == static_cast<uint8_t>('L')) {
        out = Constellation::kGlonass;
        return true;
    }
    if (a == static_cast<uint8_t>('G') && b == static_cast<uint8_t>('A')) {
        out = Constellation::kGalileo;
        return true;
    }
    if ((a == static_cast<uint8_t>('G') || a == static_cast<uint8_t>('B')) &&
        (b == static_cast<uint8_t>('B') || b == static_cast<uint8_t>('D'))) {
        out = Constellation::kBeidou;
        return true;
    }
    if (a == static_cast<uint8_t>('G') && b == static_cast<uint8_t>('Q')) {
        out = Constellation::kQzss;
        return true;
    }
    if (a == static_cast<uint8_t>('G') && b == static_cast<uint8_t>('I')) {
        out = Constellation::kNavic;
        return true;
    }
    return false;
}

/** @brief 星座 → gsv_done_ 索引（越界归到 kOther）。
 * @param c: 星座
 * @retval 数组索引
 */
size_t constellationIndex(Constellation c) noexcept
{
    const size_t index = static_cast<size_t>(c);
    return index < static_cast<size_t>(Constellation::kCount)
               ? index
               : static_cast<size_t>(Constellation::kOther);
}

/** @brief 由 talker 与可选 System ID 字段确定星座。
 * @param addr: 地址字段
 * @param idField: System ID 字段（可为空）
 * @param system: 输出的星座
 * @retval false 表示 System ID 存在但格式错误
 */
bool resolveSystem(std::span<const uint8_t> addr, std::span<const uint8_t> idField,
                   Constellation &system) noexcept
{
    bool haveSystem = talkerToConstellation(addr, system);
    if (!idField.empty()) {
        uint32_t id = 0;
        if (parseUint(idField, id) != NumResult::kOk) {
            return false;
        }
        Constellation fromId = Constellation::kOther;
        if (sysidToConstellation(id, fromId)) {
            system = fromId;
            haveSystem = true;
        }
    }
    if (!haveSystem) {
        system = Constellation::kOther;
    }
    return true;
}

/** @brief hhmmss.sss → UtcTime 的时分与秒内毫秒。
 * @param field: 时间字段
 * @param utc: 输出的 UTC 时间
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseTime(std::span<const uint8_t> field, UtcTime &utc) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parseFixed(field, kMaxTimeFracDigits, v, frac);
    if (r != NumResult::kOk) {
        return r;
    }
    if (v < 0) {
        return NumResult::kError;
    }
    const int64_t scale = pow10(frac);
    const int64_t whole = v / scale;
    const int64_t fracPart = v % scale;
    const int64_t hour = whole / kTimeHourDivisor;
    const int64_t minute = (whole / kTimeMinuteDivisor) % kTimeMinuteDivisor;
    const int64_t second = whole % kTimeMinuteDivisor;
    if (hour > kMaxHour || minute > kMaxMinute || second > kMaxSecond) {
        return NumResult::kError;
    }
    const int64_t ms = (second * kMsPerSecond) + roundDiv(fracPart * kMsPerSecond, scale);
    if (ms > kMaxMsInMinute) {
        return NumResult::kError;
    }
    utc.hour = static_cast<uint8_t>(hour);
    utc.minute = static_cast<uint8_t>(minute);
    utc.ms = static_cast<uint16_t>(ms);
    return NumResult::kOk;
}

/** @brief ddmmyy → UtcTime 的年月日。
 * @param field: 日期字段
 * @param utc: 输出的 UTC 时间
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseDate(std::span<const uint8_t> field, UtcTime &utc) noexcept
{
    uint32_t v = 0;
    const NumResult r = parseUint(field, v);
    if (r != NumResult::kOk) {
        return r;
    }
    if (field.size() != kDateFieldLen) {
        return NumResult::kError;
    }
    const uint32_t day = v / kDateDayDivisor;
    const uint32_t month = (v / kDateMonthDivisor) % kDateMonthDivisor;
    const uint32_t year2 = v % kDateMonthDivisor;
    if (day == 0 || day > kMaxDay || month == 0 || month > kMaxMonth) {
        return NumResult::kError;
    }
    utc.day = static_cast<uint8_t>(day);
    utc.month = static_cast<uint8_t>(month);
    utc.year = static_cast<uint16_t>(kYearBase + year2);
    return NumResult::kOk;
}

/** @brief ddmm.mmm → 1e-9 度。
 * @param field: 经纬度字段
 * @param isLongitude: true 表示经度（上限 180°）
 * @param e9: 输出的 1e-9 度值
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseAngle(std::span<const uint8_t> field, bool isLongitude, int64_t &e9) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parseFixed(field, kMaxAngleFracDigits, v, frac);
    if (r != NumResult::kOk) {
        return r;
    }
    if (v < 0) {
        return NumResult::kError;
    }
    const int64_t scale = pow10(frac);
    const int64_t degree = v / (kDegreeUnit * scale);
    const int64_t minuteScaled = v - (degree * kDegreeUnit * scale);
    const int64_t limitDeg = isLongitude ? kMaxLongitudeDeg : kMaxLatitudeDeg;
    if (degree > limitDeg || minuteScaled >= (kMinutesPerDegree * scale)) {
        return NumResult::kError;
    }
    const int64_t value =
        (degree * kE9PerDegree) + roundDiv(minuteScaled * kE9PerDegree, kMinutesPerDegree * scale);
    if (value > (limitDeg * kE9PerDegree)) {
        return NumResult::kError;
    }
    e9 = value;
    return NumResult::kOk;
}

/** @brief DOP 字段 → ×100；空字段/超范围按未知（0xFFFF）。
 * @param field: DOP 字段
 * @param out: 输出的 ×100 值
 * @retval kEmpty 空字段；kOk 成功；kError 格式错误
 */
NumResult parseDop(std::span<const uint8_t> field, uint16_t &out) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parseFixed(field, kMaxDopFracDigits, v, frac);
    if (r == NumResult::kEmpty) {
        out = kU16Unknown;
        return r;
    }
    if (r == NumResult::kError) {
        return r;
    }
    const int64_t x100 = rescale(v, frac, kDopScale);
    out = (x100 < 0 || x100 > static_cast<int64_t>(kU16Unknown - 1)) ? kU16Unknown
                                                                     : static_cast<uint16_t>(x100);
    return NumResult::kOk;
}

/** @brief 毫米字段（高度/水准面差距）→ int32 mm。
 * @param field: 数值字段
 * @param out: 输出的毫米值
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseMm(std::span<const uint8_t> field, int32_t &out) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parseFixed(field, kMaxMmFracDigits, v, frac);
    if (r != NumResult::kOk) {
        return r;
    }
    const int64_t mm = rescale(v, frac, kMmScale);
    if (mm < std::numeric_limits<int32_t>::min() || mm > std::numeric_limits<int32_t>::max()) {
        return NumResult::kError;
    }
    out = static_cast<int32_t>(mm);
    return NumResult::kOk;
}

/** @brief 差分龄期字段 → 0.1 s。
 * @param field: 龄期字段
 * @param out: 输出的 0.1 s 值
 * @retval kEmpty 空字段；kOk 成功；kError 格式错误
 */
NumResult parseDiffAge(std::span<const uint8_t> field, uint16_t &out) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parseFixed(field, kMaxDiffAgeFracDigits, v, frac);
    if (r == NumResult::kEmpty) {
        out = kU16Unknown;
        return r;
    }
    if (r == NumResult::kError) {
        return r;
    }
    const int64_t ds = rescale(v, frac, kDiffAgeScale);
    out = (ds < 0 || ds > static_cast<int64_t>(kU16Unknown - 1)) ? kU16Unknown
                                                                 : static_cast<uint16_t>(ds);
    return NumResult::kOk;
}

/** @brief 取下一个字段；语句提前结束时返回 false。
 * @param it: 字段迭代器
 * @param out: 输出字段
 * @retval false 表示语句已结束
 */
bool takeField(FieldIter &it, std::span<const uint8_t> &out) noexcept
{
    return it.next(out);
}

/** @brief GGA 的 UTC 时间字段。
 * @param it: 字段迭代器
 * @param fix: 待更新的定位结果
 * @retval false 表示字段缺失或格式错误
 */
bool takeTime(FieldIter &it, Fix &fix) noexcept
{
    std::span<const uint8_t> f{};
    return takeField(it, f) && (parseTime(f, fix.utc) != NumResult::kError);
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

/** @brief 收集 GSV 头部（总句数、当前句、可视数）并校验其范围。
 * @param fields: 已切分的字段
 * @param count: 字段数
 * @param total: 输出的总句数
 * @param num: 输出的当前句号
 * @retval false 表示字段缺失或越界
 */
bool takeGsvHeader(const std::array<std::span<const uint8_t>, kGsvMaxFields> &fields, size_t count,
                   uint32_t &total, uint32_t &num) noexcept
{
    if (count < kGsvMinFields) {
        return false;
    }
    uint32_t inView = 0;
    if (parseUint(fields[1], total) != NumResult::kOk ||
        parseUint(fields[2], num) != NumResult::kOk) {
        return false;
    }
    if (total == 0 || total > kGsvMaxSentences || num == 0 || num > total) {
        return false;
    }
    if (!fields[kGsvInViewField].empty() &&
        parseUint(fields[kGsvInViewField], inView) == NumResult::kError) {
        return false;
    }
    return true;
}

/** @brief 解析 GSV 卫星数据块。
 * @param fields: 已切分的字段
 * @param blocks: 数据块数（0..4）
 * @param sats: 输出的卫星数组
 * @param satCount: 输出的有效卫星数
 * @retval false 表示某块字段格式或范围错误
 */
bool takeGsvBlocks(const std::array<std::span<const uint8_t>, kGsvMaxFields> &fields, size_t blocks,
                   std::array<SatInfo, kGsvMaxBlocks> &sats, size_t &satCount) noexcept
{
    for (size_t b = 0; b < blocks; ++b) {
        const size_t base = kGsvHeaderFields + (b * kGsvBlockFields);
        if (fields[base].empty()) {
            continue;
        }
        SatInfo sat{};
        uint32_t prn = 0;
        if (parseUint(fields[base], prn) != NumResult::kOk || prn > kMaxPrn) {
            return false;
        }
        sat.prn = static_cast<uint8_t>(prn);
        uint32_t elev = 0;
        uint32_t azim = 0;
        uint32_t snr = 0;
        if (parseUint(fields[base + 1], elev) == NumResult::kError ||
            parseUint(fields[base + 2], azim) == NumResult::kError ||
            parseUint(fields[base + kGsvSnrField], snr) == NumResult::kError) {
            return false;
        }
        sat.elevDeg = static_cast<int8_t>(elev > kMaxElevDeg ? kMaxElevDeg : elev);
        sat.azimDeg = static_cast<uint16_t>(azim % kAzimModulo);
        sat.snrDbhz = static_cast<uint8_t>(snr > kMaxSnrDbhz ? kMaxSnrDbhz : snr);
        sats[satCount] = sat;
        ++satCount;
    }
    return true;
}

/** @brief 把本轮 GSV 卫星并入 pending_（同 system+prn 覆盖，满表丢弃）。
 * @param pending: 待发布卫星表
 * @param sats: 本轮卫星
 * @param satCount: 本轮有效卫星数
 * @param system: 本轮星座
 * @retval 无
 */
void mergeGsv(SatTable &pending, const std::array<SatInfo, kGsvMaxBlocks> &sats, size_t satCount,
              Constellation system) noexcept
{
    for (size_t i = 0; i < satCount; ++i) {
        SatInfo sat = sats[i];
        sat.system = system;
        bool merged = false;
        for (size_t k = 0; k < pending.count; ++k) {
            SatInfo &existing = pending.sats[k];
            if (existing.system == sat.system && existing.prn == sat.prn) {
                existing = sat;
                merged = true;
                break;
            }
        }
        if (!merged && pending.count < kMaxSats) {
            pending.sats[pending.count] = sat;
            ++pending.count;
        }
    }
}

} // namespace

// 解析细节通过 friend 访问 NmeaParser 的私有状态；对外只暴露 feed()。
struct NmeaImpl {
    static NmeaResult feed(NmeaParser &parser, std::span<const uint8_t> sentence,
                           uint32_t nowMs) noexcept;
    static NmeaResult parseGga(NmeaParser &parser, std::span<const uint8_t> body,
                               uint32_t nowMs) noexcept;
    static NmeaResult parseRmc(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static NmeaResult parseGsa(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static NmeaResult parseGsv(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static bool publish(NmeaParser &parser, uint32_t nowMs) noexcept;
    static void clearField(SatInfo &sat) noexcept;
};

void NmeaImpl::clearField(SatInfo &sat) noexcept
{
    sat = SatInfo{};
}

/**
 * @brief  把本历元内已收齐（msg_num == total）的系统合并 used 标记后整表发布
 * @param  parser: 解析器状态
 * @param  nowMs: 当前时间（写入卫星表时间戳）
 * @retval true 发生了发布；false 仍在等其余分条
 */
bool NmeaImpl::publish(NmeaParser &parser, uint32_t nowMs) noexcept
{
    bool anyDone = false;
    for (const uint8_t done : parser.gsv_done_) {
        if (done != 0) {
            anyDone = true;
            break;
        }
    }
    if (!anyDone) {
        return false;
    }

    SatTable out{};
    for (size_t i = 0; i < parser.pending_.count && out.count < kMaxSats; ++i) {
        const SatInfo &src = parser.pending_.sats[i];
        if (parser.gsv_done_[constellationIndex(src.system)] == 0) {
            continue; // 该系统本轮未收齐，不发布半轮
        }
        out.sats[out.count] = src;
        ++out.count;
    }
    out.updateMs = nowMs;

    for (size_t i = 0; i < out.count; ++i) {
        SatInfo &sat = out.sats[i];
        const uint16_t key = satKey(sat.system, sat.prn);
        for (size_t k = 0; k < parser.used_count_ && k < parser.used_.size(); ++k) {
            if (parser.used_[k] == key) {
                sat.used = true;
                break;
            }
        }
    }

    parser.sats_ = out;
    parser.pending_ = SatTable{};
    parser.gsv_done_.fill(0);
    parser.used_count_ = 0;
    return true;
}

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
 * @brief  解析 GSA：定位模式、参与解算的卫星与 DOP
 * @param  parser: 解析器状态
 * @param  body: 去掉 '$'、校验和与行尾后的正文
 * @retval kAux 已更新辅助字段；kMalformed 字段不合法
 */
NmeaResult NmeaImpl::parseGsa(NmeaParser &parser, std::span<const uint8_t> body) noexcept
{
    std::array<std::span<const uint8_t>, kGsaMaxFields> fields{};
    size_t count = 0;
    FieldIter it{body};
    while (count < fields.size() && it.next(fields[count])) {
        ++count;
    }
    if (count < kGsaMinFields) { // 地址 + 模式 + 定位类型 + 12 PRN + PDOP/HDOP/VDOP
        return NmeaResult::kMalformed;
    }
    if (!fields[1].empty() && fields[1].size() != 1) {
        return NmeaResult::kMalformed;
    }
    if (!fields[2].empty() && !fieldEquals(fields[2], '1') && !fieldEquals(fields[2], '2') &&
        !fieldEquals(fields[2], '3')) {
        return NmeaResult::kMalformed;
    }

    std::array<uint8_t, kGsaPrnCount> prns{};
    size_t prnCount = 0;
    for (size_t i = kGsaPrnFirst; i < (kGsaPrnFirst + kGsaPrnCount); ++i) {
        if (fields[i].empty()) {
            continue;
        }
        uint32_t prn = 0;
        if (parseUint(fields[i], prn) != NumResult::kOk || prn > kMaxPrn) {
            return NmeaResult::kMalformed;
        }
        prns[prnCount] = static_cast<uint8_t>(prn);
        ++prnCount;
    }

    Fix fix = parser.fix_;
    if (parseDop(fields[kGsaDopFirst], fix.pdopX100) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (parseDop(fields[kGsaDopFirst + 1U], fix.hdopX100) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (parseDop(fields[kGsaDopFirst + 2U], fix.vdopX100) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }

    Constellation system = Constellation::kOther;
    const std::span<const uint8_t> idField =
        (count > kGsaSystemIdIndex) ? fields[kGsaSystemIdIndex] : std::span<const uint8_t>{};
    if (!resolveSystem(fields[0], idField, system)) {
        return NmeaResult::kMalformed;
    }

    for (size_t i = 0; i < prnCount && parser.used_count_ < parser.used_.size(); ++i) {
        const uint16_t key = satKey(system, prns[i]);
        bool duplicate = false;
        for (size_t k = 0; k < parser.used_count_; ++k) {
            if (parser.used_[k] == key) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            parser.used_[parser.used_count_] = key;
            ++parser.used_count_;
        }
    }

    parser.fix_ = fix;
    ++parser.stats_.gsa;
    return NmeaResult::kAux;
}

/**
 * @brief  解析 GSV：按分条累积可见卫星，收齐一个星座后并入待发布表
 * @param  parser: 解析器状态
 * @param  body: 去掉 '$'、校验和与行尾后的正文
 * @retval kSatsPart 已累积；kMalformed 字段不合法
 */
NmeaResult NmeaImpl::parseGsv(NmeaParser &parser, std::span<const uint8_t> body) noexcept
{
    std::array<std::span<const uint8_t>, kGsvMaxFields> fields{};
    size_t count = 0;
    FieldIter it{body};
    while (count < fields.size() && it.next(fields[count])) {
        ++count;
    }

    uint32_t total = 0;
    uint32_t num = 0;
    if (!takeGsvHeader(fields, count, total, num)) {
        return NmeaResult::kMalformed;
    }

    const size_t remaining = count - kGsvHeaderFields;
    const size_t blocks = remaining / kGsvBlockFields;
    const size_t extra = remaining % kGsvBlockFields;
    if (extra > 1 || blocks > kGsvMaxBlocks) {
        return NmeaResult::kMalformed;
    }

    std::array<SatInfo, kGsvMaxBlocks> sats{};
    size_t satCount = 0;
    if (!takeGsvBlocks(fields, blocks, sats, satCount)) {
        return NmeaResult::kMalformed;
    }

    Constellation system = Constellation::kOther;
    const std::span<const uint8_t> idField =
        (extra == 1) ? fields[kGsvHeaderFields + (blocks * kGsvBlockFields)]
                     : std::span<const uint8_t>{};
    if (!resolveSystem(fields[0], idField, system)) {
        return NmeaResult::kMalformed;
    }

    // 全部字段校验通过后才修改状态，保证 malformed 不改任何状态。
    mergeGsv(parser.pending_, sats, satCount, system);
    if (num == total) {
        parser.gsv_done_[constellationIndex(system)] = 1;
    }
    ++parser.stats_.gsv;
    return NmeaResult::kSatsPart;
}

/**
 * @brief  校验一行完整语句并分派到对应消息解析
 * @param  parser: 解析器状态
 * @param  sentence: 完整语句（'$' 开头、'*hh\r\n' 结尾）
 * @param  nowMs: 当前时间
 * @retval 解析结果；非 NMEA 语句返回 kIgnored，校验/形状错误返回 kMalformed
 */
NmeaResult NmeaImpl::feed(NmeaParser &parser, std::span<const uint8_t> sentence,
                          uint32_t nowMs) noexcept
{
    // 输入由 demux::nmea 保证：'$'、正文、'*hh'、"\r\n"。这里再做最小防御。
    if (sentence.size() < kMinSentenceBytes || sentence[0] != static_cast<uint8_t>('$') ||
        sentence[sentence.size() - 1] != static_cast<uint8_t>('\n') ||
        sentence[sentence.size() - 2] != static_cast<uint8_t>('\r') ||
        sentence[sentence.size() - kStarOffsetFromEnd] != static_cast<uint8_t>('*')) {
        ++parser.stats_.malformed;
        return NmeaResult::kMalformed;
    }
    const std::span<const uint8_t> body = sentence.subspan(1, sentence.size() - kSentenceOverhead);
    size_t comma = 0;
    while (comma < body.size() && body[comma] != static_cast<uint8_t>(',')) {
        ++comma;
    }
    if (comma < kMinAddrLen) {
        ++parser.stats_.ignored;
        return NmeaResult::kIgnored;
    }
    const std::span<const uint8_t> addr = body.first(comma);

    NmeaResult result = NmeaResult::kIgnored;
    if (addrIs(addr, "GGA")) {
        result = parseGga(parser, body, nowMs);
    } else if (addrIs(addr, "RMC")) {
        result = parseRmc(parser, body);
    } else if (addrIs(addr, "GSA")) {
        result = parseGsa(parser, body);
    } else if (addrIs(addr, "GSV")) {
        result = parseGsv(parser, body);
    }
    if (result == NmeaResult::kMalformed) {
        ++parser.stats_.malformed; // 字段级解析失败也计入 malformed（不只是帧形状错误）
        return result;
    }
    if (result != NmeaResult::kIgnored) {
        return result;
    }
    ++parser.stats_.ignored;
    return NmeaResult::kIgnored;
}

NmeaResult NmeaParser::feed(std::span<const uint8_t> sentence, uint32_t nowMs) noexcept
{
    return NmeaImpl::feed(*this, sentence, nowMs);
}

void NmeaParser::reset() noexcept
{
    fix_ = Fix{};
    sats_ = SatTable{};
    pending_ = SatTable{};
    gsv_done_.fill(0);
    used_count_ = 0;
}

} // namespace gnss
