// Unicore Hash 语句解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：
// '#' | 第 2 字节 [A-Z] | 正文可打印 ASCII 且不含 '#' '$' '*' | '*' 后 8 位十六进制 CRC-32（同 unicore_bin
// 算法，覆盖 '#' 与 '*' 之间）或 2 位十六进制 XOR | CR LF。
// 早拒绝点：第 2 字节不是大写字母；正文出现非法字符；'*' 之后的偏移不是该位置的十六进制/CR。
// 帧长未知，因此每字节只查“新到字节”与定长尾部窗口（11 B）内的 '*' 相对偏移，O(1) 且结论只依赖入参；
// '\n' 到达时按 '*' 的偏移（4 = XOR 形式 / 10 = CRC 形式）二选一，整段复核正文并整段算校验。
// step 与 explain 共用 judge()，避免诊断文本与判定结论漂移。

#include "demux/decoders/UnicoreHash.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/TextWriter.hpp"
#include "demux/Crc.hpp"
#include "demux/Decoder.hpp"

namespace demux::unicore_hash {

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

/** 拒绝原因；explain 据此生成一句话。 */
enum class Reason : uint8_t {
    kNone,
    kSync,
    kSecond,
    kMissingStar,
    kStarOffset,
    kBody,
    kChecksumByte,
    kNoCr,
    kBadHex,
    kXorChecksum,
    kCrcChecksum,
    kTooLong,
};

struct Verdict {
    Step step = Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;
    std::size_t offset = 0U; // 出错字节下标
    uint32_t received = 0U;  // 校验/字节类原因：收到的值
    uint32_t computed = 0U;  // 校验类原因：算出的值
};

Verdict reject(Reason reason, std::size_t offset, uint8_t byte = 0U) noexcept
{
    Verdict verdict{};
    verdict.step = Step::kReject;
    verdict.reason = reason;
    verdict.offset = offset;
    verdict.received = static_cast<uint32_t>(byte);
    return verdict;
}

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
Verdict finish(std::span<const uint8_t> frame, std::size_t star) noexcept
{
    for (std::size_t idx = 1U; idx < star; ++idx) {
        if (!is_body(frame[idx])) {
            return reject(Reason::kBody, idx, frame[idx]);
        }
    }

    const std::size_t len = frame.size();
    if ((len - 1U - star) == kStarOfsXor) {
        // 形式一：2 位十六进制 XOR，覆盖 '#' 与 '*' 之间。
        // 各十六进制位 ≤ 0x0F，左移 4 位后 ≤ 0xF0，结果 ≤ 0xFF，窄化安全。
        const uint8_t checksum_rx =
            static_cast<uint8_t>((static_cast<uint32_t>(hex_value(frame[star + 1U])) << kNibbleBits) |
                                 static_cast<uint32_t>(hex_value(frame[star + 2U])));
        const uint8_t checksum_calc = xor8(frame.subspan(1U, star - 1U));
        if (checksum_calc != checksum_rx) {
            Verdict bad = reject(Reason::kXorChecksum, star + 1U, checksum_rx);
            bad.computed = static_cast<uint32_t>(checksum_calc);
            return bad;
        }
    } else {
        // 形式二：8 位十六进制 CRC-32，与 Unicore 二进制相同的算法。
        const uint32_t checksum_rx = read_hex_be(frame, star + 1U, kCrcDigits);
        const uint32_t checksum_calc = crc::crc32_novatel(frame.subspan(1U, star - 1U));
        if (checksum_calc != checksum_rx) {
            Verdict bad = reject(Reason::kCrcChecksum, star + 1U, 0U);
            bad.received = checksum_rx;
            bad.computed = checksum_calc;
            return bad;
        }
    }
    Verdict good{};
    good.step = Step::kFrame;
    return good;
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

/** 判定：step 与 explain 的唯一事实来源。 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    if (frame[0] != kSyncByte) {
        return reject(Reason::kSync, 0U, frame[0]);
    }
    const std::size_t last = frame.size() - 1U;
    const uint8_t new_byte = frame[last];

    if (frame.size() < 2U) {
        verdict.need = 2U - frame.size();
        return verdict;
    }
    if (!is_upper_alpha(frame[1])) {
        // 第 2 字节必须是语句名首字母（大写字母）：与同样以 '#' 开头的其他语法早分开。
        return reject(Reason::kSecond, 1U, frame[1]);
    }
    if (frame.size() < kMinLen) {
        verdict.need = kMinLen - frame.size();
        return verdict;
    }

    std::size_t star_ofs = 0U;
    if (new_byte == kLf) {
        // 最近的 '*' 就是校验字段起点；两种形式的 '*' 偏移都是定值。
        if (!find_star_reverse(frame, last, kStarWindow, star_ofs)) {
            return reject(Reason::kMissingStar, last, new_byte);
        }
        if ((star_ofs != kStarOfsXor) && (star_ofs != kStarOfsCrc)) {
            return reject(Reason::kStarOffset, last - star_ofs, frame[last - star_ofs]);
        }
        if (frame[last - 1U] != kCr) {
            return reject(Reason::kNoCr, last - 1U, frame[last - 1U]);
        }
        const std::size_t star = last - star_ofs; // star_ofs ≤ last，不越界
        if (star_ofs == kStarOfsXor) {
            if (!is_hex_digit(frame[star + 1U])) {
                return reject(Reason::kBadHex, star + 1U, frame[star + 1U]);
            }
            if (!is_hex_digit(frame[star + 2U])) {
                return reject(Reason::kBadHex, star + 2U, frame[star + 2U]);
            }
        } else if (!all_hex(frame, star + 1U, kCrcDigits)) {
            for (std::size_t idx = 0U; idx < kCrcDigits; ++idx) {
                if (!is_hex_digit(frame[star + 1U + idx])) {
                    return reject(Reason::kBadHex, star + 1U + idx, frame[star + 1U + idx]);
                }
            }
        } else {
            // all_hex 已通过，校验字段都是十六进制。
        }
        return finish(frame, star);
    }

    // 未到结束符：只查新到字节与其前面定长窗口内的 '*' 位置（O(1)）。
    if (!find_star_reverse(frame, last, kStarWindow, star_ofs)) {
        if (!is_body(new_byte)) {
            return reject(Reason::kBody, last, new_byte);
        }
    } else if ((star_ofs != 0U) && (!mid_byte_ok(frame, last, last - star_ofs, new_byte))) {
        // 偏移 0 是 '*' 本身（校验字段开始），由下一个字节起再预检。
        return reject(Reason::kChecksumByte, last, new_byte);
    } else {
        // 正文字节或 '*' 合法，继续累积。
    }

    if ((frame.size() + 1U) > kMaxFrame) {
        // 超过 max_frame 仍未结束：need 契约禁止要求超限字节，只能拒绝。
        return reject(Reason::kTooLong, last);
    }
    verdict.need = 1U;
    return verdict;
}

void explain_verdict(const Verdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case Reason::kSync:
        text.literal("byte 0 is ").byte_value(static_cast<uint8_t>(verdict.received)).literal(", expected '#'");
        break;
    case Reason::kSecond:
        text.literal("byte 1 is ").byte_value(static_cast<uint8_t>(verdict.received)).literal(", not 'A'-'Z'");
        break;
    case Reason::kMissingStar:
        text.literal("no '*' before LF at offset ").decimal(verdict.offset);
        break;
    case Reason::kStarOffset:
        text.literal("checksum starts at offset ")
            .decimal(verdict.offset)
            .literal(", expected 4 or 10 bytes before LF");
        break;
    case Reason::kBody:
        text.literal("illegal byte ")
            .byte_value(static_cast<uint8_t>(verdict.received))
            .literal(" at offset ")
            .decimal(verdict.offset);
        break;
    case Reason::kChecksumByte:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byte_value(static_cast<uint8_t>(verdict.received))
            .literal(", not a checksum digit or CR");
        break;
    case Reason::kNoCr:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byte_value(static_cast<uint8_t>(verdict.received))
            .literal(", expected CR");
        break;
    case Reason::kBadHex:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byte_value(static_cast<uint8_t>(verdict.received))
            .literal(", not a hex digit");
        break;
    case Reason::kXorChecksum:
        text.literal("bad XOR checksum, received ")
            .hex(verdict.received, static_cast<uint8_t>(kXorDigits))
            .literal(", computed ")
            .hex(verdict.computed, static_cast<uint8_t>(kXorDigits));
        break;
    case Reason::kCrcChecksum:
        text.literal("bad CRC32, received ")
            .hex(verdict.received, static_cast<uint8_t>(kCrcDigits))
            .literal(", computed ")
            .hex(verdict.computed, static_cast<uint8_t>(kCrcDigits));
        break;
    case Reason::kTooLong:
        text.literal("frame longer than ").decimal(kMaxFrame).literal(" bytes");
        break;
    case Reason::kNone:
    default:
        text.literal("rejected");
        break;
    }
}

} // namespace

bool first_byte(uint8_t byte) noexcept
{
    return byte == kSyncByte;
}

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 first_byte。
    const Verdict verdict = judge(frame);
    need = verdict.need;
    return verdict.step;
}

std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    demux::text_writer::TextWriter text(out);
    if (frame.empty()) {
        return text.literal("empty prefix").finish();
    }
    const Verdict verdict = judge(frame);
    if (verdict.step == Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explain_verdict(verdict, text);
    return text.finish();
}

} // namespace demux::unicore_hash
