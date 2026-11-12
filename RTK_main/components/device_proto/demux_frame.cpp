// 产品 SPP 帧（AA 44 18）的分流器 C++ 解码器与线路帧 → 字段视图（ADR-0004 §4 / ADR-0005 §1）。
// 判定规则与 dev_proto_parser_feed（components/device_proto/dev_proto_frame.c）完全一致：AA 44 18、
// 头长字节 == 0x14、payload_len（偏移 12，u16 LE）≤ DEV_PROTO_MAX_PAYLOAD、CRC-32 正确；
// CRC 复用 proto::crc32（zlib 兼容，不另建实现）。
// 早拒绝点：第 2 字节（44）、第 3 字节（18，与 Unicore 二进制 AA 44 B5 分开）、头长字节、payload 长度字段。
// 长度一确定就一次要求整段剩余字节，CRC 在帧尾整段算。纯函数、可重入、无静态可变状态；ISR 安全。

#include "proto/demux_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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
constexpr std::size_t kPosMsgLen = kOfsMsgLen + kU16Bytes;
constexpr std::size_t kCrcOfsFromEnd = kCrcLen;

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

} // namespace

bool first_byte(uint8_t byte) noexcept
{
    return byte == kSyncA;
}

demux::Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 first_byte。
    if (frame.size() < kSyncB1Len) {
        need = kSyncB1Len - frame.size();
        return demux::Step::kNeedMore;
    }
    if (frame[1] != kSyncB) {
        // 与 NovAtel/Unicore 等其它 AA 开头的协议在这里分开。
        return demux::Step::kReject;
    }
    if (frame.size() < kSyncLen) {
        need = kSyncLen - frame.size();
        return demux::Step::kNeedMore;
    }
    if (frame[2] != kSyncC) {
        // 与 Unicore 二进制（AA 44 B5）区分的关键早拒绝点。
        return demux::Step::kReject;
    }
    if (frame.size() < kPosHdrLen) {
        need = kPosHdrLen - frame.size();
        return demux::Step::kNeedMore;
    }
    // kHeaderLen == 20 < 256，窄化到 uint8_t 安全。
    if (frame[kOfsHeaderLen] != static_cast<uint8_t>(kHeaderLen)) {
        return demux::Step::kReject;
    }
    if (frame.size() < kPosMsgLen) {
        need = kPosMsgLen - frame.size();
        return demux::Step::kNeedMore;
    }

    const uint32_t payload_len = read_u16_le(frame, kOfsMsgLen);
    if (payload_len > kMaxPayload) {
        return demux::Step::kReject;
    }
    // 20 + 256 + 4 = 280 = kMaxFrame，size_t 加法不溢出。
    const std::size_t total = kHeaderLen + static_cast<std::size_t>(payload_len) + kCrcLen;
    if (frame.size() < total) {
        need = total - frame.size();
        return demux::Step::kNeedMore;
    }
    if (frame.size() > total) {
        // 分流器在 kFrame 后立即复位，正常不会出现；防御性拒绝而非按超长下标读。
        return demux::Step::kReject;
    }

    const std::size_t crc_ofs = total - kCrcOfsFromEnd;
    const uint32_t crc_calc = crc32(frame.first(crc_ofs));
    const uint32_t crc_rx = read_u32_le(frame, crc_ofs);

    return (crc_calc == crc_rx) ? demux::Step::kFrame : demux::Step::kReject;
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
