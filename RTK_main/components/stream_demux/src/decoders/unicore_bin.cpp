// Unicore 二进制解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：
// AA 44 B5 | 24 B 固定头（载荷长 u16 LE 在偏移 6）| 载荷 | CRC-32（0xEDB88320 反射、初值 0、不取反）4 B LE。
// 早拒绝点：第 2 字节（44）、第 3 字节（B5，与同首字节的产品帧 AA 44 18 分开）；长度超限；CRC 错。
// 长度一确定就一次要求整段剩余字节，CRC 在帧尾整段算。纯函数、可重入、无静态可变状态；ISR 安全。

#include "demux/decoders/unicore_bin.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/crc.hpp"
#include "demux/decoder.hpp"

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
constexpr uint32_t kBitsPerByte = 8U;
constexpr std::size_t kBytesToLenField = kOfsLen + kU16Bytes;

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

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 first_byte。
    if (frame.size() < kSyncB1Len) {
        need = kSyncB1Len - frame.size();
        return Step::kNeedMore;
    }
    if (frame[1] != kSyncB) {
        return Step::kReject;
    }
    if (frame.size() < kSyncLen) {
        need = kSyncLen - frame.size();
        return Step::kNeedMore;
    }
    if (frame[2] != kSyncC) {
        // 与同以 AA 44 开头的产品帧（AA 44 18）在这里分开。
        return Step::kReject;
    }
    if (frame.size() < kBytesToLenField) {
        need = kBytesToLenField - frame.size();
        return Step::kNeedMore;
    }

    const uint32_t payload_len = read_u16_le(frame, kOfsLen);
    // payload_len ≤ 0xFFFF，加固定头与 CRC 后仍在 size_t 范围内。
    const std::size_t total = kHdrLen + static_cast<std::size_t>(payload_len) + kCrcLen;
    if (total > kMaxFrame) {
        // 长度字段声称超过 max_frame：早拒绝。
        return Step::kReject;
    }
    if (frame.size() < total) {
        need = total - frame.size();
        return Step::kNeedMore;
    }
    if (frame.size() > total) {
        // 分流器在 kFrame 后立即复位，正常不会出现；防御性拒绝而非按超长下标读。
        return Step::kReject;
    }

    const std::size_t crc_ofs = total - kCrcLen;
    const uint32_t crc_calc = crc::crc32_novatel(frame.first(crc_ofs));
    const uint32_t crc_rx = read_u32_le(frame, crc_ofs);

    return (crc_calc == crc_rx) ? Step::kFrame : Step::kReject;
}

} // namespace demux::unicore_bin
