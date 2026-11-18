// NMEA 0183 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：'$' | 第 2 字节 [A-Z0-9] | 正文可打印
// ASCII 0x20–0x7E 且不含 '$' | '*' + 2 位十六进制 XOR | CR LF；同步/标识/正文/校验任一不符即早拒绝。
// 纯函数、无堆、不阻塞：ISR 安全；帧长未知，step 每字节 O(1)，'\n' 到达时才整段复核（结论与调用历史无关）。

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

/**
 * @brief  是否可打印 ASCII
 * @param  byte: 待判定字节
 * @retval true 在 0x20–0x7E 内；false 其余
 */
bool isPrintable(uint8_t byte) noexcept
{
    return (byte >= kPrintableMin) && (byte <= kPrintableMax);
}

/**
 * @brief  是否十六进制数字（大小写均可）
 * @param  byte: 待判定字节
 * @retval true 是 '0'–'9'、'A'–'F' 或 'a'–'f'；false 其余
 */
bool isHexDigit(uint8_t byte) noexcept
{
    return ((byte >= kDigit0) && (byte <= kDigit9)) || ((byte >= kUpperA) && (byte <= kUpperF)) ||
           ((byte >= kLowerA) && (byte <= kLowerF));
}

/**
 * @brief  是否大写字母
 * @param  byte: 待判定字节
 * @retval true 是 'A'–'Z'；false 其余
 */
bool isUpperAlpha(uint8_t byte) noexcept
{
    return (byte >= kUpperA) && (byte <= kUpperZ);
}

/**
 * @brief  是否大写字母或数字（发送方标识允许的字符集）
 * @param  byte: 待判定字节
 * @retval true 是 'A'–'Z' 或 '0'–'9'；false 其余
 */
bool isUpperAlnum(uint8_t byte) noexcept
{
    return isUpperAlpha(byte) || ((byte >= kDigit0) && (byte <= kDigit9));
}

/**
 * @brief  十六进制数字转数值
 * @param  byte: 必须已经 isHexDigit() 判定通过（大小写均可）
 * @retval 0..15；未判定通过的输入返回值无意义
 */
uint8_t hexValue(uint8_t byte) noexcept
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

/**
 * @brief  正文合法性：可打印且不是 '$'
 * @param  byte: 待判定字节
 * @retval true 合法正文字节；false 非法
 */
bool isBody(uint8_t byte) noexcept
{
    return isPrintable(byte) && (byte != kSyncByte);
}

/**
 * @brief  逐字节异或（NMEA 校验和算法）
 * @param  data: 参与异或的字节（'$' 与 '*' 之间）；为空时结果为 0
 * @retval 8 位异或累积值
 */
uint8_t xor8(std::span<const uint8_t> data) noexcept
{
    uint8_t acc = 0U;

    for (const uint8_t byte : data) {
        // 两个 uint8_t 异或 < 256，窄化回 uint8_t 安全。
        acc = static_cast<uint8_t>(acc ^ byte);
    }
    return acc;
}

/**
 * @brief  在 frame 末尾的定长窗口内反向寻找最近的 '*'
 * @param  frame: 候选前缀，非空
 * @param  last: 窗口末端下标（通常为最后一个字节的下标）
 * @param  window: 最多向前搜索的字节数；窗口定长，因此 O(1)
 * @param[out] ofs: 找到时写入 '*' 距 last 的回退字节数（0 表示 last 本身），未找到时写 0
 * @retval true 窗口内存在 '*'；false 不存在
 */
bool findStarReverse(std::span<const uint8_t> frame, std::size_t last, std::size_t window,
                     std::size_t &ofs) noexcept
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

/**
 * @brief  构造一个 kReject 结论
 * @param  reason: 拒绝原因
 * @param  offset: 出错字节在候选前缀中的下标
 * @param  byte: 该位置的原始字节，校验类原因用它记录“收到的值”
 * @retval 结论（step=kReject，need 保持默认 1）
 */
Verdict reject(Reason reason, std::size_t offset, uint8_t byte = 0U) noexcept
{
    Verdict verdict{};
    verdict.step = Step::kReject;
    verdict.reason = reason;
    verdict.offset = offset;
    verdict.received = byte;
    return verdict;
}

