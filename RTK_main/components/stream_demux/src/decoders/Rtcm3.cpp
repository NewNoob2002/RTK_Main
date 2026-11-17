// RTCM 3.x 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：0xD3 | 第 2 字节高 6 位保留位为 0 且低 2 位
// 是长度高 2 位 | 长度 10 bit | 载荷 | CRC-24Q（3 B 大端，覆盖头 3 B + 载荷）；保留位非 0 即早拒绝。
// 纯函数、无静态可变状态、无堆、不阻塞：可重入、ISR 安全；长度一确定就要整段剩余字节，CRC 在帧尾整段算。

#include "demux/decoders/Rtcm3.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/TextWriter.hpp"
#include "demux/Crc.hpp"
#include "demux/Decoder.hpp"

namespace demux::rtcm3 {

namespace {

constexpr uint8_t kSyncByte = 0xD3U;
constexpr uint8_t kReservedMask = 0xFCU; // 第 2 字节高 6 位必须为 0
constexpr uint8_t kLenHiMask = 0x03U;    // 第 2 字节低 2 位是长度的最高 2 位
constexpr std::size_t kSyncLen = 1U;
constexpr std::size_t kHdrLen = 3U;
constexpr std::size_t kCrcLen = 3U;
constexpr uint32_t kBitsPerByte = 8U;
constexpr uint8_t kCrcDigits = 6U; // CRC-24Q 的十六进制位数

/** 拒绝原因；explain 据此生成一句话。 */
enum class Reason : uint8_t {
    kNone,
    kSync,
    kReserved,
    kExtra,
    kCrc,
};

/** judge() 的结论：判定、原因、待补字节数与诊断数值。 */
struct Verdict {
    Step step = Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;
    uint32_t received = 0U;
    uint32_t computed = 0U;
    std::size_t total = 0U;
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
 * @brief  判定一帧：同步字节 / 保留位 / 长度 / CRC-24Q
 * @param  frame: 候选前缀，非空且 frame[0] 已通过 firstByte
 * @retval step=kNeedMore 还不能下结论，need 为至少还要的字节数（长度未知时为 1）
 * @retval step=kFrame 长度与 CRC-24Q 都通过
 * @retval step=kReject 同步字节错（kSync）、保留位非 0（kReserved）、比声明长度长（kExtra）
 *         或 CRC 不符（kCrc）
 * @note   纯函数；长度字段一到就要求整段剩余字节（含 3 B CRC），CRC 整段一次算完。
 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    if (frame[0] != kSyncByte) {
        return reject(Reason::kSync);
    }
    if (frame.size() < kSyncLen + 1U) {
        verdict.need = (kSyncLen + 1U) - frame.size();
        return verdict;
    }
    if ((frame[1] & kReservedMask) != 0U) {
        Verdict bad = reject(Reason::kReserved);
        bad.received = frame[1];
        return bad;
    }
    if (frame.size() < kHdrLen) {
        verdict.need = kHdrLen - frame.size();
        return verdict;
    }

    // 10 bit 长度：最大 1023，总长 = 3 + 1023 + 3 = 1029 = kMaxFrame，因此不需要额外超限判断。
    const uint32_t payloadLen = ((static_cast<uint32_t>(frame[1]) & kLenHiMask) << kBitsPerByte) |
                                static_cast<uint32_t>(frame[2]);
    const std::size_t total = static_cast<std::size_t>(payloadLen) + kHdrLen + kCrcLen;

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
    const uint32_t crcCalc = crc::crc24q(frame.first(crcOfs));
    // 校验值 3 B 大端；每字节 < 256，移位后仍在 uint32_t 内。
    const uint32_t crcRx = (static_cast<uint32_t>(frame[crcOfs]) << 16U) |
                           (static_cast<uint32_t>(frame[crcOfs + 1U]) << 8U) |
                           static_cast<uint32_t>(frame[crcOfs + 2U]);

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
        text.literal("byte 0 is ")
            .hex(static_cast<uint32_t>(kSyncByte), 2U)
            .literal(", expected 0xD3");
        break;
    case Reason::kReserved:
        text.literal("reserved bits set in byte 1: ").hex(verdict.received, 2U);
        break;
    case Reason::kExtra:
        text.literal("frame longer than the declared ").decimal(verdict.total).literal(" bytes");
        break;
    case Reason::kCrc:
        text.literal("bad CRC-24Q, received ")
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

} // namespace demux::rtcm3
