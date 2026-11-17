// Unicore Hash 语句解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：'#' | 第 2 字节 [A-Z] | 正文可打印
// 且不含 '#' '$' '*' | '*' 后 8 位十六进制 CRC-32（同 unicore_bin 算法）或 2 位十六进制 XOR | CR LF。
// 纯函数、无静态可变状态、无堆、不阻塞：ISR 安全；帧长未知，step 每字节 O(1)，'\n' 到达时才整段复核。

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

/** judge() 的结论：判定、原因、待补字节数与诊断数值。 */
struct Verdict {
    Step step = Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;
    std::size_t offset = 0U; // 出错字节下标
    uint32_t received = 0U;  // 校验/字节类原因：收到的值
    uint32_t computed = 0U;  // 校验类原因：算出的值
};

/**
 * @brief  构造一个 kReject 结论
 * @param  reason: 拒绝原因
 * @param  offset: 出错字节在候选前缀中的下标
 * @param  byte: 该位置的原始字节，作为“收到的值”
 * @retval 结论（step=kReject，need 保持默认 1）
 */
Verdict reject(Reason reason, std::size_t offset, uint8_t byte = 0U) noexcept
{
    Verdict verdict{};
    verdict.step = Step::kReject;
    verdict.reason = reason;
    verdict.offset = offset;
    verdict.received = static_cast<uint32_t>(byte);
    return verdict;
}

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
 * @brief  正文合法性（比 NMEA 更严）：可打印且不含 '#' '$' '*'
 * @param  byte: 待判定字节
 * @retval true 合法正文字节；false 非法
 */
bool isBody(uint8_t byte) noexcept
{
    return isPrintable(byte) && (byte != kSyncByte) && (byte != kDollar) && (byte != kStar);
}

/**
 * @brief  逐字节异或（XOR 形式校验和）
 * @param  data: 参与异或的字节（'#' 与 '*' 之间）；为空时结果为 0
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
 * @param  frame: 候选语句，非空
 * @param  last: 窗口末端下标（此处为 '\n' 的下标）
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
 * @brief  读 count 位大端十六进制并拼成整数
 * @param  frame: 候选语句
 * @param  from: 首个十六进制字符下标
 * @param  count: 位数，≤ 8；调用方保证范围合法且都是十六进制字符
 * @retval 拼出的 32 位值
 */
uint32_t readHexBe(std::span<const uint8_t> frame, std::size_t from, std::size_t count) noexcept
{
    uint32_t value = 0U;

    for (std::size_t idx = 0U; idx < count; ++idx) {
        value = (value << kNibbleBits) | static_cast<uint32_t>(hexValue(frame[from + idx]));
    }
    return value;
}

/**
 * @brief  检查一段字节是否全是十六进制字符
 * @param  frame: 候选语句
 * @param  from: 起始下标；调用方保证 from + count ≤ frame.size()
 * @param  count: 检查的字节数
 * @retval true 全是十六进制字符（含 count == 0 的空段）；false 至少一个不是
 */
