// NMEA 0183 解析器：语句骨架校验、语句分发、卫星表合并与 Fix 发布。
// 线程/调用约束：feed/reset 只能由同一任务顺序调用；不可在 ISR 中使用、不可阻塞。
// 堆：不使用。浮点：不使用（定点解析，ADR-0010 §2.5）。
// 具体语句解析见 NmeaParserPosition.cpp（GGA/RMC）与 NmeaParserSatellites.cpp（GSA/GSV），
// 定点原语见 NmeaParserNumeric.cpp。
#include <string_view>

#include "NmeaParserPrivate.hpp"

namespace gnss {
namespace {

using detail::constellationIndex;
using detail::kMinAddrLen;
using detail::satKey;

// NMEA 语句骨架：'$' + 地址 + 正文 + '*' + 2 位校验 + CRLF；最短语句 11 字节。
constexpr size_t kMinSentenceBytes = 11U;
constexpr size_t kSentenceOverhead = 6U; // '$' + '*'hh + CRLF
// 消息名固定 3 个字母（GGA/RMC/GSA/GSV），地址含 2 字母 talker。
constexpr size_t kAddrSuffixLen = 3U;
/** 文末 "*hh\r\n" 中 '*' 距末尾的字节数（$ 正文 * hh \r \n）。 */
constexpr size_t kStarOffsetFromEnd = 5U;

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

} // namespace

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
