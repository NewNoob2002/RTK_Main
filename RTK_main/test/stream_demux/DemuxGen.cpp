// 分流器测试帧生成辅助实现（独立位运算 CRC，不共享被测组件查找表）。

#include "DemuxGen.hpp"

#include <algorithm>
#include <array>

#include "proto/Frame.hpp"

namespace demux_test {

namespace {

constexpr uint32_t kCrc24qPoly = 0x1864CFBU;
constexpr uint32_t kCrc24qTopBit = 0x1000000U;
constexpr uint32_t kCrc24qMask = 0xFFFFFFU;
constexpr uint32_t kCrc32ReflPoly = 0xEDB88320U;
constexpr uint32_t kCrc32InitIeee = 0xFFFFFFFFU;
constexpr uint32_t kBitsPerByte = 8U;
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kNibbleMask = 0x0FU;
constexpr uint32_t kNibbleBits = 4U;
constexpr uint32_t kShift8 = 8U;
constexpr uint32_t kShift16 = 16U;
constexpr uint32_t kShift24 = 24U;
constexpr std::size_t kOfsByte0 = 0U;
constexpr std::size_t kOfsByte1 = 1U;
constexpr std::size_t kOfsByte2 = 2U;
constexpr std::size_t kOfsByte3 = 3U;

constexpr uint8_t kRtcm3Sync = 0xD3U;
constexpr uint32_t kRtcm3HiBitsMask = 0x03U;
constexpr std::size_t kRtcm3HdrLen = 3U;
constexpr std::size_t kRtcm3CrcLen = 3U;
constexpr std::size_t kRtcm3Overhead = kRtcm3HdrLen + kRtcm3CrcLen;

constexpr uint8_t kUbxSync1 = 0xB5U;
constexpr uint8_t kUbxSync2 = 0x62U;
constexpr std::size_t kOfsUbxClass = 2U;
constexpr std::size_t kOfsUbxId = 3U;
constexpr std::size_t kOfsUbxLen = 4U;
constexpr std::size_t kUbxHdrLen = 6U;
constexpr std::size_t kUbxCkLen = 2U;
constexpr std::size_t kUbxOverhead = kUbxHdrLen + kUbxCkLen;

constexpr uint8_t kUnicoreSync1 = 0xAAU;
constexpr uint8_t kUnicoreSync2 = 0x44U;
constexpr uint8_t kUnicoreSync3 = 0xB5U;
constexpr std::size_t kOfsUnicoreIdle = 3U;
constexpr std::size_t kOfsUnicoreMsgId = 4U;
constexpr std::size_t kOfsUnicoreLen = 6U;
constexpr std::size_t kOfsUnicoreRest = 8U;
constexpr std::size_t kUnicoreHdrLen = 24U;
constexpr std::size_t kUnicoreHdrRest = 16U;
constexpr std::size_t kUnicoreCrcLen = 4U;
constexpr std::size_t kUnicoreOverhead = kUnicoreHdrLen + kUnicoreCrcLen;

constexpr uint8_t kAsciiPrintMin = 0x20U;
constexpr uint8_t kAsciiPrintMax = 0x7EU;
constexpr std::size_t kNmeaTrailerLen = 5U; // *XX\r\n
constexpr std::size_t kHashCrcDigits = 8U;
constexpr std::size_t kHashCrcTrailer = 11U; // *XXXXXXXX\r\n
constexpr std::size_t kTrailerChkHi = 1U;
constexpr std::size_t kTrailerChkLo = 2U;
constexpr std::size_t kTrailerCr = 3U;
constexpr std::size_t kTrailerLf = 4U;
constexpr std::size_t kHashTrailerCr = 9U;
constexpr std::size_t kHashTrailerLf = 10U;

uint8_t nibble_to_hex_byte(uint8_t nibble, bool lowercase) noexcept
{
    static constexpr std::array<char, 16> kUpper{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    static constexpr std::array<char, 16> kLower{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    // 掩码后范围为 0..15，下标不越界
    const auto idx = static_cast<std::size_t>(static_cast<unsigned int>(nibble) & kNibbleMask);
    const char ch = lowercase ? kLower[idx] : kUpper[idx];
    // ASCII 十六进制字符范围为 0x30..0x66，转为 uint8_t 无符号损失
    return static_cast<uint8_t>(ch);
}

bool is_upper_alpha(uint8_t ch) noexcept
{
    return (ch >= static_cast<uint8_t>('A')) && (ch <= static_cast<uint8_t>('Z'));
}

bool is_digit_char(uint8_t ch) noexcept
{
    return (ch >= static_cast<uint8_t>('0')) && (ch <= static_cast<uint8_t>('9'));
}

bool is_printable_body_char(uint8_t ch, bool forbid_hash) noexcept
{
    if ((ch < kAsciiPrintMin) || (ch > kAsciiPrintMax)) {
        return false;
    }
    if ((ch == static_cast<uint8_t>('$')) || (ch == static_cast<uint8_t>('*'))) {
        return false;
    }
    if (forbid_hash && (ch == static_cast<uint8_t>('#'))) {
        return false;
    }
    return true;
}

void write_u16_le(std::span<uint8_t> dst, std::size_t offset, uint16_t val) noexcept
{
    // uint16_t 扩宽到 unsigned int 后移位，避免整型提升为有符号 int
    const auto u = static_cast<unsigned int>(val);
    dst[offset + kOfsByte0] = static_cast<uint8_t>(u & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte1] =
        static_cast<uint8_t>((u >> kShift8) & kByteMask); // 右移并掩码后范围为 0..255
}

void write_u32_le(std::span<uint8_t> dst, std::size_t offset, uint32_t val) noexcept
{
    dst[offset + kOfsByte0] = static_cast<uint8_t>(val & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte1] =
        static_cast<uint8_t>((val >> kShift8) & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte2] =
        static_cast<uint8_t>((val >> kShift16) & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte3] =
        static_cast<uint8_t>((val >> kShift24) & kByteMask); // 掩码后范围为 0..255
}

uint32_t crc32_reflected_raw(uint32_t init, std::span<const uint8_t> data) noexcept
{
    uint32_t crc = init;
    for (const uint8_t byte : data) {
        crc ^= static_cast<uint32_t>(byte);
        for (uint32_t bit = 0U; bit < kBitsPerByte; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ kCrc32ReflPoly;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

bool validate_unicore_hash_body(std::string_view body, std::size_t max_body) noexcept
{
    if (body.empty() || (body.size() > max_body)) {
        return false;
    }
    // 将 char 的位模式按无符号字节解释，范围 0..255
    if (!is_upper_alpha(static_cast<uint8_t>(body[0]))) {
        return false;
    }
    for (const char ch : body) {
        // char 位模式按无符号字节 0..255 解释
        if (!is_printable_body_char(static_cast<uint8_t>(ch), true)) {
            return false;
        }
    }
    return true;
}

} // namespace

uint32_t crc24q(std::span<const uint8_t> data) noexcept
{
    uint32_t crc = 0U;
    for (const uint8_t byte : data) {
        crc ^= static_cast<uint32_t>(byte) << 16U;
        for (uint32_t bit = 0U; bit < kBitsPerByte; ++bit) {
            crc <<= 1U;
            if ((crc & kCrc24qTopBit) != 0U) {
                crc ^= kCrc24qPoly;
            }
        }
        crc &= kCrc24qMask;
    }
    return crc & kCrc24qMask;
}

uint32_t crc32_ieee(std::span<const uint8_t> data) noexcept
{
    if (data.empty()) {
        return 0U;
    }
    return crc32_reflected_raw(kCrc32InitIeee, data) ^ kCrc32InitIeee;
}

uint32_t crc32_novatel(std::span<const uint8_t> data) noexcept
{
    if (data.empty()) {
        return 0U;
    }
    return crc32_reflected_raw(0U, data);
}

uint8_t xor_checksum(std::span<const uint8_t> data) noexcept
{
    uint8_t acc = 0U;
    for (const uint8_t byte : data) {
        acc ^= byte;
    }
    return acc;
}

UbxChecksum ubx_checksum(std::span<const uint8_t> data) noexcept
{
    uint32_t a = 0U;
    uint32_t b = 0U;
    for (const uint8_t byte : data) {
        a = (a + static_cast<uint32_t>(byte)) & kByteMask;
        b = (b + a) & kByteMask;
    }
    // 均经 & kByteMask 掩码，范围为 0..255
    return UbxChecksum{static_cast<uint8_t>(a), static_cast<uint8_t>(b)};
}

bool gen_rtcm3(std::span<uint8_t> out, std::span<const uint8_t> payload,
               std::size_t &out_len) noexcept
{
    if (payload.size() > kRtcm3MaxPayload) {
        return false;
    }
    const std::size_t total = kRtcm3Overhead + payload.size();
    if (out.size() < total) {
        return false;
    }

    out[kOfsByte0] = kRtcm3Sync;
    // payload.size() <= 1023，右移 8 位与 0x03 掩码后范围为 0..3，高 6 位固定为 0
    out[kOfsByte1] = static_cast<uint8_t>((payload.size() >> kShift8) & kRtcm3HiBitsMask);
    // 掩码后范围为 0..255
    out[kOfsByte2] = static_cast<uint8_t>(payload.size() & kByteMask);
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), out.subspan(kRtcm3HdrLen).begin());
    }

    const std::size_t body_len = kRtcm3HdrLen + payload.size();
    const uint32_t crc = crc24q(out.subspan(0, body_len));
    out[body_len + kOfsByte0] =
        static_cast<uint8_t>((crc >> kShift16) & kByteMask); // 大端高字节，范围 0..255
    out[body_len + kOfsByte1] =
        static_cast<uint8_t>((crc >> kShift8) & kByteMask);            // 大端中字节，范围 0..255
    out[body_len + kOfsByte2] = static_cast<uint8_t>(crc & kByteMask); // 大端低字节，范围 0..255
    out_len = total;
    return true;
}

bool gen_nmea(std::span<uint8_t> out, std::string_view body, bool lowercase_hex,
              std::size_t &out_len) noexcept
{
    if (body.empty() || (body.size() > (kNmeaMaxFrame - 1U - kNmeaTrailerLen))) {
        return false;
    }
    // 首字符按无符号字节 0..255 解释
    const auto first = static_cast<uint8_t>(body[0]);
    if (!is_upper_alpha(first) && !is_digit_char(first)) {
        return false;
    }
    for (const char ch : body) {
        // 按无符号字节 0..255 解释
        if (!is_printable_body_char(static_cast<uint8_t>(ch), false)) {
            return false;
        }
    }

    const std::size_t total = 1U + body.size() + kNmeaTrailerLen;
    if (out.size() < total) {
        return false;
    }

    out[kOfsByte0] = static_cast<uint8_t>('$');
    for (std::size_t i = 0U; i < body.size(); ++i) {
        // ASCII 可打印字节 0x20..0x7E
        out[1U + i] = static_cast<uint8_t>(body[i]);
    }
    const uint8_t chk = xor_checksum(out.subspan(1U, body.size()));
    const auto chk_u = static_cast<unsigned int>(chk);
    const std::size_t star_pos = 1U + body.size();
    out[star_pos] = static_cast<uint8_t>('*');
    // chk_u 右移 4 位后在 0..15
    out[star_pos + kTrailerChkHi] =
        nibble_to_hex_byte(static_cast<uint8_t>(chk_u >> kNibbleBits), lowercase_hex);
    out[star_pos + kTrailerChkLo] = nibble_to_hex_byte(chk, lowercase_hex);
    out[star_pos + kTrailerCr] = static_cast<uint8_t>('\r');
    out[star_pos + kTrailerLf] = static_cast<uint8_t>('\n');
    out_len = total;
    return true;
}

bool gen_ubx(std::span<uint8_t> out, uint8_t msg_class, uint8_t msg_id,
             std::span<const uint8_t> payload, std::size_t &out_len) noexcept
{
    if (payload.size() > kUbxMaxPayload) {
        return false;
    }
    const std::size_t total = kUbxOverhead + payload.size();
    if (out.size() < total) {
        return false;
    }

    out[kOfsByte0] = kUbxSync1;
    out[kOfsByte1] = kUbxSync2;
    out[kOfsUbxClass] = msg_class;
    out[kOfsUbxId] = msg_id;
    // 已校验 payload.size() <= 4088，在 uint16_t 范围内
    write_u16_le(out, kOfsUbxLen, static_cast<uint16_t>(payload.size()));
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), out.subspan(kUbxHdrLen).begin());
    }
    const UbxChecksum ck =
        ubx_checksum(out.subspan(kOfsUbxClass, kUbxHdrLen - kOfsUbxClass + payload.size()));
    out[kUbxHdrLen + payload.size()] = ck.ck_a;
    out[kUbxHdrLen + payload.size() + 1U] = ck.ck_b;
    out_len = total;
    return true;
}