/**
 * @brief  '\n' 到达时的整段复核：尾部结构、正文合法性、XOR 校验
 * @param  frame: 以 '\n' 结尾的候选语句，长度 ≥ 1
 * @retval step=kReject 且 reason 给出具体位置（kShort/kNoCrByte/kHexByte/kMissingStar/kBody/kChecksum）
 * @retval step=kNeedMore 且 reason=kNone 表示校验通过，由调用方改写为 kFrame（占位约定）
 * @note   整段扫描 O(len)；纯函数，只依赖入参。
 */
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
    if (!isHexDigit(frame[last - kHexLoOffset])) {
        return reject(Reason::kHexByte, last - kHexLoOffset, frame[last - kHexLoOffset]);
    }
    if (!isHexDigit(frame[last - kHexHiOffset])) {
        return reject(Reason::kHexByte, last - kHexHiOffset, frame[last - kHexHiOffset]);
    }
    if (frame[star] != kStar) {
        return reject(Reason::kMissingStar, star, frame[star]);
    }
    // 正文（'$' 与 '*' 之间）必须可打印且既不含 '$' 也不含 '*'（'*' 只能出现在校验字段起点）。
    for (std::size_t idx = 1U; idx < star; ++idx) {
        if ((!isBody(frame[idx])) || (frame[idx] == kStar)) {
            return reject(Reason::kBody, idx, frame[idx]);
        }
    }

    const uint8_t checksumCalc = xor8(frame.subspan(1U, star - 1U));
    // 两位十六进制拼成一个字节：各 ≤ 0x0F，高位左移 4 位后 ≤ 0xF0，结果 ≤ 0xFF，窄化安全。
    const uint8_t checksumRx =
        static_cast<uint8_t>((static_cast<uint32_t>(hexValue(frame[star + 1U])) << kNibbleBits) |
                             static_cast<uint32_t>(hexValue(frame[star + 2U])));

    if (checksumCalc != checksumRx) {
        Verdict verdict = reject(Reason::kChecksum, star + 1U, checksumRx);
        verdict.computed = checksumCalc;
        return verdict;
    }
    return Verdict{}; // step = kNeedMore 占位，由调用方改写
}

/**
 * @brief  对候选前缀做完整判定：step 与 explain 的唯一事实来源
 * @param  frame: 候选前缀，非空且 frame[0] 已通过 firstByte
 * @retval step=kNeedMore（need 见 Verdict，帧长未知时恒为 1）
 * @retval step=kFrame 语句完整且 XOR 校验通过
 * @retval step=kReject 具体原因见 Verdict::reason
 * @note   纯函数；通常路径只查新到字节与定长尾部窗口（O(1)），'\n' 分支才整段复核。
 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    const std::size_t last = frame.size() - 1U;
    const uint8_t newByte = frame[last];

    if (frame[0] != kSyncByte) {
        return reject(Reason::kSync, 0U, frame[0]);
    }
    if (frame.size() < 2U) {
        verdict.need = 2U - frame.size();
        return verdict;
    }
    if (!isUpperAlnum(frame[1])) {
        // 第 2 字节必须是发送方标识（大写字母或数字）：与同样以 '$' 开头的其他语法早分开。
        return reject(Reason::kSecond, 1U, frame[1]);
    }
    if (newByte == kLf) {
        Verdict finished = finish(frame);
        if (finished.reason == Reason::kNone) {
            finished.step = Step::kFrame;
        }
        return finished;
    }

    std::size_t starOfs = 0U;
    if (findStarReverse(frame, last, kStarWindow, starOfs)) {
        if (starOfs == 0U) {
            // 当前字节就是 '*'：校验字段从这里开始。
        } else if (starOfs < kHexHiOffset) {
            // 偏移 1、2：两位十六进制校验位。
            if (!isHexDigit(newByte)) {
                return reject(Reason::kHexByte, last, newByte);
            }
        } else if (starOfs == kHexHiOffset) {
            // 偏移 3：只能是 '\r'。
            if (newByte != kCr) {
                return reject(Reason::kNoCrByte, last, newByte);
            }
        } else {
            // 偏移 4 只可能以 '\n' 结束（上面已处理），其余情况非法。
            return reject(Reason::kUnexpected, last, newByte);
        }
    } else if (!isBody(newByte)) {
        return reject(Reason::kBody, last, newByte);
    } else {
        // 正文字节合法，继续累积。
    }

    if ((frame.size() + 1U) > kMaxFrame) {
        // 超过 maxFrame 仍未结束：need 契约禁止要求超限字节，只能拒绝。
        return reject(Reason::kTooLong, last);
    }
    verdict.need = 1U;
    return verdict;
}

// NOLINTBEGIN(readability-function-size): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
/**
 * @brief  把判定理由写成一句人读文本
 * @param  verdict: 判定结果
 * @param[out] text: 文本输出器（空间不足时截断）
 * @note   不拆（ADR-0013 §2.4）：拒绝原因到一句话的纯文本映射表，每个 case 独立、彼此无状态。
 */
void explainVerdict(const Verdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case Reason::kSync:
        text.literal("sync byte 1 is ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(", expected '$'");
        break;
    case Reason::kSecond:
        text.literal("byte 1 is ")
            .byteValue(verdict.received)
            .literal(", not an upper-case sender id");
        break;
    case Reason::kShort:
        text.literal("shorter than ").decimal(kMinLen).literal(" bytes");
        break;
    case Reason::kMissingStar:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byteValue(verdict.received)
            .literal(", expected '*'");
        break;
    case Reason::kBody:
        text.literal("illegal byte ")
            .byteValue(verdict.received)
            .literal(" at offset ")
            .decimal(verdict.offset);
        break;
    case Reason::kNoCrByte:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byteValue(verdict.received)
            .literal(", expected CR");
        break;
    case Reason::kHexByte:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byteValue(verdict.received)
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
            .byteValue(verdict.received)
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
// NOLINTEND(readability-function-size)

} // namespace

bool firstByte(uint8_t byte) noexcept
{
    return byte == kSyncByte;
}

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 firstByte。
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
    explainVerdict((verdict.step == Step::kReject) ? verdict : reject(Reason::kNone, 0U), text);
    return text.finish();
}

} // namespace demux::nmea
