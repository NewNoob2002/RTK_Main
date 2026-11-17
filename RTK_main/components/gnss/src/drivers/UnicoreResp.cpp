// Unicore $command 应答解码：成帧判定（judge 家族）与应答文本解析（ADR-0010 §2.4）。
// 线程/调用约束：全部为纯函数（无全局可变状态）；responseStep 由 demux 引擎每收到一字节调用，须 ISR 安全。
// 堆：不使用。浮点：不使用。
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "gnss/drivers/UnicoreReceiver.hpp"

#include "demux/TextWriter.hpp"
#include "../AsciiCmd.hpp"
#include "UnicoreKeys.hpp"

namespace gnss {
namespace {

using unicore::kCommandPrefix;
using unicore::kConfigPrefix;
using unicore::kCr;
using unicore::kDollar;
using unicore::kLf;
using unicore::kMinReplyBytes;
using unicore::kResponseTag;
using unicore::kStar;
using unicore::kStarFromEnd;

/** 应答帧的判定原因，explain 与 step 共用同一份判定逻辑。 */
enum class ResponseReason : uint8_t {
    kNone = 0,
    kEmpty,
    kPrefix,
    kTail,
    kBody,
    kChecksum,
    kStarField,
    kStarByte,
    kNoRoom,
};

struct ResponseVerdict {
    demux::Step step = demux::Step::kReject;
    ResponseReason reason = ResponseReason::kNone;
    std::size_t need = 0;
    std::size_t offset = 0;
    uint8_t received = 0;
    uint8_t computed = 0;
    uint8_t alt = 0; // 第二个期望字节（两种前缀都还成立时才有，0 表示没有）
};

/**
 * @brief 构造“拒绝”结论。
 * @param reason: 拒绝原因（explain 用）。
 * @param offset: 出问题的字节位置。
 * @param received: 实际读到的字节/校验值。
 * @param computed: 期望的字节/校验值。
 * @retval verdict: step = kReject。
 */
ResponseVerdict reject(ResponseReason reason, std::size_t offset = 0, uint8_t received = 0,
                       uint8_t computed = 0) noexcept
{
    ResponseVerdict verdict{};
    verdict.reason = reason;
    verdict.offset = offset;
    verdict.received = received;
    verdict.computed = computed;
    return verdict;
}

/**
 * @brief 构造“前缀字节不符”的拒绝结论（两种前缀都可能时给出两个期望字节）。
 * @param offset: 出问题的字节位置。
 * @param received: 实际读到的字节。
 * @param wantFirst: 第一个仍可能的期望字节（0 表示该候选已失效）。
 * @param wantSecond: 第二个仍可能的期望字节（0 表示没有）。
 * @retval verdict: step = kReject，reason = kPrefix。
 */
ResponseVerdict rejectPrefix(std::size_t offset, uint8_t received, uint8_t wantFirst,
                             uint8_t wantSecond) noexcept
{
    ResponseVerdict verdict = reject(ResponseReason::kPrefix, offset, received, wantFirst);
    verdict.alt = wantSecond;
    return verdict;
}

/**
 * @brief 构造“还需要更多字节”结论。
 * @param need: 至少还差多少字节。
 * @retval verdict: step = kNeedMore。
 */
ResponseVerdict needMore(std::size_t need) noexcept
{
    ResponseVerdict verdict{};
    verdict.step = demux::Step::kNeedMore;
    verdict.need = need;
    return verdict;
}

/**
 * @brief 构造“完整帧”结论。
 * @retval verdict: step = kFrame。
 */
ResponseVerdict complete() noexcept
{
    ResponseVerdict verdict{};
    verdict.step = demux::Step::kFrame;
    return verdict;
}

/**
 * @brief 检查两种可接受前缀（"$command," / "$CONFIG,"）与最小长度。
 *
 * 逐字节判定：只要仍有前缀成立就继续收取；两个候选同时失配才拒绝。两者从第 2 字节起分叉（'c' / 'C'），
 * 因此绝大多数拒收只报一个期望字节，第 2 字节失配时报两个。
 * @param frame: 已收到的字节。
 * @retval std::nullopt: 前缀完整且长度够，可继续看正文。
 * @retval verdict: 需要继续收取或直接拒绝。
 */
std::optional<ResponseVerdict> checkPrefix(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (size == 0U) {
        return reject(ResponseReason::kEmpty);
    }
    const size_t cmdLen = kCommandPrefix.size();
    const size_t cfgLen = kConfigPrefix.size();
    const size_t longest = cmdLen > cfgLen ? cmdLen : cfgLen;
    const size_t compared = size < longest ? size : longest;
    bool cmdAlive = true;
    bool cfgAlive = true;
    for (size_t i = 0; i < compared; ++i) {
        const uint8_t byte = frame[i];
        // 前缀已走完的候选不再受本字节约束（视为仍然成立）。
        const bool cmdNext =
            cmdAlive && ((i >= cmdLen) || (byte == static_cast<uint8_t>(kCommandPrefix[i])));
        const bool cfgNext =
            cfgAlive && ((i >= cfgLen) || (byte == static_cast<uint8_t>(kConfigPrefix[i])));
        if (!cmdNext && !cfgNext) {
            // 只有失配前仍成立的候选才参与“expected”文本，避免报出已排除的字节。
            const uint8_t wantCmd =
                (cmdAlive && (i < cmdLen)) ? static_cast<uint8_t>(kCommandPrefix[i]) : 0U;
            const uint8_t wantCfg =
                (cfgAlive && (i < cfgLen)) ? static_cast<uint8_t>(kConfigPrefix[i]) : 0U;
            return rejectPrefix(i, byte, wantCmd, wantCfg);
        }
        cmdAlive = cmdNext;
        cfgAlive = cfgNext;
    }
    // 最小帧长按较长前缀（$command）计算：对 $CONFIG 只多要 1 字节，仍远小于真实读回行。
    if (size < cmdLen) {
        return needMore(cmdLen - size);
    }
    if (size < (cmdLen + kMinReplyBytes)) {
        return needMore((cmdLen + kMinReplyBytes) - size);
    }
    return std::nullopt;
}

/**
 * @brief 校验和是否匹配（标准 XOR，或 UM980 实测把 '$' 一并计入的变体）。
 * @param frame: 整帧字节。
 * @param starPos: '*' 的下标。
 * @param received: 输出，帧内写的校验值。
 * @param computed: 输出，按报文体算出的校验值。
 * @retval true: 二者任一一致。
 * @retval false: 都不一致。
 */
bool checksumMatches(std::span<const uint8_t> frame, std::size_t starPos, uint8_t &received,
                     uint8_t &computed) noexcept
{
    computed = ascii::xorChecksum(frame, 1U, starPos);
    // hexValue 返回 uint8_t，整型提升为 int 后再移位属有符号位运算：先转 uint32_t（值域 0..15，安全）。
    const uint32_t high = static_cast<uint32_t>(ascii::hexValue(frame[starPos + 1U]))
                          << ascii::kHexShift;
    const uint32_t low = static_cast<uint32_t>(ascii::hexValue(frame[starPos + 2U]));
    received = static_cast<uint8_t>(high | low);
    if (computed == received) {
        return true;
    }
    // UM980 R4.10Build11833 实测把 '$' 也算进 $command 校验（"$command,VERSIONA,response: OK*45"，
    // 标准 XOR 为 0x61）；SparkFun Unicore 库对旧固件有同样的兼容（Unicore/src 内 "improperly adding the '$'"）。
    return static_cast<uint8_t>(computed ^ kDollar) == received;
}

/**
 * @brief 收到 '\n' 之后的完整帧检查：尾结构、报文体字节、校验和。
 * @param frame: 整帧字节。
 * @retval std::nullopt: 全部通过。
 * @retval verdict: 拒绝原因。
 */
std::optional<ResponseVerdict> checkFullFrame(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (!ascii::hasHexTail(frame)) {
        return reject(ResponseReason::kTail, size - 1U, frame[size - 1U]);
    }
    const size_t starPos = size - kStarFromEnd;
    for (size_t i = 1U; i < starPos; ++i) {
        if (!ascii::isBodyByte(frame[i])) {
            return reject(ResponseReason::kBody, i, frame[i]);
        }
    }
    uint8_t received = 0;
    uint8_t computed = 0;
    if (!checksumMatches(frame, starPos, received, computed)) {
        return reject(ResponseReason::kChecksum, starPos, received, computed);
    }
    return std::nullopt;
}

/**
 * @brief 尚未收到 '\n'：在末尾 5 字节窗口里找 '*'，据此判断补几个字节。
 * @param frame: 已收到的字节（非空）。
 * @retval std::nullopt: 不会出现（仅为与另外两个检查函数同签名）。
 * @retval verdict: 需要继续收取或直接拒绝。
 */
std::optional<ResponseVerdict> checkPartialFrame(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    size_t starPos = size;
    const size_t window = size < kStarFromEnd ? size : kStarFromEnd;
    for (size_t k = 0; k < window; ++k) {
        if (frame[size - 1U - k] == kStar) {
            starPos = size - 1U - k;
            break;
        }
    }
    if (starPos == size) {
        // 没有 '*' 时只校验最后一字节是否为合法报文体，避免把半个多字节字符判成帧。
        if (!ascii::isBodyByte(frame[size - 1U])) {
            return reject(ResponseReason::kBody, size - 1U, frame[size - 1U]);
        }
        return needMore(1U);
    }
    if ((size - starPos) > kStarFromEnd) {
        return reject(ResponseReason::kStarField, starPos);
    }
    for (size_t i = starPos + 1U; i < size; ++i) {
        const uint8_t c = frame[i];
        if (!ascii::isHex(c) && c != kCr && c != kLf) {
            return reject(ResponseReason::kStarByte, i, c);
        }
    }
    if ((size - starPos) >= kStarFromEnd) {
        return reject(ResponseReason::kNoRoom, starPos);
    }
    return needMore(kStarFromEnd - (size - starPos));
}

/**
 * @brief 统一的帧判定入口：explain 与 step 共用，保证诊断结论与实际成帧一致。
 * @param frame: 已收到的字节。
 * @retval verdict: kFrame / kNeedMore / kReject 之一。
 */
ResponseVerdict judge(std::span<const uint8_t> frame) noexcept
{
    if (const std::optional<ResponseVerdict> head = checkPrefix(frame)) {
        return *head;
    }
    if (frame[frame.size() - 1U] == kLf) {
        if (const std::optional<ResponseVerdict> tail = checkFullFrame(frame)) {
            return *tail;
        }
        return complete();
    }
    if (const std::optional<ResponseVerdict> partial = checkPartialFrame(frame)) {
        return *partial;
    }
    return complete();
}

/**
 * @brief 把判定原因写成一句英文（explain 契约）。
 * @param verdict: judge 的结论。
 * @param text: 输出文本写入器。
 */
void explainVerdict(const ResponseVerdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case ResponseReason::kEmpty:
        text.literal("empty prefix");
        break;
    case ResponseReason::kPrefix:
        text.literal("byte ").decimal(verdict.offset).literal(" is ");
        text.byteValue(verdict.received).literal(", expected ");
        text.byteValue(verdict.computed);
        if (verdict.alt != 0U) {
            text.literal(" or ");
            text.byteValue(verdict.alt);
        }
        break;
    case ResponseReason::kTail:
        text.literal("missing \"*hh\\r\\n\" tail before byte ")
            .decimal(verdict.offset)
            .literal(" (");
        text.byteValue(verdict.received).literal(")");
        break;
    case ResponseReason::kBody:
        text.literal("illegal byte ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(" at offset ")
            .decimal(verdict.offset);
        break;
    case ResponseReason::kChecksum:
        text.literal("bad checksum, received ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(", computed ")
            .hex(static_cast<uint32_t>(verdict.computed), 2U);
        break;
    case ResponseReason::kStarField:
        text.literal("'*' at offset ")
            .decimal(verdict.offset)
            .literal(" but fewer than two checksum digits");
        break;
    case ResponseReason::kStarByte:
        text.literal("illegal byte ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(" in checksum field at offset ")
            .decimal(verdict.offset);
        break;
    case ResponseReason::kNoRoom:
        text.literal("no room for CR LF after checksum at offset ").decimal(verdict.offset);
        break;
    case ResponseReason::kNone:
    default:
        text.literal("rejected");
        break;
    }
}
} // namespace

namespace unicore {

/**
 * @brief $command 应答帧的起始字节判定。
 * @param byte: 待判字节。
 * @retval true: 是 '$'。
 * @retval false: 其他。
 */
bool responseFirstByte(uint8_t byte) noexcept
{
    return byte == kDollar;
}

/**
 * @brief $command 应答帧的成帧判定（demux 引擎每收到一字节调用，须 ISR 安全）。
 * @param frame: 已收到的字节。
 * @param need: 输出，至少还差多少字节（kNeedMore 时有效）。
 * @retval step: kFrame / kNeedMore / kReject。
 */
demux::Step responseStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    const ResponseVerdict verdict = judge(frame);
    need = verdict.need;
    return verdict.step;
}

/**
 * @brief 把拒绝原因写成一句英文（诊断为“not rejected”表示帧本身没问题）。
 * @param frame: 已收到的字节。
 * @param out: 输出缓冲。
 * @retval len: 写入的字符数。
 */
std::size_t explainResponse(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    demux::text_writer::TextWriter text(out);
    if (frame.empty()) {
        return text.literal("empty prefix").finish();
    }
    const ResponseVerdict verdict = judge(frame);
    if (verdict.step == demux::Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explainVerdict(verdict, text);
    return text.finish();
}
} // namespace unicore

/**
 * @brief 帧尾是否为合法的 "*hh\r\n"，且长度足够容纳最短应答。
 * @param frame: 整帧字节。
 * @retval true: 尾部结构与最小长度都满足。
 * @retval false: 结构不符或过短。
 */
bool UnicoreReceiver::respHasValidTail(std::span<const uint8_t> frame) noexcept
{
    return (frame.size() >= (kCommandPrefix.size() + kMinReplyBytes)) && ascii::hasHexTail(frame);
}

/**
 * @brief 找 ",response: " 的位置（命令回显与应答文本的分界）。
 * @param frame: 整帧字节。
 * @retval 位置: 标签起始下标。
 * @retval std::nullopt: 没有该标签（不是 $command 应答）。
 */
std::optional<size_t> UnicoreReceiver::respFindTag(std::span<const uint8_t> frame) noexcept
{
    for (size_t i = kCommandPrefix.size(); (i + kResponseTag.size()) <= frame.size(); ++i) {
        if (ascii::startsWith(frame.subspan(i), kResponseTag)) {
            return i;
        }
    }
    return std::nullopt;
}

/**
 * @brief 帧里的回显是否就是本步发送的命令（忽略发送时的 CR/LF）。
 * @param frame: 整帧字节。
 * @param tagPos: ",response: " 的起始下标。
 * @param sent: 本步发出的命令字节。
 * @retval true: 回显与 sent 逐字节相同。
 * @retval false: 长度或内容不同（属于别的命令的应答）。
 */
bool UnicoreReceiver::respEchoMatches(std::span<const uint8_t> frame, size_t tagPos,
                                      std::span<const uint8_t> sent) noexcept
{
    if (tagPos < kCommandPrefix.size()) {
        return false;
    }
    const size_t sentLen = ascii::stripLineEnd(sent);
    if ((tagPos - kCommandPrefix.size()) != sentLen) {
        return false;
    }
    return ascii::bytesStartsWith(frame.subspan(kCommandPrefix.size()), sent.first(sentLen));
}

/**
 * @brief 取应答文本（标签之后到 '*' 之前，去掉尾部空格）。
 * @param frame: 整帧字节。
 * @param tagPos: ",response: " 的起始下标。
 * @retval span: 应答文本（可能为空）。
 */
std::span<const uint8_t> UnicoreReceiver::respText(std::span<const uint8_t> frame,
                                                   size_t tagPos) noexcept
{
    if (frame.size() < kStarFromEnd) {
        return {};
    }
    const size_t begin = tagPos + kResponseTag.size();
    size_t end = frame.size() - kStarFromEnd;
    while ((end > begin) && (frame[end - 1U] == static_cast<uint8_t>(' '))) {
        --end;
    }
    if ((end <= begin) || (begin >= frame.size())) {
        return {};
    }
    return frame.subspan(begin, end - begin);
}

} // namespace gnss
