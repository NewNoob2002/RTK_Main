// Unicore Hash 语句解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：
// '#' | 第 2 字节 [A-Z] | 正文可打印 ASCII 且不含 '#' '$' '*' | '*' 后 8 位十六进制 CRC-32（同 unicore_bin
// 算法，覆盖 '#' 与 '*' 之间）或 2 位十六进制 XOR | CR LF。
// 早拒绝点：第 2 字节不是大写字母；正文出现非法字符；'*' 之后的偏移不是该位置的十六进制/CR。
// 帧长未知，因此每字节只查“新到字节”与定长尾部窗口（11 B）内的 '*' 相对偏移，O(1) 且结论只依赖入参；
// '\n' 到达时按 '*' 的偏移（4 = XOR 形式 / 10 = CRC 形式）二选一，整段复核正文并整段算校验。
//
// Generated-by: pi + workbuddy/deepseek-v4.1-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "rtk/demux/decoders/unicore_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/demux/crc.hpp"
#include "rtk/demux/decoder.hpp"

namespace rtk::demux::unicore_hash {

namespace {

constexpr uint8_t kSyncByte = 0x23U; // '#'
constexpr uint8_t kStar = 0x2AU;     // '*'
constexpr uint8_t kCr = 0x0DU;
constexpr uint8_t kLf = 0x0AU;
constexpr uint8_t kDollar = 0x24U; // '$'
constexpr uint8_t kPrintableMin = 0x20U;
constexpr uint8_t kPrintableMax = 0x7EU;
constexpr uint8_t kDigit0 = 0x30U; // '0'
constexpr uint8_t kDigit9 = 0x39U; // '9'
constexpr uint8_t kUpperA = 0x41U; // 'A'
constexpr uint8_t kUpperF = 0x46U; // 'F'
constexpr uint8_t kUpperZ = 0x5AU; // 'Z'
constexpr uint8_t kLowerA = 0x61U; // 'a'
constexpr uint8_t kLowerF = 0x66U; // 'f'
constexpr uint8_t kHexAlphaBase = 10U;

// 尾部以 '\n' 为参照的 '*' 偏移（由近及远搜索，先命中较短的 XOR 形式）。
constexpr std::size_t kStarOfsXor = 4U;  // "*hh\r\n"
constexpr std::size_t kStarOfsCrc = 10U; // "*hhhhhhhh\r\n"
constexpr std::size_t kStarWindow = kStarOfsCrc;
constexpr std::size_t kCrcDigits = 8U;
constexpr std::size_t kXorDigits = 2U;
// '*' 之后第 3 / 第 9 字节：XOR 形式为 '\r'，CRC 形式仍为十六进制 / '\r'。
constexpr std::size_t kOfsBranch = 3U;
constexpr std::size_t kOfsCr = 9U;
constexpr std::size_t kMinLen = 3U; // "#X\r\n" 至少 "#X*" 之后还有校验字节
constexpr uint32_t kNibbleBits = 4U;

bool is_printable(uint8_t byte) noexcept
{
    return (byte >= kPrintableMin) && (byte <= kPrintableMax);
}

bool is_hex_digit(uint8_t byte) noexcept
{
    return ((byte >= kDigit0) && (byte <= kDigit9)) || ((byte >= kUpperA) && (byte <= kUpperF)) ||
           ((byte >= kLowerA) && (byte <= kLowerF));
}

bool is_upper_alpha(uint8_t byte) noexcept
{
    return (byte >= kUpperA) && (byte <= kUpperZ);
}

/** 调用方必须先经 is_hex_digit() 判定；返回值 0..15。 */
uint8_t hex_value(uint8_t byte) noexcept
{
    if ((byte >= kDigit0) && (byte <= kDigit9)) {
        // 差 0..9，窄化到 uint8_t 安全。
        return static_cast<uint8_t>(byte - kDigit0);
    }
    if ((byte >= kUpperA) && (byte <= kUpperF)) {
        // 差 0..5，加 10 后 ≤ 15，窄化安全。
        return static_cast<uint8_t>((byte - kUpperA) + kHexAlphaBase);
    }
    // 小写形式，同上 ≤ 15。
    return static_cast<uint8_t>((byte - kLowerA) + kHexAlphaBase);
}

/** Hash 正文比 NMEA 更严：可打印且不含 '#' '$' '*'。 */
bool is_body(uint8_t byte) noexcept
{
    return is_printable(byte) && (byte != kSyncByte) && (byte != kDollar) && (byte != kStar);
}

uint8_t xor8(std::span<const uint8_t> data) noexcept
{
    uint8_t acc = 0U;

    for (const uint8_t byte : data) {
        // 两个 uint8_t 异或 < 256，窄化回 uint8_t 安全。
        acc = static_cast<uint8_t>(acc ^ byte);
    }
    return acc;
}

/** 在 frame[last-window .. last] 内反向寻找最近的 '*'；窗口定长，因此 O(1)。 */
bool find_star_reverse(std::span<const uint8_t> frame, std::size_t last, std::size_t window, std::size_t &ofs) noexcept
{
    ofs = 0U;
    for (std::size_t back = 0U; (back <= window) && (back <= last); ++back) {
        if (frame[last - back] == kStar) {
            ofs = back;
            return true;
        }
    }
    return false;
}

/** 读 count 位大端十六进制（count ≤ 8）；调用方保证范围合法且都是十六进制字符。 */
uint32_t read_hex_be(std::span<const uint8_t> frame, std::size_t from, std::size_t count) noexcept
{
    uint32_t value = 0U;

    for (std::size_t idx = 0U; idx < count; ++idx) {
        value = (value << kNibbleBits) | static_cast<uint32_t>(hex_value(frame[from + idx]));
    }
    return value;
}

bool all_hex(std::span<const uint8_t> frame, std::size_t from, std::size_t count) noexcept
{
    for (std::size_t idx = 0U; idx < count; ++idx) {
        if (!is_hex_digit(frame[from + idx])) {
            return false;
        }
    }
    return true;
}

/** '\n' 到达时的整段复核：正文合法性 + 两种校验形式之一。star 为 '*' 的下标。 */
Step finish(std::span<const uint8_t> frame, std::size_t star) noexcept
{
    for (std::size_t idx = 1U; idx < star; ++idx) {
        if (!is_body(frame[idx])) {
            return Step::kReject;
        }
    }

    const std::size_t len = frame.size();
    bool checksum_ok = false;
    if ((len - 1U - star) == kStarOfsXor) {
        // 形式一：2 位十六进制 XOR，覆盖 '#' 与 '*' 之间。
        // 各十六进制位 ≤ 0x0F，左移 4 位后 ≤ 0xF0，结果 ≤ 0xFF，窄化安全。
        const uint8_t checksum_rx =
            static_cast<uint8_t>((static_cast<uint32_t>(hex_value(frame[star + 1U])) << kNibbleBits) |
                                 static_cast<uint32_t>(hex_value(frame[star + 2U])));
        checksum_ok = (xor8(frame.subspan(1U, star - 1U)) == checksum_rx);
    } else {
        // 形式二：8 位十六进制 CRC-32，与 Unicore 二进制相同的算法。
        checksum_ok = (crc::crc32_novatel(frame.subspan(1U, star - 1U)) == read_hex_be(frame, star + 1U, kCrcDigits));
    }
    return checksum_ok ? Step::kFrame : Step::kReject;
}

/** 未到结束符时，按新到字节相对 '*' 的偏移做 O(1) 预检（after_star ≥ 1，即当前字节不是 '*'）。 */
bool mid_byte_ok(std::span<const uint8_t> frame, std::size_t last, std::size_t star, uint8_t new_byte) noexcept
{
    const std::size_t after_star = last - star;

    if (after_star <= kXorDigits) {
        // 两种形式的前两位都是十六进制。
        return is_hex_digit(new_byte);
    }
    if (after_star == kOfsBranch) {
        // XOR 形式到此结束应为 '\r'；CRC 形式继续是十六进制。
        return is_hex_digit(new_byte) || (new_byte == kCr);
    }
    if (after_star <= kCrcDigits) {
        // 4..8：只有 CRC 形式能走到这里；第 3 字节已是 '\r' 说明 XOR 形式早该结束。
        if (frame[star + kOfsBranch] == kCr) {
            return false;
        }
        return is_hex_digit(new_byte);
    }
    if (after_star == kOfsCr) {
        // CRC 形式 8 位十六进制之后的 '\r'。
        if (frame[star + kOfsBranch] == kCr) {
            return false;
        }
        return new_byte == kCr;
    }
    // 更远的偏移只可能是 '\n'（结束符由调用方单独处理）。
    return false;
}

} // namespace

bool first_byte(uint8_t byte) noexcept
{
    return byte == kSyncByte;
}

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 first_byte。
    const std::size_t last = frame.size() - 1U;
    const uint8_t new_byte = frame[last];

