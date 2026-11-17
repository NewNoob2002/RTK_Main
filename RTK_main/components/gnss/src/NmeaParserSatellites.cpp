// NMEA 卫星视图语句解析：GSA（参与解算的卫星、DOP）与 GSV（逐颗信噪比、多句合并）。
// 线程安全：否（只由 gnss::NmeaParser::feed 在单任务上下文调用）。ISR 安全：否（写解析状态）。
// 堆与阻塞：不使用堆、不阻塞、不使用浮点。
#include "NmeaParserPrivate.hpp"

namespace gnss {
namespace {

using detail::constellationIndex;
using detail::fieldEquals;
using detail::FieldIter;
using detail::kGsaPrnCount;
using detail::NumResult;
using detail::parseDop;
using detail::parseUint;
using detail::resolveSystem;
using detail::satKey;

// GSA：12 个 PRN 槽 + 3 个 DOP + 可选 System ID。
constexpr size_t kGsaMaxFields = 20U;
constexpr size_t kGsaMinFields = 18U;
constexpr size_t kGsaPrnFirst = 3U;
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

/** @brief 读出 GSA 的 12 个 PRN 字段（空字段表示该位未使用）。
 * @param fields: 已切分的字段
 * @param prns: 输出的 PRN 数组
 * @param prnCount: 输出的有效 PRN 数
 * @retval false 表示某个 PRN 字段格式错误或超出范围
 */
bool takeGsaPrns(const std::array<std::span<const uint8_t>, kGsaMaxFields> &fields,
                 std::array<uint8_t, kGsaPrnCount> &prns, size_t &prnCount) noexcept
{
    for (size_t i = kGsaPrnFirst; i < (kGsaPrnFirst + kGsaPrnCount); ++i) {
        if (fields[i].empty()) {
            continue;
        }
        uint32_t prn = 0;
        if (parseUint(fields[i], prn) != NumResult::kOk || prn > kMaxPrn) {
            return false;
        }
        prns[prnCount] = static_cast<uint8_t>(prn);
        ++prnCount;
    }
    return true;
}

/** @brief 解析 GSA 尾部的 PDOP/HDOP/VDOP。
 * @param fields: 已切分的字段
 * @param fix: 待更新的定位结果
 * @retval false 表示某个 DOP 字段格式错误
 */
bool takeGsaDops(const std::array<std::span<const uint8_t>, kGsaMaxFields> &fields,
                 Fix &fix) noexcept
{
    if (parseDop(fields[kGsaDopFirst], fix.pdopX100) == NumResult::kError) {
        return false;
    }
    if (parseDop(fields[kGsaDopFirst + 1U], fix.hdopX100) == NumResult::kError) {
        return false;
    }
    return parseDop(fields[kGsaDopFirst + 2U], fix.vdopX100) != NumResult::kError;
}

} // namespace

/**
 * @brief  把本历元 GSA 的 PRN 并入参与解算表（去重，满表截断）
 * @param  parser: 解析器状态
 * @param  system: 星座
 * @param  prns: PRN 数组
 * @param  prnCount: 有效 PRN 数
 * @retval 无
 */
void NmeaImpl::mergeUsed(NmeaParser &parser, Constellation system,
                         const std::array<uint8_t, kGsaPrnCount> &prns, size_t prnCount) noexcept
{
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
    if (!takeGsaPrns(fields, prns, prnCount)) {
        return NmeaResult::kMalformed;
    }

    Fix fix = parser.fix_;
    if (!takeGsaDops(fields, fix)) {
        return NmeaResult::kMalformed;
    }

    Constellation system = Constellation::kOther;
    const std::span<const uint8_t> idField =
        (count > kGsaSystemIdIndex) ? fields[kGsaSystemIdIndex] : std::span<const uint8_t>{};
    if (!resolveSystem(fields[0], idField, system)) {
        return NmeaResult::kMalformed;
    }

    mergeUsed(parser, system, prns, prnCount);

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

} // namespace gnss
