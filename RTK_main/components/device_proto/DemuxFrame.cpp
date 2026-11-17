// 产品 SPP 帧（AA 44 18）的 demux::Decoder 实现与线路帧 → 字段视图；判定与 dev_proto_parser_feed 一致（ADR-0004 §4 / ADR-0005 §1）。
// 纯函数、可重入、无静态可变状态，任意任务/ISR 可调用；无阻塞。
// 不使用堆；CRC 复用 proto::crc32。

#include "proto/DemuxFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "demux/TextWriter.hpp"
#include "demux/Decoder.hpp"
#include "proto/Frame.hpp"
#include "DevProtoFrame.h"

namespace proto::demux_frame {

namespace {

constexpr std::size_t kSyncLen = 3U;   // AA 44 18
constexpr std::size_t kSyncB1Len = 2U; // 同步到第 2 字节（含）
constexpr std::size_t kPosHdrLen = kOfsHeaderLen + 1U;
constexpr std::size_t kU16Bytes = 2U;
constexpr std::size_t kU32Bytes = 4U;
constexpr uint32_t kBitsPerByte = 8U;
constexpr uint32_t kByteMask = 0xFFU; // explain 同步诊断：received 低 8 位实收、高 8 位期望
constexpr std::size_t kPosMsgLen = kOfsMsgLen + kU16Bytes;
constexpr std::size_t kCrcOfsFromEnd = kCrcLen;
constexpr uint8_t kCrcDigits = 8U;

/** 拒绝原因；explain 据此生成一句话。 */
enum class Reason : uint8_t {
    kNone,
    kSync,
    kHeaderLen,
    kTooLong,
    kExtra,
    kCrc,
};

struct Verdict {
    demux::Step step = demux::Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;
    std::size_t offset = 0U; // 同步字节出错的位置
    uint32_t received = 0U;  // 同步/校验类原因：收到的值
    uint32_t computed = 0U;  // 校验类原因：算出的值
    std::size_t total = 0U;  // 长度类原因：声称的总长
};

/**
 * @brief  构造一个只带原因的拒绝结论
 * @param  reason: 拒绝原因
 * @retval step 为 kReject、reason 为 reason 的结论
 * @note   纯函数、无阻塞、ISR 安全
 */
Verdict reject(Reason reason) noexcept
{
    Verdict verdict{};
    verdict.step = demux::Step::kReject;
    verdict.reason = reason;
    return verdict;
}

/**
 * @brief  构造同步字节不符的拒绝结论
 * @param  offset: 不符字节的 0 基偏移（诊断时显示为 offset+1）
 * @param  expected: 期望字节
 * @param  got: 实收字节
 * @retval step 为 kReject、reason 为 kSync 的结论；received 低 8 位为 got、高 8 位为 expected
 * @note   纯函数、无阻塞、ISR 安全
 */
Verdict rejectSync(uint8_t offset, uint8_t expected, uint8_t got) noexcept
{
    Verdict verdict = reject(Reason::kSync);
    verdict.offset = offset;
    verdict.received =
        static_cast<uint32_t>(got) | (static_cast<uint32_t>(expected) << kBitsPerByte);
    return verdict;
}

/**
 * @brief  读小端 u16
 * @param  frame: 字节缓冲
 * @param  from: 起始偏移
 * @retval 该偏移处的无符号 16 位值
 * @note   不检查越界，调用方保证 from + 2 ≤ frame.size()；纯函数、ISR 安全
 */
uint32_t readU16Le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    return (static_cast<uint32_t>(frame[from]) | (static_cast<uint32_t>(frame[from + 1U]) << 8U));
}

/**
 * @brief  读小端 u32
 * @param  frame: 字节缓冲
 * @param  from: 起始偏移
 * @retval 该偏移处的无符号 32 位值
 * @note   不检查越界，调用方保证 from + 4 ≤ frame.size()；纯函数、ISR 安全
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
 * @brief  判定一帧：sync/头长/帧长/CRC
 * @param  frame: 候选帧（从同步字节开始，可为任意长度前缀）
 * @retval 结论：kFrame 表示整帧校验通过；kNeedMore 表示还需 verdict.need 字节；kReject 带原因与诊断字段
 * @note   协议早拒绝阶梯：sync/头长/帧长/CRC 的次序本身是内容，拆开反而看不出拒绝顺序（ADR-0013 §2.4）。
 *         纯函数、可重入、无阻塞、ISR 安全。
 */
Verdict judge(std::span<const uint8_t> frame) noexcept // NOLINT(readability-function-*)
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
        // 与 NovAtel/Unicore 等其它 AA 开头的协议在这里分开。
        return rejectSync(1U, kSyncB, frame[1]);
    }
    if (frame.size() < kSyncLen) {
        verdict.need = kSyncLen - frame.size();
        return verdict;
    }
    if (frame[2] != kSyncC) {
        // 与 Unicore 二进制（AA 44 B5）区分的关键早拒绝点。
        return rejectSync(2U, kSyncC, frame[2]);
    }
    if (frame.size() < kPosHdrLen) {
        verdict.need = kPosHdrLen - frame.size();
        return verdict;
    }
    // kHeaderLen == 20 < 256，窄化到 uint8_t 安全。
    if (frame[kOfsHeaderLen] != static_cast<uint8_t>(kHeaderLen)) {
        Verdict bad = reject(Reason::kHeaderLen);
        bad.received = static_cast<uint32_t>(frame[kOfsHeaderLen]);
        return bad;
    }
    if (frame.size() < kPosMsgLen) {
        verdict.need = kPosMsgLen - frame.size();
        return verdict;
    }

    const uint32_t payloadLen = readU16Le(frame, kOfsMsgLen);
    if (payloadLen > kMaxPayload) {
        Verdict bad = reject(Reason::kTooLong);
        bad.total = static_cast<std::size_t>(payloadLen);
        return bad;
    }
    // 20 + 256 + 4 = 280 = kMaxFrame，size_t 加法不溢出。
    const std::size_t total = kHeaderLen + static_cast<std::size_t>(payloadLen) + kCrcLen;
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

    const std::size_t crcOfs = total - kCrcOfsFromEnd;
    const uint32_t crcCalc = crc32(frame.first(crcOfs));
    const uint32_t crcRx = readU32Le(frame, crcOfs);

    if (crcCalc != crcRx) {
        Verdict bad = reject(Reason::kCrc);
        bad.received = crcRx;
        bad.computed = crcCalc;
        return bad;
    }
    Verdict good{};
    good.step = demux::Step::kFrame;
    return good;
}

