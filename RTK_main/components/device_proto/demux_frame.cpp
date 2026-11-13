// 产品 SPP 帧（AA 44 18）的分流器 C++ 解码器与线路帧 → 字段视图（ADR-0004 §4 / ADR-0005 §1）。
// 判定规则与 dev_proto_parser_feed（components/device_proto/dev_proto_frame.c）完全一致：AA 44 18、
// 头长字节 == 0x14、payload_len（偏移 12，u16 LE）≤ DEV_PROTO_MAX_PAYLOAD、CRC-32 正确；
// CRC 复用 proto::crc32（zlib 兼容，不另建实现）。
// 早拒绝点：第 2 字节（44）、第 3 字节（18，与 Unicore 二进制 AA 44 B5 分开）、头长字节、payload 长度字段。
// 长度一确定就一次要求整段剩余字节，CRC 在帧尾整段算。纯函数、可重入、无静态可变状态；ISR 安全。
// step 与 explain 共用 judge()，避免诊断文本与判定结论漂移。

#include "proto/demux_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "demux/text_writer.hpp"
#include "demux/decoder.hpp"
#include "proto/frame.hpp"
#include "dev_proto_frame.h"

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

Verdict reject(Reason reason) noexcept
{
    Verdict verdict{};
    verdict.step = demux::Step::kReject;
    verdict.reason = reason;
    return verdict;
}

Verdict reject_sync(uint8_t offset, uint8_t expected, uint8_t got) noexcept
{
    Verdict verdict = reject(Reason::kSync);
    verdict.offset = offset;
    verdict.received = static_cast<uint32_t>(got) | (static_cast<uint32_t>(expected) << kBitsPerByte);
    return verdict;
}

/** 小端 u16；调用方保证 from + 2 <= frame.size()。 */
uint32_t read_u16_le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    return (static_cast<uint32_t>(frame[from]) | (static_cast<uint32_t>(frame[from + 1U]) << 8U));
}

/** 小端 u32；调用方保证 from + 4 <= frame.size()。 */
uint32_t read_u32_le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    uint32_t value = 0U;

    for (std::size_t idx = 0U; idx < kU32Bytes; ++idx) {
        // idx ≤ 3，窄化到 uint32_t 安全；移位量 = idx * 8 ≤ 24，uint32_t 内不溢出。
        value |= static_cast<uint32_t>(frame[from + idx]) << (static_cast<uint32_t>(idx) * kBitsPerByte);
    }
    return value;
}

/** 判定：step 与 explain 的唯一事实来源。 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    if (frame[0] != kSyncA) {
        return reject_sync(0U, kSyncA, frame[0]);
    }
    if (frame.size() < kSyncB1Len) {
        verdict.need = kSyncB1Len - frame.size();
        return verdict;
    }
    if (frame[1] != kSyncB) {
        // 与 NovAtel/Unicore 等其它 AA 开头的协议在这里分开。
        return reject_sync(1U, kSyncB, frame[1]);
    }
    if (frame.size() < kSyncLen) {
        verdict.need = kSyncLen - frame.size();
        return verdict;
    }
    if (frame[2] != kSyncC) {
        // 与 Unicore 二进制（AA 44 B5）区分的关键早拒绝点。
        return reject_sync(2U, kSyncC, frame[2]);
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

    const uint32_t payload_len = read_u16_le(frame, kOfsMsgLen);
    if (payload_len > kMaxPayload) {
        Verdict bad = reject(Reason::kTooLong);
        bad.total = static_cast<std::size_t>(payload_len);
        return bad;
    }
    // 20 + 256 + 4 = 280 = kMaxFrame，size_t 加法不溢出。
    const std::size_t total = kHeaderLen + static_cast<std::size_t>(payload_len) + kCrcLen;
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

    const std::size_t crc_ofs = total - kCrcOfsFromEnd;
    const uint32_t crc_calc = crc32(frame.first(crc_ofs));
    const uint32_t crc_rx = read_u32_le(frame, crc_ofs);

    if (crc_calc != crc_rx) {
        Verdict bad = reject(Reason::kCrc);
        bad.received = crc_rx;
        bad.computed = crc_calc;
        return bad;
    }
    Verdict good{};
    good.step = demux::Step::kFrame;
    return good;
}

void explain_verdict(const Verdict &verdict, demux::text_writer::TextWriter &text) noexcept
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
        text.literal("header length is ").hex(verdict.received, 2U).literal(", expected ").hex(kHeaderLen, 2U);
        break;
    case Reason::kTooLong:
        text.literal("payload length ").decimal(verdict.total).literal(" exceeds ").decimal(kMaxPayload);
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

bool first_byte(uint8_t byte) noexcept
{
    return byte == kSyncA;
}

demux::Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
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
    if (verdict.step == demux::Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explain_verdict(verdict, text);
    return text.finish();
}

std::optional<FrameView> view(std::span<const uint8_t> wire) noexcept
{
    const std::size_t min_len = kHeaderLen + kCrcLen;

    if (wire.size() < min_len) {
        return std::nullopt;
    }
    if ((wire[0] != kSyncA) || (wire[1] != kSyncB) || (wire[2] != kSyncC)) {
        return std::nullopt;
    }
    // kHeaderLen == 20 < 256，窄化到 uint8_t 安全。
    if (wire[kOfsHeaderLen] != static_cast<uint8_t>(kHeaderLen)) {
        return std::nullopt;
    }

    const uint32_t payload_len = read_u16_le(wire, kOfsMsgLen);
    if (payload_len > kMaxPayload) {
        return std::nullopt;
    }
    const std::size_t expected = kHeaderLen + static_cast<std::size_t>(payload_len) + kCrcLen;
    if (wire.size() != expected) {
        // 长度字段与线路帧实际长度不一致：调用方给的 span 不是这一帧。
        return std::nullopt;
    }

    FrameView out{};
    // msg_id 是 16 bit 字段：read_u16_le 的结果 < 2^16，窄化安全。
    out.msg_id = static_cast<uint16_t>(read_u16_le(wire, kOfsMsgId));
    out.sender = wire[kOfsSender];
    out.type = wire[kOfsType];
    out.protocol = wire[kOfsProtocol];
    out.interval = wire[kOfsInterval];
    out.payload = wire.subspan(kHeaderLen, static_cast<std::size_t>(payload_len));
    return out;
}

} // namespace proto::demux_frame