    if (frame.size() < 2U) {
        need = 2U - frame.size();
        return Step::kNeedMore;
    }
    if (!is_upper_alpha(frame[1])) {
        // 第 2 字节必须是语句名首字母（大写字母）：与同样以 '#' 开头的其他语法早分开。
        return Step::kReject;
    }
    if (frame.size() < kMinLen) {
        need = kMinLen - frame.size();
        return Step::kNeedMore;
    }

    std::size_t star_ofs = 0U;
    if (new_byte == kLf) {
        // 最近的 '*' 就是校验字段起点；两种形式的 '*' 偏移都是定值。
        if (!find_star_reverse(frame, last, kStarWindow, star_ofs)) {
            return Step::kReject;
        }
        if ((star_ofs != kStarOfsXor) && (star_ofs != kStarOfsCrc)) {
            return Step::kReject;
        }
        if (frame[last - 1U] != kCr) {
            return Step::kReject;
        }
        const std::size_t star = last - star_ofs; // star_ofs ≤ last，不越界
        if (star_ofs == kStarOfsXor) {
            if ((!is_hex_digit(frame[star + 1U])) || (!is_hex_digit(frame[star + 2U]))) {
                return Step::kReject;
            }
        } else if (!all_hex(frame, star + 1U, kCrcDigits)) {
            return Step::kReject;
        }
        return finish(frame, star);
    }

    // 未到结束符：只查新到字节与其前面定长窗口内的 '*' 位置（O(1)）。
    if (!find_star_reverse(frame, last, kStarWindow, star_ofs)) {
        if (!is_body(new_byte)) {
            return Step::kReject;
        }
    } else if ((star_ofs != 0U) && (!mid_byte_ok(frame, last, last - star_ofs, new_byte))) {
        // 偏移 0 是 '*' 本身（校验字段开始），由下一个字节起再预检。
        return Step::kReject;
    } else {
        // 正文字节或 '*' 合法，继续累积。
    }

    if ((frame.size() + 1U) > kMaxFrame) {
        // 超过 max_frame 仍未结束：need 契约禁止要求超限字节，只能拒绝。
        return Step::kReject;
    }
    need = 1U;
    return Step::kNeedMore;
}

} // namespace rtk::demux::unicore_hash
