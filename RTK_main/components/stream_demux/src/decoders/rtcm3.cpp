// RTCM 3.x 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：第 2 字节高 6 位保留位为 0、
// 长度 10 bit、CRC-24Q 覆盖头 3 B + 载荷（3 B 大端）。长度一确定就一次要求整段剩余字节，CRC 在帧尾整段算。
// 早拒绝点：第 2 字节保留位（这是与其它 0xD3 数据分开的唯一可靠信号）。
// 纯函数、可重入、无静态可变状态；ISR 安全。

#include "rtk/demux/decoders/rtcm3.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/demux/crc.hpp"
#include "rtk/demux/decoder.hpp"

namespace rtk::demux::rtcm3 {

namespace {

constexpr uint8_t kSyncByte = 0xD3U;
constexpr uint8_t kReservedMask = 0xFCU; // 第 2 字节高 6 位必须为 0
constexpr uint8_t kLenHiMask = 0x03U;    // 第 2 字节低 2 位是长度的最高 2 位
constexpr std::size_t kSyncLen = 1U;
constexpr std::size_t kHdrLen = 3U;
constexpr std::size_t kCrcLen = 3U;
constexpr uint32_t kBitsPerByte = 8U;

} // namespace

bool first_byte(uint8_t byte) noexcept
{
    return byte == kSyncByte;
}

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 first_byte。
    if (frame.size() < kSyncLen + 1U) {
        need = (kSyncLen + 1U) - frame.size();
        return Step::kNeedMore;
    }
    if ((frame[1] & kReservedMask) != 0U) {
        return Step::kReject;
    }
    if (frame.size() < kHdrLen) {
        need = kHdrLen - frame.size();
        return Step::kNeedMore;
    }

    // 10 bit 长度：最大 1023，总长 = 3 + 1023 + 3 = 1029 = kMaxFrame，因此不需要额外超限判断。
    const uint32_t payload_len =
        ((static_cast<uint32_t>(frame[1]) & kLenHiMask) << kBitsPerByte) | static_cast<uint32_t>(frame[2]);
    const std::size_t total = static_cast<std::size_t>(payload_len) + kHdrLen + kCrcLen;

    if (frame.size() < total) {
        need = total - frame.size();
        return Step::kNeedMore;
    }
    if (frame.size() > total) {
        // 分流器在 kFrame 后立即复位，正常不会出现；防御性拒绝而非按超长下标读。
        return Step::kReject;
    }

    const std::size_t crc_ofs = total - kCrcLen;
    const uint32_t crc_calc = crc::crc24q(frame.first(crc_ofs));
    // 校验值 3 B 大端；每字节 < 256，移位后仍在 uint32_t 内。
    const uint32_t crc_rx = (static_cast<uint32_t>(frame[crc_ofs]) << 16U) |
                            (static_cast<uint32_t>(frame[crc_ofs + 1U]) << 8U) |
                            static_cast<uint32_t>(frame[crc_ofs + 2U]);

    return (crc_calc == crc_rx) ? Step::kFrame : Step::kReject;
}

} // namespace rtk::demux::rtcm3