/**
 * @brief  按 Verdict 的 reason/offset/received/total/computed 生成一句英文诊断
 * @param  verdict: judge 产生的拒绝结论
 * @param[out] text: 文本写入器；空间不足时 write 自行截断
 * @retval 无
 * @note   reason 为 kNone（或未知）时写 "rejected"；无分配、无阻塞、ISR 安全
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
            .hex(verdict.received >> kBitsPerByte, 2U);
        break;
    case Reason::kHeaderLen:
        text.literal("header length is ")
            .hex(verdict.received, 2U)
            .literal(", expected ")
            .hex(kHeaderLen, 2U);
        break;
    case Reason::kTooLong:
        text.literal("payload length ")
            .decimal(verdict.total)
            .literal(" exceeds ")
            .decimal(kMaxPayload);
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

demux::Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
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
    if (verdict.step == demux::Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explainVerdict(verdict, text);
    return text.finish();
}

std::optional<FrameView> view(std::span<const uint8_t> wire) noexcept
{
    const std::size_t minLen = kHeaderLen + kCrcLen;

    if (wire.size() < minLen) {
        return std::nullopt;
    }
    if ((wire[0] != kSyncA) || (wire[1] != kSyncB) || (wire[2] != kSyncC)) {
        return std::nullopt;
    }
    // kHeaderLen == 20 < 256，窄化到 uint8_t 安全。
    if (wire[kOfsHeaderLen] != static_cast<uint8_t>(kHeaderLen)) {
        return std::nullopt;
    }

    const uint32_t payloadLen = readU16Le(wire, kOfsMsgLen);
    if (payloadLen > kMaxPayload) {
        return std::nullopt;
    }
    const std::size_t expected = kHeaderLen + static_cast<std::size_t>(payloadLen) + kCrcLen;
    if (wire.size() != expected) {
        // 长度字段与线路帧实际长度不一致：调用方给的 span 不是这一帧。
        return std::nullopt;
    }

    FrameView out{};
    // msgId 是 16 bit 字段：readU16Le 的结果 < 2^16，窄化安全。
    out.msgId = static_cast<uint16_t>(readU16Le(wire, kOfsMsgId));
    out.sender = wire[kOfsSender];
    out.type = wire[kOfsType];
    out.protocol = wire[kOfsProtocol];
    out.interval = wire[kOfsInterval];
    out.payload = wire.subspan(kHeaderLen, static_cast<std::size_t>(payloadLen));
    return out;
}

} // namespace proto::demux_frame