bool gen_unicore_bin(std::span<uint8_t> out, uint16_t msg_id, uint8_t cpu_idle,
                     std::span<const uint8_t> hdr_rest_16, std::span<const uint8_t> payload,
                     std::size_t &out_len) noexcept
{
    if (payload.size() > kUnicoreBinMaxPayload) {
        return false;
    }
    if (!hdr_rest_16.empty() && (hdr_rest_16.size() != kUnicoreHdrRest)) {
        return false;
    }
    const std::size_t total = kUnicoreOverhead + payload.size();
    if (out.size() < total) {
        return false;
    }

    out[kOfsByte0] = kUnicoreSync1;
    out[kOfsByte1] = kUnicoreSync2;
    out[kOfsByte2] = kUnicoreSync3;
    out[kOfsUnicoreIdle] = cpu_idle;
    write_u16_le(out, kOfsUnicoreMsgId, msg_id);
    // 已校验 payload.size() <= 4068，在 uint16_t 范围内
    write_u16_le(out, kOfsUnicoreLen, static_cast<uint16_t>(payload.size()));
    if (!hdr_rest_16.empty()) {
        std::copy(hdr_rest_16.begin(), hdr_rest_16.end(), out.subspan(kOfsUnicoreRest).begin());
    } else {
        std::fill_n(out.subspan(kOfsUnicoreRest).begin(), kUnicoreHdrRest, uint8_t{0});
    }
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), out.subspan(kUnicoreHdrLen).begin());
    }
    const std::size_t crc_input_len = kUnicoreHdrLen + payload.size();
    const uint32_t crc = crc32_novatel(out.subspan(0, crc_input_len));
    write_u32_le(out, crc_input_len, crc);
    out_len = total;
    return true;
}

