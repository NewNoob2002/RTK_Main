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

uint8_t nibbleToHexByte(uint8_t nibble, bool lowercase) noexcept
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

bool isUpperAlpha(uint8_t ch) noexcept
{
    return (ch >= static_cast<uint8_t>('A')) && (ch <= static_cast<uint8_t>('Z'));
}

bool isDigitChar(uint8_t ch) noexcept
{
    return (ch >= static_cast<uint8_t>('0')) && (ch <= static_cast<uint8_t>('9'));
}

bool isPrintableBodyChar(uint8_t ch, bool forbidHash) noexcept
{
    if ((ch < kAsciiPrintMin) || (ch > kAsciiPrintMax)) {
        return false;
    }
    if ((ch == static_cast<uint8_t>('$')) || (ch == static_cast<uint8_t>('*'))) {
        return false;
    }
    if (forbidHash && (ch == static_cast<uint8_t>('#'))) {
        return false;
    }
    return true;
}

void writeU16Le(std::span<uint8_t> dst, std::size_t offset, uint16_t val) noexcept
{
    // uint16_t 扩宽到 unsigned int 后移位，避免整型提升为有符号 int
    const auto u = static_cast<unsigned int>(val);
    dst[offset + kOfsByte0] = static_cast<uint8_t>(u & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte1] =
        static_cast<uint8_t>((u >> kShift8) & kByteMask); // 右移并掩码后范围为 0..255
}

void writeU32Le(std::span<uint8_t> dst, std::size_t offset, uint32_t val) noexcept
{
    dst[offset + kOfsByte0] = static_cast<uint8_t>(val & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte1] =
        static_cast<uint8_t>((val >> kShift8) & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte2] =
        static_cast<uint8_t>((val >> kShift16) & kByteMask); // 掩码后范围为 0..255
    dst[offset + kOfsByte3] =
        static_cast<uint8_t>((val >> kShift24) & kByteMask); // 掩码后范围为 0..255
}

uint32_t crc32ReflectedRaw(uint32_t init, std::span<const uint8_t> data) noexcept
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

bool validateUnicoreHashBody(std::string_view body, std::size_t maxBody) noexcept
{
    if (body.empty() || (body.size() > maxBody)) {
        return false;
    }
    // 将 char 的位模式按无符号字节解释，范围 0..255
    if (!isUpperAlpha(static_cast<uint8_t>(body[0]))) {
        return false;
    }
    for (const char ch : body) {
        // char 位模式按无符号字节 0..255 解释
        if (!isPrintableBodyChar(static_cast<uint8_t>(ch), true)) {
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

uint32_t crc32Ieee(std::span<const uint8_t> data) noexcept
{
    if (data.empty()) {
        return 0U;
    }
    return crc32ReflectedRaw(kCrc32InitIeee, data) ^ kCrc32InitIeee;
}

uint32_t crc32Novatel(std::span<const uint8_t> data) noexcept
{
    if (data.empty()) {
        return 0U;
    }
    return crc32ReflectedRaw(0U, data);
}

uint8_t xorChecksum(std::span<const uint8_t> data) noexcept
{
    uint8_t acc = 0U;
    for (const uint8_t byte : data) {
        acc ^= byte;
    }
    return acc;
}

UbxChecksum ubxChecksum(std::span<const uint8_t> data) noexcept
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

bool genRtcm3(std::span<uint8_t> out, std::span<const uint8_t> payload,
              std::size_t &outLen) noexcept
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

    const std::size_t bodyLen = kRtcm3HdrLen + payload.size();
    const uint32_t crc = crc24q(out.subspan(0, bodyLen));
    out[bodyLen + kOfsByte0] =
        static_cast<uint8_t>((crc >> kShift16) & kByteMask); // 大端高字节，范围 0..255
    out[bodyLen + kOfsByte1] =
        static_cast<uint8_t>((crc >> kShift8) & kByteMask);           // 大端中字节，范围 0..255
    out[bodyLen + kOfsByte2] = static_cast<uint8_t>(crc & kByteMask); // 大端低字节，范围 0..255
    outLen = total;
    return true;
}

bool genNmea(std::span<uint8_t> out, std::string_view body, bool lowercaseHex,
             std::size_t &outLen) noexcept
{
    if (body.empty() || (body.size() > (kNmeaMaxFrame - 1U - kNmeaTrailerLen))) {
        return false;
    }
    // 首字符按无符号字节 0..255 解释
    const auto first = static_cast<uint8_t>(body[0]);
    if (!isUpperAlpha(first) && !isDigitChar(first)) {
        return false;
    }
    for (const char ch : body) {
        // 按无符号字节 0..255 解释
        if (!isPrintableBodyChar(static_cast<uint8_t>(ch), false)) {
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
    const uint8_t chk = xorChecksum(out.subspan(1U, body.size()));
    const auto chkU = static_cast<unsigned int>(chk);
    const std::size_t starPos = 1U + body.size();
    out[starPos] = static_cast<uint8_t>('*');
    // chkU 右移 4 位后在 0..15
    out[starPos + kTrailerChkHi] =
        nibbleToHexByte(static_cast<uint8_t>(chkU >> kNibbleBits), lowercaseHex);
    out[starPos + kTrailerChkLo] = nibbleToHexByte(chk, lowercaseHex);
    out[starPos + kTrailerCr] = static_cast<uint8_t>('\r');
    out[starPos + kTrailerLf] = static_cast<uint8_t>('\n');
    outLen = total;
    return true;
}

bool genUbx(std::span<uint8_t> out, uint8_t msgClass, uint8_t msgId,
            std::span<const uint8_t> payload, std::size_t &outLen) noexcept
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
    out[kOfsUbxClass] = msgClass;
    out[kOfsUbxId] = msgId;
    // 已校验 payload.size() <= 4088，在 uint16_t 范围内
    writeU16Le(out, kOfsUbxLen, static_cast<uint16_t>(payload.size()));
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), out.subspan(kUbxHdrLen).begin());
    }
    const UbxChecksum ck =
        ubxChecksum(out.subspan(kOfsUbxClass, kUbxHdrLen - kOfsUbxClass + payload.size()));
    out[kUbxHdrLen + payload.size()] = ck.ckA;
    out[kUbxHdrLen + payload.size() + 1U] = ck.ckB;
    outLen = total;
    return true;
}

