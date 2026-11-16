// NMEA 0183 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：
// '$' | 第 2 字节 [A-Z0-9] | 正文可打印 ASCII 0x20–0x7E 且不含 '$' | '*' + 2 位十六进制 XOR | CR LF。
// 早拒绝点：第 2 字节不是发送方标识；正文出现非法字符（不可打印、'$'）；'*' 之后不是十六进制/CR/LF。
// 帧长未知，因此每字节只查“新到字节”与定长尾部窗口（5 B）内的 '*' 相对偏移，O(1) 且结论只依赖入参；
// '\n' 到达时再整段复核正文并整段算 XOR，保证 kFrame 结论与调用历史无关。
// step 与 explain 共用同一个 judge()，避免诊断文本与判定结论漂移。

#include "demux/decoders/Nmea.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/TextWriter.hpp"
#include "demux/Decoder.hpp"

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
constexpr std::size_t kHexLoOffset = 2U;
constexpr std::size_t kStarWindow = kStarOffset;
// "$" + 发送方标识 + "*hh\r\n" 的最短长度。
constexpr std::size_t kMinLen = 7U;
constexpr uint32_t kNibbleBits = 4U;

/** 拒绝原因；explain 据此生成一句话，step 只看 verdict.step。 */
enum class Reason : uint8_t {
    kNone,
    kSync,
    kSecond,
    kShort,
    kMissingStar,
    kBody,
    kNoCrByte,
    kHexByte,
    kChecksum,
    kUnexpected,
    kTooLong,
};

/** judge() 的完整结论：判定、原因与诊断所需的数值。 */
struct Verdict {
    Step step = Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;   // 仅 kNeedMore 有意义
    std::size_t offset = 0U; // 出错字节下标
    uint8_t received = 0U;   // 校验类原因：收到的值
    uint8_t computed = 0U;   // 校验类原因：算出的值
};

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

Verdict reject(Reason reason, std::size_t offset, uint8_t byte = 0U) noexcept
{
    Verdict verdict{};
    verdict.step = Step::kReject;
    verdict.reason = reason;
    verdict.offset = offset;
    verdict.received = byte;
    return verdict;
}

/** '\n' 到达时的整段复核：尾部结构、正文合法性、XOR 校验。 */
Verdict finish(std::span<const uint8_t> frame) noexcept
{
    const std::size_t len = frame.size();
    const std::size_t last = len - 1U;

    if (len < kMinLen) {
        return reject(Reason::kShort, last);
    }
    const std::size_t star = last - kStarOffset; // len ≥ kMinLen 保证 star ≥ 2，不越界
    if (frame[last - 1U] != kCr) {
        return reject(Reason::kNoCrByte, last - 1U, frame[last - 1U]);
    }
    if (!is_hex_digit(frame[last - kHexLoOffset])) {
        return reject(Reason::kHexByte, last - kHexLoOffset, frame[last - kHexLoOffset]);
    }
    if (!is_hex_digit(frame[last - kHexHiOffset])) {
        return reject(Reason::kHexByte, last - kHexHiOffset, frame[last - kHexHiOffset]);
    }
    if (frame[star] != kStar) {
        return reject(Reason::kMissingStar, star, frame[star]);
    }
    // 正文（'$' 与 '*' 之间）必须可打印且既不含 '$' 也不含 '*'（'*' 只能出现在校验字段起点）。
    for (std::size_t idx = 1U; idx < star; ++idx) {
        if ((!is_body(frame[idx])) || (frame[idx] == kStar)) {
            return reject(Reason::kBody, idx, frame[idx]);
        }
    }

    const uint8_t checksum_calc = xor8(frame.subspan(1U, star - 1U));
    // 两位十六进制拼成一个字节：各 ≤ 0x0F，高位左移 4 位后 ≤ 0xF0，结果 ≤ 0xFF，窄化安全。
    const uint8_t checksum_rx =
        static_cast<uint8_t>((static_cast<uint32_t>(hex_value(frame[star + 1U])) << kNibbleBits) |
                             static_cast<uint32_t>(hex_value(frame[star + 2U])));

    if (checksum_calc != checksum_rx) {
        Verdict verdict = reject(Reason::kChecksum, star + 1U, checksum_rx);
        verdict.computed = checksum_calc;
        return verdict;
    }
    return Verdict{}; // step = kNeedMore 占位，由调用方改写
}

/** 判定：step 与 explain 的唯一事实来源。 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    const std::size_t last = frame.size() - 1U;
    const uint8_t new_byte = frame[last];

    if (frame[0] != kSyncByte) {
        return reject(Reason::kSync, 0U, frame[0]);
    }
    if (frame.size() < 2U) {
        verdict.need = 2U - frame.size();
        return verdict;
    }
    if (!is_upper_alnum(frame[1])) {
        // 第 2 字节必须是发送方标识（大写字母或数字）：与同样以 '$' 开头的其他语法早分开。
        return reject(Reason::kSecond, 1U, frame[1]);
    }
    if (new_byte == kLf) {
        Verdict finished = finish(frame);
        if (finished.reason == Reason::kNone) {
            finished.step = Step::kFrame;
        }
        return finished;
    }

    std::size_t star_ofs = 0U;
    if (find_star_reverse(frame, last, kStarWindow, star_ofs)) {
        if (star_ofs == 0U) {
            // 当前字节就是 '*'：校验字段从这里开始。
        } else if (star_ofs < kHexHiOffset) {
            // 偏移 1、2：两位十六进制校验位。
            if (!is_hex_digit(new_byte)) {
                return reject(Reason::kHexByte, last, new_byte);
            }
        } else if (star_ofs == kHexHiOffset) {
            // 偏移 3：只能是 '\r'。
            if (new_byte != kCr) {
                return reject(Reason::kNoCrByte, last, new_byte);
            }
        } else {
            // 偏移 4 只可能以 '\n' 结束（上面已处理），其余情况非法。
            return reject(Reason::kUnexpected, last, new_byte);
        }
    } else if (!is_body(new_byte)) {
        return reject(Reason::kBody, last, new_byte);
    } else {
        // 正文字节合法，继续累积。
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
        text.literal("sync byte 1 is ").hex(static_cast<uint32_t>(verdict.received), 2U).literal(", expected '$'");
        break;
    case Reason::kSecond:
        text.literal("byte 1 is ").byte_value(verdict.received).literal(", not an upper-case sender id");
        break;
    case Reason::kShort:
        text.literal("shorter than ").decimal(kMinLen).literal(" bytes");
        break;
    case Reason::kMissingStar:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byte_value(verdict.received)
            .literal(", expected '*'");
        break;
    case Reason::kBody:
        text.literal("illegal byte ").byte_value(verdict.received).literal(" at offset ").decimal(verdict.offset);
        break;
    case Reason::kNoCrByte:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byte_value(verdict.received)
            .literal(", expected CR");
        break;
    case Reason::kHexByte:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byte_value(verdict.received)
            .literal(", not a hex digit");
        break;
    case Reason::kChecksum:
        text.literal("bad checksum, received ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(", computed ")
            .hex(static_cast<uint32_t>(verdict.computed), 2U);
        break;
    case Reason::kUnexpected:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byte_value(verdict.received)
            .literal(" after the checksum");
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
    explain_verdict((verdict.step == Step::kReject) ? verdict : reject(Reason::kNone, 0U), text);
    return text.finish();
}

} // namespace demux::nmea