bool gen_unicore_hash_crc32(std::span<uint8_t> out, std::string_view body, bool lowercase_hex,
                            std::size_t &out_len) noexcept
{
    if (!validate_unicore_hash_body(body, kUnicoreHashMaxFrame - 1U - kHashCrcTrailer)) {
        return false;
    }
    const std::size_t total = 1U + body.size() + kHashCrcTrailer;
    if (out.size() < total) {
        return false;
    }

    out[kOfsByte0] = static_cast<uint8_t>('#');
    for (std::size_t i = 0U; i < body.size(); ++i) {
        // ASCII 可打印字节 0x20..0x7E
        out[1U + i] = static_cast<uint8_t>(body[i]);
    }
    const uint32_t crc = crc32_novatel(out.subspan(1U, body.size()));
    const std::size_t star_pos = 1U + body.size();
    out[star_pos] = static_cast<uint8_t>('*');
    for (std::size_t i = 0U; i < kHashCrcDigits; ++i) {
        // i 范围为 0..7，位移量范围为 28..0
        const uint32_t shift =
            (static_cast<uint32_t>(kHashCrcDigits - 1U) - static_cast<uint32_t>(i)) * kNibbleBits;
        // 掩码后范围为 0..15
        const auto nib = static_cast<uint8_t>((crc >> shift) & kNibbleMask);
        out[star_pos + 1U + i] = nibble_to_hex_byte(nib, lowercase_hex);
    }
    out[star_pos + kHashTrailerCr] = static_cast<uint8_t>('\r');
    out[star_pos + kHashTrailerLf] = static_cast<uint8_t>('\n');
    out_len = total;
    return true;
}

