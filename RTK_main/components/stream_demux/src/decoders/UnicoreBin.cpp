// Unicore 二进制解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：AA 44 B5 | 24 B 固定头（载荷长 u16 LE
// 在偏移 6）| 载荷 | CRC-32（0xEDB88320 反射、初值 0、不取反）4 B LE；同步字节 2/3、长度超限、CRC 错均早拒绝。
// 纯函数、无静态可变状态、无堆、不阻塞：可重入、ISR 安全；长度确定后一次要求整段剩余字节，CRC 整段算。

#include "demux/decoders/UnicoreBin.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/TextWriter.hpp"
#include "demux/Crc.hpp"
#include "demux/Decoder.hpp"

namespace demux::unicore_bin {

namespace {

constexpr uint8_t kSyncA = 0xAAU;
constexpr uint8_t kSyncB = 0x44U;
constexpr uint8_t kSyncC = 0xB5U;
constexpr std::size_t kSyncB1Len = 2U; // 同步到第 2 字节（含）
constexpr std::size_t kSyncLen = 3U;
constexpr std::size_t kHdrLen = 24U;
constexpr std::size_t kCrcLen = 4U;
constexpr std::size_t kOfsLen = 6U;
constexpr std::size_t kU16Bytes = 2U;
constexpr std::size_t kU32Bytes = 4U;
// explain 的同步字节诊断：received 低 8 位 = 实收字节，高 8 位 = 期望字节。
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kByteShift = 8U;
constexpr uint32_t kBitsPerByte = 8U;
constexpr std::size_t kBytesToLenField = kOfsLen + kU16Bytes;
constexpr uint8_t kCrcDigits = 8U;

/** 拒绝原因；explain 据此生成一句话。 */
enum class Reason : uint8_t {
    kNone,
    kSync,
    kTooLong,
    kExtra,
    kCrc,
};

/** judge() 的结论：判定、原因、待补字节数与诊断数值。 */
struct Verdict {
    Step step = Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;
    std::size_t offset = 0U; // 同步字节出错的位置
    uint32_t received = 0U;  // 同步/校验类原因：收到的值
    uint32_t computed = 0U;  // 校验类原因：算出的值
    std::size_t total = 0U;  // 长度类原因：声称的总长
};

/**
 * @brief  构造一个 kReject 结论
 * @param  reason: 拒绝原因
 * @retval 结论（step=kReject，need 保持默认 1，诊断数值由调用方补写）
 */
Verdict reject(Reason reason) noexcept
{
    Verdict verdict{};
    verdict.step = Step::kReject;
    verdict.reason = reason;
    return verdict;
}

/**
 * @brief  构造“同步字节不符”的 kReject 结论
 * @param  offset: 出错的同步字节下标（0/1/2）
 * @param  expected: 该位置期望的字节
 * @param  got: 实际收到的字节
 * @retval 结论；received 打包为 got | expected<<8，由 explainVerdict 拆开输出
 */
Verdict rejectSync(uint8_t offset, uint8_t expected, uint8_t got) noexcept
{
    Verdict verdict = reject(Reason::kSync);
    verdict.offset = offset;
    verdict.received = static_cast<uint32_t>(got) | (static_cast<uint32_t>(expected) << kByteShift);
    return verdict;
}

/**
 * @brief  读取小端 u16
 * @param  frame: 候选前缀
 * @param  from: 低位字节下标；调用方保证 from + 2 ≤ frame.size()
 * @retval 组合出的 16 位值
 */
uint32_t readU16Le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    return (static_cast<uint32_t>(frame[from]) | (static_cast<uint32_t>(frame[from + 1U]) << 8U));
}

/**
 * @brief  读取小端 u32
 * @param  frame: 候选前缀
 * @param  from: 最低位字节下标；调用方保证 from + 4 ≤ frame.size()
 * @retval 组合出的 32 位值
 */
uint32_t readU32Le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    uint32_t value = 0U;

    for (std::size_t idx = 0U; idx < kU32Bytes; ++idx) {
        // idx ≤ 3，窄化到 uint32_t 安全；移位量 = idx * 8 ≤ 24，uint32_t 内不溢出。
        value |= static_cast<uint32_t>(frame[from + idx])
                 << (static_cast<uint32_t>(idx) * kBitsPerByte);
    }
    return value;
}