bool genUnicoreBin(std::span<uint8_t> out, uint16_t msgId, uint8_t cpuIdle,
                   std::span<const uint8_t> hdrRest16, std::span<const uint8_t> payload,
                   std::size_t &outLen) noexcept
{
    if (payload.size() > kUnicoreBinMaxPayload) {
        return false;
    }
    if (!hdrRest16.empty() && (hdrRest16.size() != kUnicoreHdrRest)) {
        return false;
    }
    const std::size_t total = kUnicoreOverhead + payload.size();
    if (out.size() < total) {
        return false;
    }

    out[kOfsByte0] = kUnicoreSync1;
    out[kOfsByte1] = kUnicoreSync2;
    out[kOfsByte2] = kUnicoreSync3;
    out[kOfsUnicoreIdle] = cpuIdle;
    writeU16Le(out, kOfsUnicoreMsgId, msgId);
    // 已校验 payload.size() <= 4068，在 uint16_t 范围内
    writeU16Le(out, kOfsUnicoreLen, static_cast<uint16_t>(payload.size()));
    if (!hdrRest16.empty()) {
        std::copy(hdrRest16.begin(), hdrRest16.end(), out.subspan(kOfsUnicoreRest).begin());
    } else {
        std::fill_n(out.subspan(kOfsUnicoreRest).begin(), kUnicoreHdrRest, uint8_t{0});
    }
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), out.subspan(kUnicoreHdrLen).begin());
    }
    const std::size_t crcInputLen = kUnicoreHdrLen + payload.size();
    const uint32_t crc = crc32Novatel(out.subspan(0, crcInputLen));
    writeU32Le(out, crcInputLen, crc);
    outLen = total;
    return true;
}

bool genUnicoreHashCrc32(std::span<uint8_t> out, std::string_view body, bool lowercaseHex,
                         std::size_t &outLen) noexcept
{
    if (!validateUnicoreHashBody(body, kUnicoreHashMaxFrame - 1U - kHashCrcTrailer)) {
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
    const uint32_t crc = crc32Novatel(out.subspan(1U, body.size()));
    const std::size_t starPos = 1U + body.size();
    out[starPos] = static_cast<uint8_t>('*');
    for (std::size_t i = 0U; i < kHashCrcDigits; ++i) {
        // i 范围为 0..7，位移量范围为 28..0
        const uint32_t shift =
            (static_cast<uint32_t>(kHashCrcDigits - 1U) - static_cast<uint32_t>(i)) * kNibbleBits;
        // 掩码后范围为 0..15
        const auto nib = static_cast<uint8_t>((crc >> shift) & kNibbleMask);
        out[starPos + 1U + i] = nibbleToHexByte(nib, lowercaseHex);
    }
    out[starPos + kHashTrailerCr] = static_cast<uint8_t>('\r');
    out[starPos + kHashTrailerLf] = static_cast<uint8_t>('\n');
    outLen = total;
    return true;
}

bool genUnicoreHashXor(std::span<uint8_t> out, std::string_view body, bool lowercaseHex,
                       std::size_t &outLen) noexcept
{
    if (!validateUnicoreHashBody(body, kUnicoreHashMaxFrame - 1U - kNmeaTrailerLen)) {
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
    const uint8_t chk = xorChecksum(out.subspan(1U, body.size()));
    const auto chkU = static_cast<unsigned int>(chk);
    const std::size_t starPos = 1U + body.size();
    out[starPos] = static_cast<uint8_t>('*');
    // chkU 右移 4 位后在 0..15
    out[starPos + kTrailerChkHi] =
        nibbleToHexByte(static_cast<uint8_t>(chkU >> kNibbleBits), lowercaseHex);
    out[starPos + kTrailerChkLo] = nibbleToHexByte(chk, lowercaseHex);
    out[starPos + kTrailerCr] = static_cast<uint8_t>('\r');
    out[starPos + kTrailerLf] = static_cast<uint8_t>('\n');
    outLen = total;
    return true;
}

bool genProduct(std::span<uint8_t> out, uint16_t msgId, uint8_t sender, uint8_t type,
                std::span<const uint8_t> payload, std::size_t &outLen) noexcept
{
    return proto::buildFrame(out, msgId, sender, type, payload, outLen) == fw::Status::kOk;
}

} // namespace demux_test
