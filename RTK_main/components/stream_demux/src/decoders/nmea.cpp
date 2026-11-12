// NMEA 0183 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：
// '$' | 第 2 字节 [A-Z0-9] | 正文可打印 ASCII 0x20–0x7E 且不含 '$' | '*' + 2 位十六进制 XOR | CR LF。
// 早拒绝点：第 2 字节不是发送方标识；正文出现非法字符（不可打印、'$'）；'*' 之后不是十六进制/CR/LF。
// 帧长未知，因此每字节只查“新到字节”与定长尾部窗口（5 B）内的 '*' 相对偏移，O(1) 且结论只依赖入参；
// '\n' 到达时再整段复核正文并整段算 XOR，保证 kFrame 结论与调用历史无关。

#include "demux/decoders/nmea.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoder.hpp"

namespace demux::nmea {

namespace {

constexpr uint8_t kSyncByte = 0x24U; // '$'
constexpr uint8_t kStar = 0x2AU;     // '*'
constexpr uint8_t kCr = 0x0DU;
constexpr uint8_t kLf = 0x0AU;
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

// 尾部以 '\n' 为参照的固定偏移：'\n' 0 | '\r' 1 | 低位十六进制 2 | 高位十六进制 3 | '*' 4。
constexpr std::size_t kStarOffset = 4U;
constexpr std::size_t kHexHiOffset = 3U;
constexpr std::size_t kStarWindow = kStarOffset;
// "$" + 发送方标识 + "*hh\r\n" 的最短长度。
constexpr std::size_t kMinLen = 7U;
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

bool is_upper_alnum(uint8_t byte) noexcept
{
    return is_upper_alpha(byte) || ((byte >= kDigit0) && (byte <= kDigit9));
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

/** 正文合法性：可打印且不含 '$'。 */
bool is_body(uint8_t byte) noexcept
{
    return is_printable(byte) && (byte != kSyncByte);
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

/** '\n' 到达时的整段复核：尾部结构、正文合法性、XOR 校验。 */
Step finish(std::span<const uint8_t> frame) noexcept
{
    const std::size_t len = frame.size();
    const std::size_t last = len - 1U;

    if (len < kMinLen) {
        return Step::kReject;
    }
    const std::size_t star = last - kStarOffset; // len ≥ kMinLen 保证 star ≥ 2，不越界
    if ((frame[last - 1U] != kCr) || (!is_hex_digit(frame[last - 2U])) || (!is_hex_digit(frame[last - kHexHiOffset])) ||
        (frame[star] != kStar)) {
        return Step::kReject;
    }
    // 正文（'$' 与 '*' 之间）必须可打印且既不含 '$' 也不含 '*'（'*' 只能出现在校验字段起点）。
    for (std::size_t idx = 1U; idx < star; ++idx) {
        if ((!is_body(frame[idx])) || (frame[idx] == kStar)) {
            return Step::kReject;
        }
    }

    const uint8_t checksum_calc = xor8(frame.subspan(1U, star - 1U));
    // 两位十六进制拼成一个字节：各 ≤ 0x0F，高位左移 4 位后 ≤ 0xF0，结果 ≤ 0xFF，窄化安全。
    const uint8_t checksum_rx =
        static_cast<uint8_t>((static_cast<uint32_t>(hex_value(frame[star + 1U])) << kNibbleBits) |
                             static_cast<uint32_t>(hex_value(frame[star + 2U])));

    return (checksum_calc == checksum_rx) ? Step::kFrame : Step::kReject;
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
    if (!is_upper_alnum(frame[1])) {
        // 第 2 字节必须是发送方标识（大写字母或数字）：与同样以 '$' 开头的其他语法早分开。
        return Step::kReject;
    }
    if (new_byte == kLf) {
        return finish(frame);
    }

    std::size_t star_ofs = 0U;
    if (find_star_reverse(frame, last, kStarWindow, star_ofs)) {
        if (star_ofs == 0U) {
            // 当前字节就是 '*'：校验字段从这里开始。
        } else if (star_ofs < kHexHiOffset) {
            // 偏移 1、2：两位十六进制校验位。
            if (!is_hex_digit(new_byte)) {
                return Step::kReject;
            }
        } else if (star_ofs == kHexHiOffset) {
            // 偏移 3：只能是 '\r'。
            if (new_byte != kCr) {
                return Step::kReject;
            }
        } else {
            // 偏移 4 只可能以 '\n' 结束（上面已处理），其余情况非法。
            return Step::kReject;
        }
    } else if (!is_body(new_byte)) {
        return Step::kReject;
    } else {
        // 正文字节合法，继续累积。
    }

    if ((frame.size() + 1U) > kMaxFrame) {
        // 超过 max_frame 仍未结束：need 契约禁止要求超限字节，只能拒绝。
        return Step::kReject;
    }
    need = 1U;
    return Step::kNeedMore;
}

} // namespace demux::nmea