bool allHex(std::span<const uint8_t> frame, std::size_t from, std::size_t count) noexcept
{
    for (std::size_t idx = 0U; idx < count; ++idx) {
        if (!isHexDigit(frame[from + idx])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief  语句结构已确定后的整段复核：正文合法性 + 按 '*'\''偏移二选一校验
 * @param  frame: 从同步字节起到 '\n' 的候选语句
 * @param  star: '*' 的下标（两种校验形式的尾部偏移都已由调用方确认）
 * @retval step=kFrame 校验通过
 * @retval step=kReject 正文非法（kBody）、XOR 不符（kXorChecksum）或 CRC 不符（kCrcChecksum）
 * @note   整段 O(len)；纯函数，只依赖入参。
 */
Verdict finish(std::span<const uint8_t> frame, std::size_t star) noexcept
{
    for (std::size_t idx = 1U; idx < star; ++idx) {
        if (!isBody(frame[idx])) {
            return reject(Reason::kBody, idx, frame[idx]);
        }
    }

    const std::size_t len = frame.size();
    if ((len - 1U - star) == kStarOfsXor) {
        // 形式一：2 位十六进制 XOR，覆盖 '#' 与 '*' 之间。
        // 各十六进制位 ≤ 0x0F，左移 4 位后 ≤ 0xF0，结果 ≤ 0xFF，窄化安全。
        const uint8_t checksumRx = static_cast<uint8_t>(
            (static_cast<uint32_t>(hexValue(frame[star + 1U])) << kNibbleBits) |
            static_cast<uint32_t>(hexValue(frame[star + 2U])));
        const uint8_t checksumCalc = xor8(frame.subspan(1U, star - 1U));
        if (checksumCalc != checksumRx) {
            Verdict bad = reject(Reason::kXorChecksum, star + 1U, checksumRx);
            bad.computed = static_cast<uint32_t>(checksumCalc);
            return bad;
        }
    } else {
        // 形式二：8 位十六进制 CRC-32，与 Unicore 二进制相同的算法。
        const uint32_t checksumRx = readHexBe(frame, star + 1U, kCrcDigits);
        const uint32_t checksumCalc = crc::crc32Novatel(frame.subspan(1U, star - 1U));
        if (checksumCalc != checksumRx) {
            Verdict bad = reject(Reason::kCrcChecksum, star + 1U, 0U);
            bad.received = checksumRx;
            bad.computed = checksumCalc;
            return bad;
        }
    }
    Verdict good{};
    good.step = Step::kFrame;
    return good;
}

/**
 * @brief  未到结束符时，按新到字节相对 '*' 的偏移做 O(1) 预检
 * @param  frame: 累积中的候选语句
 * @param  last: 新到字节下标
 * @param  star: 最近的 '*' 下标（调用方已确认 star < last）
 * @param  newByte: 新到字节（frame[last]）
 * @retval true 该偏移上允许出现这个字节；false 应立刻按 kChecksumByte 拒绝
 * @note   只看 1..9 的定长偏移与 frame[star+3] 一个标志位，O(1)。
 */
bool midByteOk(std::span<const uint8_t> frame, std::size_t last, std::size_t star,
               uint8_t newByte) noexcept
{
    const std::size_t afterStar = last - star;

    if (afterStar <= kXorDigits) {
        // 两种形式的前两位都是十六进制。
        return isHexDigit(newByte);
    }
    if (afterStar == kOfsBranch) {
        // XOR 形式到此结束应为 '\r'；CRC 形式继续是十六进制。
        return isHexDigit(newByte) || (newByte == kCr);
    }
    if (afterStar <= kCrcDigits) {
        // 4..8：只有 CRC 形式能走到这里；第 3 字节已是 '\r' 说明 XOR 形式早该结束。
        if (frame[star + kOfsBranch] == kCr) {
            return false;
        }
        return isHexDigit(newByte);
    }
    if (afterStar == kOfsCr) {
        // CRC 形式 8 位十六进制之后的 '\r'。
        if (frame[star + kOfsBranch] == kCr) {
            return false;
        }
        return newByte == kCr;
    }
    // 更远的偏移只可能是 '\n'（结束符由调用方单独处理）。
    return false;
}

/**
 * @brief  语句已结束（末字节为 '\n'）：定位 '*'、校验其偏移与 CR，再按 XOR/CRC 形式整段复核
 * @param  frame: 从同步字节起到 '\n' 的候选帧
 * @param  last: frame 末字节下标（'\n' 的位置）
 * @retval step=kFrame 校验通过
 * @retval step=kReject 缺 '*'（kMissingStar）、'*' 偏移不是 4/10（kStarOffset）、'\n' 前不是 CR（kNoCr）、
 *         校验字段含非十六进制字符（kBadHex），或 finish() 报出的校验/正文错误
 * @note   只读入参，无副作用；由 judge() 在帧尾到达时调用。
 */
Verdict judgeFinished(std::span<const uint8_t> frame, std::size_t last) noexcept
{
    std::size_t starOfs = 0U;
    // 最近的 '*' 就是校验字段起点；两种形式的 '*' 偏移都是定值。
    if (!findStarReverse(frame, last, kStarWindow, starOfs)) {
        return reject(Reason::kMissingStar, last, frame[last]);
    }
    if ((starOfs != kStarOfsXor) && (starOfs != kStarOfsCrc)) {
        return reject(Reason::kStarOffset, last - starOfs, frame[last - starOfs]);
    }
    if (frame[last - 1U] != kCr) {
        return reject(Reason::kNoCr, last - 1U, frame[last - 1U]);
    }
    const std::size_t star = last - starOfs; // starOfs ≤ last，不越界
    if (starOfs == kStarOfsXor) {
        if (!isHexDigit(frame[star + 1U])) {
            return reject(Reason::kBadHex, star + 1U, frame[star + 1U]);
        }
        if (!isHexDigit(frame[star + 2U])) {
            return reject(Reason::kBadHex, star + 2U, frame[star + 2U]);
        }
    } else if (!allHex(frame, star + 1U, kCrcDigits)) {
        for (std::size_t idx = 0U; idx < kCrcDigits; ++idx) {
            if (!isHexDigit(frame[star + 1U + idx])) {
                return reject(Reason::kBadHex, star + 1U + idx, frame[star + 1U + idx]);
            }
        }
    } else {
        // allHex 已通过，校验字段都是十六进制。
    }
    return finish(frame, star);
}

/**
 * @brief  判定一帧：同步字节、第 2 字节、长度上限、未结束时逐字节预检、结束时整段复核
 * @param  frame: 候选前缀，非空且 frame[0] 已通过 firstByte
 * @retval step=kNeedMore 还不能下结论，need 为至少还要的字节数（帧长未知时通常为 1）
 * @retval step=kFrame 语句完整且 XOR 或 CRC-32 校验通过
 * @retval step=kReject 具体原因见 Verdict::reason
 * @note   纯函数；未到 '\n' 的路径只查新到字节与定长尾部窗口（O(1)），'\n' 分支整段复核。
 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    if (frame[0] != kSyncByte) {
        return reject(Reason::kSync, 0U, frame[0]);
    }
    const std::size_t last = frame.size() - 1U;
    const uint8_t newByte = frame[last];

    if (frame.size() < 2U) {
        verdict.need = 2U - frame.size();
        return verdict;
    }
    if (!isUpperAlpha(frame[1])) {
        // 第 2 字节必须是语句名首字母（大写字母）：与同样以 '#' 开头的其他语法早分开。
        return reject(Reason::kSecond, 1U, frame[1]);
    }
    if (frame.size() < kMinLen) {
        verdict.need = kMinLen - frame.size();
        return verdict;
    }

    if (newByte == kLf) {
        return judgeFinished(frame, last);
    }

    // 未到结束符：只查新到字节与其前面定长窗口内的 '*' 位置（O(1)）。
    std::size_t starOfs = 0U;
    if (!findStarReverse(frame, last, kStarWindow, starOfs)) {
        if (!isBody(newByte)) {
            return reject(Reason::kBody, last, newByte);
        }
    } else if ((starOfs != 0U) && (!midByteOk(frame, last, last - starOfs, newByte))) {
        // 偏移 0 是 '*' 本身（校验字段开始），由下一个字节起再预检。
        return reject(Reason::kChecksumByte, last, newByte);
    } else {
        // 正文字节或 '*' 合法，继续累积。
    }

    if ((frame.size() + 1U) > kMaxFrame) {
        // 超过 maxFrame 仍未结束：need 契约禁止要求超限字节，只能拒绝。
        return reject(Reason::kTooLong, last);
    }
    verdict.need = 1U;
    return verdict;
}

/**
 * @brief  把判定理由写成一句人读文本
 * @param  verdict: 判定结果
 * @param[out] text: 文本输出器（空间不足时截断）
 * @note   不拆（ADR-0013 §2.4）：拒绝原因到一句话的纯文本映射表，每个 case 独立、彼此无状态。
 */
void explainVerdict(const Verdict &verdict, // NOLINT(readability-function-*)
                    demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case Reason::kSync:
        text.literal("byte 0 is ")
            .byteValue(static_cast<uint8_t>(verdict.received))
            .literal(", expected '#'");
        break;
    case Reason::kSecond:
        text.literal("byte 1 is ")
            .byteValue(static_cast<uint8_t>(verdict.received))
            .literal(", not 'A'-'Z'");
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
            .byteValue(static_cast<uint8_t>(verdict.received))
            .literal(" at offset ")
            .decimal(verdict.offset);
        break;
    case Reason::kChecksumByte:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byteValue(static_cast<uint8_t>(verdict.received))
            .literal(", not a checksum digit or CR");
        break;
    case Reason::kNoCr:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byteValue(static_cast<uint8_t>(verdict.received))
            .literal(", expected CR");
        break;
    case Reason::kBadHex:
        text.literal("byte ")
            .decimal(verdict.offset)
            .literal(" is ")
            .byteValue(static_cast<uint8_t>(verdict.received))
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
    if (verdict.step == Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explainVerdict(verdict, text);
    return text.finish();
}

} // namespace demux::unicore_hash