bool gen_unicore_hash_xor(std::span<uint8_t> out, std::string_view body, bool lowercase_hex,
                          std::size_t &out_len) noexcept
{
    if (!validate_unicore_hash_body(body, kUnicoreHashMaxFrame - 1U - kNmeaTrailerLen)) {
        return false;
    }
    const std::size_t total = 1U + body.size() + kNmeaTrailerLen;
    if (out.size() < total) {
        return false;
    }

    out[kOfsByte0] = static_cast<uint8_t>('#');
    for (std::size_t i = 0U; i < body.size(); ++i) {
        // ASCII 可打印字节 0x20..0x7E
        out[1U + i] = static_cast<uint8_t>(body[i]);
    }
    const uint8_t chk = xor_checksum(out.subspan(1U, body.size()));
    const auto chk_u = static_cast<unsigned int>(chk);
    const std::size_t star_pos = 1U + body.size();
    out[star_pos] = static_cast<uint8_t>('*');
    // chk_u 右移 4 位后在 0..15
    out[star_pos + kTrailerChkHi] =
        nibble_to_hex_byte(static_cast<uint8_t>(chk_u >> kNibbleBits), lowercase_hex);
    out[star_pos + kTrailerChkLo] = nibble_to_hex_byte(chk, lowercase_hex);
    out[star_pos + kTrailerCr] = static_cast<uint8_t>('\r');
    out[star_pos + kTrailerLf] = static_cast<uint8_t>('\n');
    out_len = total;
    return true;
}

bool gen_product(std::span<uint8_t> out, uint16_t msg_id, uint8_t sender, uint8_t type,
                 std::span<const uint8_t> payload, std::size_t &out_len) noexcept
{
    return proto::buildFrame(out, msg_id, sender, type, payload, out_len) == fw::Status::kOk;
}

} // namespace demux_test
