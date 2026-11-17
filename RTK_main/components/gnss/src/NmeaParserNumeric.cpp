// NMEA 十进制定点解析原语（ADR-0010 §2.5）：字段切分、整数/定点解析、星座映射。
// 线程/调用约束：纯计算，无状态、无阻塞；可被任意任务调用，不可在 ISR 中使用。
// 堆：不使用。浮点：不使用。
#include "NmeaParserPrivate.hpp"

#include <limits>

namespace gnss {
namespace detail {
namespace {

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

// NMEA 4.11 System ID（§2.3）。
constexpr uint32_t kSysIdGps = 1U;
constexpr uint32_t kSysIdGlonass = 2U;
constexpr uint32_t kSysIdGalileo = 3U;
constexpr uint32_t kSysIdBeidou = 4U;
constexpr uint32_t kSysIdQzss = 5U;
constexpr uint32_t kSysIdNavic = 6U;
} // namespace

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

} // namespace detail
} // namespace gnss