/**
 * @brief  判定一帧：三个同步字节、长度、CRC-32
 * @param  frame: 候选前缀，非空且 frame[0] 已通过 firstByte
 * @retval step=kNeedMore 还不能下结论，need 为至少还要的字节数（逐个同步字节与长度字段补齐）
 * @retval step=kFrame 长度与 CRC-32 都通过
 * @retval step=kReject 同步字节 2/3 不符（kSync）、长度超 kMaxFrame（kTooLong）、比声明长度长（kExtra）
 *         或 CRC 不符（kCrc）
 * @note   纯函数；长度字段一到就要求整段剩余字节（含 4 B CRC），CRC 整段一次算完。
 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    if (frame[0] != kSyncA) {
        return rejectSync(0U, kSyncA, frame[0]);
    }
    if (frame.size() < kSyncB1Len) {
        verdict.need = kSyncB1Len - frame.size();
        return verdict;
    }
    if (frame[1] != kSyncB) {
        return rejectSync(1U, kSyncB, frame[1]);
    }
    if (frame.size() < kSyncLen) {
        verdict.need = kSyncLen - frame.size();
        return verdict;
    }
    if (frame[2] != kSyncC) {
        // 与同以 AA 44 开头的产品帧（AA 44 18）在这里分开。
        return rejectSync(2U, kSyncC, frame[2]);
    }
    if (frame.size() < kBytesToLenField) {
        verdict.need = kBytesToLenField - frame.size();
        return verdict;
    }

    const uint32_t payloadLen = readU16Le(frame, kOfsLen);
    // payloadLen ≤ 0xFFFF，加固定头与 CRC 后仍在 size_t 范围内。
    const std::size_t total = kHdrLen + static_cast<std::size_t>(payloadLen) + kCrcLen;
    if (total > kMaxFrame) {
        // 长度字段声称超过 maxFrame：早拒绝。
        Verdict bad = reject(Reason::kTooLong);
        bad.total = total;
        return bad;
    }
    if (frame.size() < total) {
        verdict.need = total - frame.size();
        return verdict;
    }
    if (frame.size() > total) {
        // 分流器在 kFrame 后立即复位，正常不会出现；防御性拒绝而非按超长下标读。
        Verdict bad = reject(Reason::kExtra);
        bad.total = total;
        return bad;
    }

    const std::size_t crcOfs = total - kCrcLen;
    const uint32_t crcCalc = crc::crc32Novatel(frame.first(crcOfs));
    const uint32_t crcRx = readU32Le(frame, crcOfs);

    if (crcCalc != crcRx) {
        Verdict bad = reject(Reason::kCrc);
        bad.received = crcRx;
        bad.computed = crcCalc;
        return bad;
    }
    Verdict good{};
    good.step = Step::kFrame;
    return good;
}

/**
 * @brief  把判定理由写成一句人读文本
 * @param  verdict: 判定结果
 * @param[out] text: 文本输出器（空间不足时截断）
 * @note   纯 switch 的文本映射，每个 case 独立无状态。
 */
void explainVerdict(const Verdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case Reason::kSync:
        text.literal("sync byte ")
            .decimal(verdict.offset + 1U)
            .literal(" is ")
            .hex(verdict.received & kByteMask, 2U)
            .literal(", expected ")
            .hex(verdict.received >> kByteShift, 2U);
        break;
    case Reason::kTooLong:
        text.literal("length ")
            .decimal(verdict.total)
            .literal(" exceeds ")
            .decimal(kMaxFrame)
            .literal(" bytes");
        break;
    case Reason::kExtra:
        text.literal("frame longer than the declared ").decimal(verdict.total).literal(" bytes");
        break;
    case Reason::kCrc:
        text.literal("bad CRC32, received ")
            .hex(verdict.received, kCrcDigits)
            .literal(", computed ")
            .hex(verdict.computed, kCrcDigits);
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
    return byte == kSyncA;
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

} // namespace demux::unicore_bin
