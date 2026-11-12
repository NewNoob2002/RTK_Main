// NMEA 0183 解析实现：GGA/RMC/GSA/GSV → gnss::Fix / gnss::SatTable（ADR-0010 §2.5）。
// 线程/调用约束：非线程安全，仅由一个任务顺序调用；不可在 ISR 中使用；不可阻塞。
// 堆：不使用（全部状态在对象内定长数组）。浮点：不使用（十进制定点解析）。
#include "gnss/nmea_parser.hpp"

#include <limits>

namespace gnss {
namespace {

// 十进制定点解析结果；“空字段”与“格式错误”必须区分：前者保持未知，后者整句拒绝。
enum class NumResult : uint8_t { kEmpty, kOk, kError };

bool addr_is(std::span<const uint8_t> addr, const char *suffix) noexcept
{
    for (size_t i = 0; i < 3; ++i) {
        if (addr[addr.size() - 3 + i] != static_cast<uint8_t>(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool is_digit(uint8_t c) noexcept
{
    return c >= static_cast<uint8_t>('0') && c <= static_cast<uint8_t>('9');
}

int64_t pow10(uint8_t n) noexcept
{
    int64_t r = 1;
    for (uint8_t i = 0; i < n; ++i) {
        r *= 10;
    }
    return r;
}

// 四舍五入（远离 0），b 必须为正。用于定点缩放，避免浮点。
int64_t round_div(int64_t a, int64_t b) noexcept
{
    if (a >= 0) {
        return (a + (b / 2)) / b;
    }
    return -((-a + (b / 2)) / b);
}

int64_t rescale(int64_t value, uint8_t from_frac, uint8_t to_frac) noexcept
{
    if (from_frac == to_frac) {
        return value;
    }
    if (from_frac < to_frac) {
        return value * pow10(static_cast<uint8_t>(to_frac - from_frac));
    }
    return round_div(value, pow10(static_cast<uint8_t>(from_frac - to_frac)));
}

// 解析 [+-]?digits[.digits]，返回 10^frac_digits 定点值。上限 4e15，保证后续 ×100 不溢出。
NumResult parse_fixed(std::span<const uint8_t> field, uint8_t max_frac, int64_t &value, uint8_t &frac) noexcept
{
    constexpr int64_t kMagnitudeCap = 4000000000000000LL;
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
    uint8_t frac_count = 0;
    bool seen_dot = false;
    bool seen_digit = false;
    for (; pos < field.size(); ++pos) {
        const uint8_t c = field[pos];
        if (c == static_cast<uint8_t>('.')) {
            if (seen_dot) {
                return NumResult::kError;
            }
            seen_dot = true;
            continue;
        }
        if (!is_digit(c)) {
            return NumResult::kError;
        }
        seen_digit = true;
        if (seen_dot) {
            if (frac_count >= max_frac) {
                return NumResult::kError;
            }
            ++frac_count;
        }
        if (v > (kMagnitudeCap - 9) / 10) {
            return NumResult::kError;
        }
        v = (v * 10) + static_cast<int64_t>(c - static_cast<uint8_t>('0'));
    }
    if (!seen_digit) {
        return NumResult::kError;
    }
    value = negative ? -v : v;
    frac = frac_count;
    return NumResult::kOk;
}

NumResult parse_uint(std::span<const uint8_t> field, uint32_t &value) noexcept
{
    if (field.empty()) {
        return NumResult::kEmpty;
    }
    uint32_t v = 0;
    for (const uint8_t c : field) {
        if (!is_digit(c)) {
            return NumResult::kError;
        }
        if (v > (0xFFFFFFFFU - 9U) / 10U) {
            return NumResult::kError;
        }
        v = (v * 10U) + static_cast<uint32_t>(c - static_cast<uint8_t>('0'));
    }
    value = v;
    return NumResult::kOk;
}

// 以逗号切分语句主体（不含 '$' 与 "*hh\r\n"）。返回的每个字段不含分隔符。
struct FieldIter {
    std::span<const uint8_t> body;
    size_t pos = 0;

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

bool field_equals(std::span<const uint8_t> field, char c) noexcept
{
    return field.size() == 1 && field[0] == static_cast<uint8_t>(c);
}

// GNSS 星座映射：NMEA 4.11 System ID 优先，其次 talker（NMEA 0183 的 GP/GL/GA/GB/BD/GQ/GI）。
bool sysid_to_constellation(uint32_t id, Constellation &out) noexcept
{
    switch (id) {
    case 1:
        out = Constellation::kGps;
        return true;
    case 2:
        out = Constellation::kGlonass;
        return true;
    case 3:
        out = Constellation::kGalileo;
        return true;
    case 4:
        out = Constellation::kBeidou;
        return true;
    case 5:
        out = Constellation::kQzss;
        return true;
    case 6:
        out = Constellation::kNavic;
        return true;
    default:
        return false;
    }
}

bool talker_to_constellation(std::span<const uint8_t> addr, Constellation &out) noexcept
{
    if (addr.size() < 5) {
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

size_t constellation_index(Constellation c) noexcept
{
    const size_t index = static_cast<size_t>(c);
    return index < static_cast<size_t>(Constellation::kCount) ? index : static_cast<size_t>(Constellation::kOther);
}

// hhmmss.sss → UtcTime 的时分与秒内毫秒；空字段返回 kEmpty。
NumResult parse_time(std::span<const uint8_t> field, UtcTime &utc) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parse_fixed(field, 9, v, frac);
    if (r != NumResult::kOk) {
        return r;
    }
    if (v < 0) {
        return NumResult::kError;
    }
    const int64_t scale = pow10(frac);
    const int64_t whole = v / scale;
    const int64_t frac_part = v % scale;
    const int64_t hour = whole / 10000;
    const int64_t minute = (whole / 100) % 100;
    const int64_t second = whole % 100;
    if (hour > 23 || minute > 59 || second > 59) {
        return NumResult::kError;
    }
    const int64_t ms = (second * 1000) + round_div(frac_part * 1000, scale);
    if (ms > 59999) {
        return NumResult::kError; // 59.9995 进位后越界，按格式错误处理
    }
    utc.hour = static_cast<uint8_t>(hour);
    utc.minute = static_cast<uint8_t>(minute);
    utc.ms = static_cast<uint16_t>(ms);
    return NumResult::kOk;
}

// ddmmyy（RMC 日期）→ UtcTime 的年月日；空字段返回 kEmpty。
NumResult parse_date(std::span<const uint8_t> field, UtcTime &utc) noexcept
{
    uint32_t v = 0;
    const NumResult r = parse_uint(field, v);
    if (r != NumResult::kOk) {
        return r;
    }
    if (field.size() != 6) {
        return NumResult::kError;
    }
    const uint32_t day = v / 10000U;
    const uint32_t month = (v / 100U) % 100U;
    const uint32_t year2 = v % 100U;
    if (day == 0 || day > 31 || month == 0 || month > 12) {
        return NumResult::kError;
    }
    utc.day = static_cast<uint8_t>(day);
    utc.month = static_cast<uint8_t>(month);
    utc.year = static_cast<uint16_t>(2000U + year2);
    return NumResult::kOk;
}

// ddmm.mmm... → 1e-9 度。分数位最多 7（ADR-0010 §2.5，UM980 GGA 的上限），保证 int64 不溢出。
NumResult parse_angle(std::span<const uint8_t> field, bool is_longitude, int64_t &e9) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parse_fixed(field, 7, v, frac);
    if (r != NumResult::kOk) {
        return r;
    }
    if (v < 0) {
        return NumResult::kError;
    }
    const int64_t scale = pow10(frac);
    const int64_t degree = v / (100 * scale);
    const int64_t minute_scaled = v - (degree * 100 * scale);
    const int64_t limit = is_longitude ? 180 : 90;
    if (degree > limit || minute_scaled >= (60 * scale)) {
        return NumResult::kError;
    }
    const int64_t value = (degree * 1000000000LL) + round_div(minute_scaled * 1000000000LL, 60 * scale);
    if (value > (limit * 1000000000LL)) {
        return NumResult::kError;
    }
    e9 = value;
    return NumResult::kOk;
}

// DOP 字段 → ×100；空字段/超范围按未知（0xFFFF）。
NumResult parse_dop(std::span<const uint8_t> field, uint16_t &out) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parse_fixed(field, 4, v, frac);
    if (r == NumResult::kEmpty) {
        out = kU16Unknown;
        return r;
    }
    if (r == NumResult::kError) {
        return r;
    }
    const int64_t x100 = rescale(v, frac, 2);
    out = (x100 < 0 || x100 > static_cast<int64_t>(kU16Unknown - 1)) ? kU16Unknown : static_cast<uint16_t>(x100);
    return NumResult::kOk;
}

// 毫米字段（高度/水准面差距）→ int32 mm。
NumResult parse_mm(std::span<const uint8_t> field, int32_t &out) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parse_fixed(field, 9, v, frac);
    if (r != NumResult::kOk) {
        return r;
    }
    const int64_t mm = rescale(v, frac, 3);
    if (mm < std::numeric_limits<int32_t>::min() || mm > std::numeric_limits<int32_t>::max()) {
        return NumResult::kError;
    }
    out = static_cast<int32_t>(mm);
    return NumResult::kOk;
}

// 差分龄期 0.1 s。
NumResult parse_diff_age(std::span<const uint8_t> field, uint16_t &out) noexcept
{
    int64_t v = 0;
    uint8_t frac = 0;
    const NumResult r = parse_fixed(field, 4, v, frac);
    if (r == NumResult::kEmpty) {
        out = kU16Unknown;
        return r;
    }
    if (r == NumResult::kError) {
        return r;
    }
    const int64_t ds = rescale(v, frac, 1);
    out = (ds < 0 || ds > static_cast<int64_t>(kU16Unknown - 1)) ? kU16Unknown : static_cast<uint16_t>(ds);
    return NumResult::kOk;
}

} // namespace

// 解析细节通过 friend 访问 NmeaParser 的私有状态；对外只暴露 feed()。
struct NmeaImpl {
    static NmeaResult feed(NmeaParser &parser, std::span<const uint8_t> sentence, uint32_t now_ms) noexcept;
    static NmeaResult parse_gga(NmeaParser &parser, std::span<const uint8_t> body, uint32_t now_ms) noexcept;
    static NmeaResult parse_rmc(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static NmeaResult parse_gsa(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static NmeaResult parse_gsv(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static bool publish(NmeaParser &parser, uint32_t now_ms) noexcept;
    static void clear_field(SatInfo &sat) noexcept;
};

void NmeaImpl::clear_field(SatInfo &sat) noexcept
{
    sat = SatInfo{};
}

// 把本历元内已收齐（msg_num == total）的系统合并 used 标记后整表发布。
// 返回是否发生了发布；调用方据此决定 feed() 返回 kSatsReady 还是 kFix。
bool NmeaImpl::publish(NmeaParser &parser, uint32_t now_ms) noexcept
{
    bool any_done = false;
    for (const uint8_t done : parser.gsv_done_) {
        if (done != 0) {
            any_done = true;
            break;
        }
    }
    if (!any_done) {
        return false;
    }

    SatTable out{};
    for (size_t i = 0; i < parser.pending_.count && out.count < kMaxSats; ++i) {
        const SatInfo &src = parser.pending_.sats[i];
        if (parser.gsv_done_[constellation_index(src.system)] == 0) {
            continue; // 该系统本轮未收齐，不发布半轮
        }
        out.sats[out.count] = src;
        ++out.count;
    }
    out.update_ms = now_ms;

    for (size_t i = 0; i < out.count; ++i) {
        SatInfo &sat = out.sats[i];
        const uint16_t key =
            static_cast<uint16_t>((static_cast<uint16_t>(sat.system) << 8) | static_cast<uint16_t>(sat.prn));
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

NmeaResult NmeaImpl::parse_gga(NmeaParser &parser, std::span<const uint8_t> body, uint32_t now_ms) noexcept
{
    FieldIter it{body};
    std::span<const uint8_t> f{};
    if (!it.next(f)) { // 地址字段（talker + GGA）
        return NmeaResult::kMalformed;
    }

    Fix fix = parser.fix_;
    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    if (parse_time(f, fix.utc) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }

    std::span<const uint8_t> lat_field{};
    std::span<const uint8_t> ns_field{};
    std::span<const uint8_t> lon_field{};
    std::span<const uint8_t> ew_field{};
    if (!it.next(lat_field) || !it.next(ns_field) || !it.next(lon_field) || !it.next(ew_field)) {
        return NmeaResult::kMalformed;
    }
    if (!lat_field.empty() || !ns_field.empty()) {
        int64_t lat = 0;
        if (parse_angle(lat_field, false, lat) != NumResult::kOk) {
            return NmeaResult::kMalformed;
        }
        const int sign = field_equals(ns_field, 'N') ? 1 : (field_equals(ns_field, 'S') ? -1 : 0);
        if (sign == 0) {
            return NmeaResult::kMalformed;
        }
        fix.lat_e9 = static_cast<int64_t>(sign) * lat;
    }
    if (!lon_field.empty() || !ew_field.empty()) {
        int64_t lon = 0;
        if (parse_angle(lon_field, true, lon) != NumResult::kOk) {
            return NmeaResult::kMalformed;
        }
        const int sign = field_equals(ew_field, 'E') ? 1 : (field_equals(ew_field, 'W') ? -1 : 0);
        if (sign == 0) {
            return NmeaResult::kMalformed;
        }
        fix.lon_e9 = static_cast<int64_t>(sign) * lon;
    }

    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    uint32_t quality_raw = 0;
    const NumResult qr = parse_uint(f, quality_raw);
    if (qr == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (qr == NumResult::kOk) {
        fix.quality =
            (quality_raw <= 8U) ? static_cast<FixQuality>(static_cast<uint8_t>(quality_raw)) : FixQuality::kUnknown;
        fix.valid = fix.quality != FixQuality::kInvalid && fix.quality != FixQuality::kUnknown;
    } else {
        fix.quality = FixQuality::kInvalid;
        fix.valid = false;
    }

    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    uint32_t sats_raw = 0;
    const NumResult sr = parse_uint(f, sats_raw);
    if (sr == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (sr == NumResult::kOk) {
        fix.sats_used = static_cast<uint8_t>(sats_raw > 255U ? 255U : sats_raw);
    }

    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    if (parse_dop(f, fix.hdop_x100) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }

    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    if (parse_mm(f, fix.alt_mm) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    if (!f.empty() && !field_equals(f, 'M')) {
        return NmeaResult::kMalformed;
    }

    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    if (parse_mm(f, fix.geoid_mm) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (!it.next(f)) {
        return NmeaResult::kMalformed;
    }
    if (!f.empty() && !field_equals(f, 'M')) {
        return NmeaResult::kMalformed;
    }

    // 差分字段在部分接收机上可省略：缺失按未知处理。
    if (it.next(f) && parse_diff_age(f, fix.diff_age_ds) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (it.next(f)) {
        uint32_t station = 0;
        const NumResult st = parse_uint(f, station);
        if (st == NumResult::kError) {
            return NmeaResult::kMalformed;
        }
        fix.diff_station =
            (st == NumResult::kOk) ? static_cast<uint16_t>(station > 0xFFFFU ? 0xFFFFU : station) : kU16Unknown;
    }

    fix.update_ms = now_ms;
    parser.fix_ = fix;
    ++parser.stats_.gga;
    return NmeaImpl::publish(parser, now_ms) ? NmeaResult::kSatsReady : NmeaResult::kFix;
}

NmeaResult NmeaImpl::parse_rmc(NmeaParser &parser, std::span<const uint8_t> body) noexcept
{
    std::array<std::span<const uint8_t>, 12> fields{};
    size_t count = 0;
    FieldIter it{body};
    while (count < fields.size() && it.next(fields[count])) {
        ++count;
    }
    if (count < 10) { // 地址 + UTC + 状态 + 4 个坐标字段 + 速度 + 航向 + 日期
        return NmeaResult::kMalformed;
    }

    Fix fix = parser.fix_;
    if (parse_time(fields[1], fix.utc) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (!fields[2].empty() && !field_equals(fields[2], 'A') && !field_equals(fields[2], 'V')) {
        return NmeaResult::kMalformed;
    }

    int64_t knots = 0;
    uint8_t frac = 0;
    const NumResult sp = parse_fixed(fields[7], 3, knots, frac);
    if (sp == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (sp == NumResult::kOk) {
        const int64_t milli_knots = rescale(knots, frac, 3);
        const int64_t mmps = round_div(milli_knots * 1852, 3600);
        fix.speed_mmps = (mmps < 0) ? 0U : static_cast<uint32_t>(mmps);
    }

    int64_t course = 0;
    const NumResult cr = parse_fixed(fields[8], 4, course, frac);
    if (cr == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (cr == NumResult::kOk) {
        const int64_t cdeg = rescale(course, frac, 2);
        fix.course_cdeg = (cdeg < 0) ? 0U : static_cast<uint16_t>(cdeg > 35999 ? 35999 : cdeg);
    }

    if (parse_date(fields[9], fix.utc) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }

    parser.fix_ = fix;
    ++parser.stats_.rmc;
    return NmeaResult::kAux;
}

NmeaResult NmeaImpl::parse_gsa(NmeaParser &parser, std::span<const uint8_t> body) noexcept
{
    std::array<std::span<const uint8_t>, 20> fields{};
    size_t count = 0;
    FieldIter it{body};
    while (count < fields.size() && it.next(fields[count])) {
        ++count;
    }
    if (count < 18) { // 地址 + 模式 + 定位类型 + 12 PRN + PDOP/HDOP/VDOP
        return NmeaResult::kMalformed;
    }
    if (!fields[1].empty() && fields[1].size() != 1) {
        return NmeaResult::kMalformed;
    }
    if (!fields[2].empty() && !field_equals(fields[2], '1') && !field_equals(fields[2], '2') &&
        !field_equals(fields[2], '3')) {
        return NmeaResult::kMalformed;
    }

    std::array<uint8_t, 12> prns{};
    size_t prn_count = 0;
    for (size_t i = 3; i < 15; ++i) {
        if (fields[i].empty()) {
            continue;
        }
        uint32_t prn = 0;
        if (parse_uint(fields[i], prn) != NumResult::kOk || prn > 255U) {
            return NmeaResult::kMalformed;
        }
        prns[prn_count] = static_cast<uint8_t>(prn);
        ++prn_count;
    }

    Fix fix = parser.fix_;
    if (parse_dop(fields[15], fix.pdop_x100) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (parse_dop(fields[16], fix.hdop_x100) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }
    if (parse_dop(fields[17], fix.vdop_x100) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }

    Constellation system = Constellation::kOther;
    bool have_system = talker_to_constellation(fields[0], system);
    if (count > 18 && !fields[18].empty()) {
        uint32_t id = 0;
        if (parse_uint(fields[18], id) != NumResult::kOk) {
            return NmeaResult::kMalformed;
        }
        Constellation from_id = Constellation::kOther;
        if (sysid_to_constellation(id, from_id)) {
            system = from_id;
            have_system = true;
        }
    }
    if (!have_system) {
        system = Constellation::kOther;
    }

    for (size_t i = 0; i < prn_count && parser.used_count_ < parser.used_.size(); ++i) {
        const uint16_t key =
            static_cast<uint16_t>((static_cast<uint16_t>(system) << 8) | static_cast<uint16_t>(prns[i]));
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

NmeaResult NmeaImpl::parse_gsv(NmeaParser &parser, std::span<const uint8_t> body) noexcept
{
    std::array<std::span<const uint8_t>, 24> fields{};
    size_t count = 0;
    FieldIter it{body};
    while (count < fields.size() && it.next(fields[count])) {
        ++count;
    }
    if (count < 4) {
        return NmeaResult::kMalformed;
    }
    uint32_t total = 0;
    uint32_t num = 0;
    uint32_t in_view = 0;
    if (parse_uint(fields[1], total) != NumResult::kOk || parse_uint(fields[2], num) != NumResult::kOk) {
        return NmeaResult::kMalformed;
    }
    if (total == 0 || total > 9 || num == 0 || num > total) {
        return NmeaResult::kMalformed;
    }
    if (!fields[3].empty() && parse_uint(fields[3], in_view) == NumResult::kError) {
        return NmeaResult::kMalformed;
    }

    const size_t remaining = count - 4;
    size_t blocks = remaining / 4;
    size_t extra = remaining % 4;
    if (extra > 1) {
        return NmeaResult::kMalformed;
    }
    if (blocks > 4) {
        return NmeaResult::kMalformed;
    }

    std::array<SatInfo, 4> sats{};
    size_t sat_count = 0;
    for (size_t b = 0; b < blocks; ++b) {
        const size_t base = 4 + (b * 4);
        if (fields[base].empty()) {
            continue;
        }
        SatInfo sat{};
        uint32_t prn = 0;
        if (parse_uint(fields[base], prn) != NumResult::kOk || prn > 255U) {
            return NmeaResult::kMalformed;
        }
        sat.prn = static_cast<uint8_t>(prn);
        uint32_t elev = 0;
        uint32_t azim = 0;
        uint32_t snr = 0;
        if (parse_uint(fields[base + 1], elev) == NumResult::kError ||
            parse_uint(fields[base + 2], azim) == NumResult::kError ||
            parse_uint(fields[base + 3], snr) == NumResult::kError) {
            return NmeaResult::kMalformed;
        }
        sat.elev_deg = static_cast<int8_t>(elev > 90U ? 90U : elev);
        sat.azim_deg = static_cast<uint16_t>(azim % 360U);
        sat.snr_dbhz = static_cast<uint8_t>(snr > 255U ? 255U : snr);
        sats[sat_count] = sat;
        ++sat_count;
    }

    Constellation system = Constellation::kOther;
    bool have_system = talker_to_constellation(fields[0], system);
    if (extra == 1) {
        const std::span<const uint8_t> id_field = fields[4 + (blocks * 4)];
        if (!id_field.empty()) {
            uint32_t id = 0;
            if (parse_uint(id_field, id) != NumResult::kOk) {
                return NmeaResult::kMalformed;
            }
            Constellation from_id = Constellation::kOther;
            if (sysid_to_constellation(id, from_id)) {
                system = from_id;
                have_system = true;
            }
        }
    }
    if (!have_system) {
        system = Constellation::kOther;
    }

    // 全部字段校验通过后才修改状态，保证 malformed 不改任何状态。
    for (size_t i = 0; i < sat_count; ++i) {
        SatInfo sat = sats[i];
        sat.system = system;
        bool merged = false;
        for (size_t k = 0; k < parser.pending_.count; ++k) {
            SatInfo &existing = parser.pending_.sats[k];
            if (existing.system == sat.system && existing.prn == sat.prn) {
                existing = sat;
                merged = true;
                break;
            }
        }
        if (!merged && parser.pending_.count < kMaxSats) {
            parser.pending_.sats[parser.pending_.count] = sat;
            ++parser.pending_.count;
        }
    }
    if (num == total) {
        parser.gsv_done_[constellation_index(system)] = 1;
    }
    ++parser.stats_.gsv;
    return NmeaResult::kSatsPart;
}

NmeaResult NmeaImpl::feed(NmeaParser &parser, std::span<const uint8_t> sentence, uint32_t now_ms) noexcept
{
    // 输入由 demux::nmea 保证：'$'、正文、'*hh'、"\r\n"。这里再做最小防御。
    if (sentence.size() < 11 || sentence[0] != static_cast<uint8_t>('$') ||
        sentence[sentence.size() - 1] != static_cast<uint8_t>('\n') ||
        sentence[sentence.size() - 2] != static_cast<uint8_t>('\r') ||
        sentence[sentence.size() - 5] != static_cast<uint8_t>('*')) {
        ++parser.stats_.malformed;
        return NmeaResult::kMalformed;
    }
    const std::span<const uint8_t> body = sentence.subspan(1, sentence.size() - 6);
    size_t comma = 0;
    while (comma < body.size() && body[comma] != static_cast<uint8_t>(',')) {
        ++comma;
    }
    if (comma < 5) {
        ++parser.stats_.ignored;
        return NmeaResult::kIgnored;
    }
    const std::span<const uint8_t> addr = body.first(comma);

    NmeaResult result = NmeaResult::kIgnored;
    if (addr_is(addr, "GGA")) {
        result = parse_gga(parser, body, now_ms);
    } else if (addr_is(addr, "RMC")) {
        result = parse_rmc(parser, body);
    } else if (addr_is(addr, "GSA")) {
        result = parse_gsa(parser, body);
    } else if (addr_is(addr, "GSV")) {
        result = parse_gsv(parser, body);
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

NmeaResult NmeaParser::feed(std::span<const uint8_t> sentence, uint32_t now_ms) noexcept
{
    return NmeaImpl::feed(*this, sentence, now_ms);
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
