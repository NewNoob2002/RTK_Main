// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "app_rtcm.hpp"

#include "rtk/proto/frame.hpp"

namespace rtk::app {

void RtcmForwarder::forward(core::Role role, std::span<const uint8_t> wire) noexcept
{
    if (role != core::Role::kPhone) {
        ++stats_.rejected_role;
        return;
    }
    ++stats_.frames;
    // wire 已由 RTCM 解码器校验，长度 <= 1029，转换到 uint32_t 无损。
    stats_.bytes += static_cast<uint32_t>(wire.size());
    stats_.crc32 = proto::crc32(wire, stats_.crc32);

    constexpr std::size_t kHeaderBytes = 3;
    constexpr std::size_t kMinMessageBytes = 8; // 3 字节头 + 至少 2 字节载荷 + 3 字节 CRC。
    if (wire.size() >= kMinMessageBytes) {
        // 载荷首 12 bit 范围为 0..4095，转换到 uint16_t 无损。
        stats_.last_msg =
            static_cast<uint16_t>((static_cast<unsigned>(wire[kHeaderBytes]) << 4U) | (wire[kHeaderBytes + 1U] >> 4U));
    } else {
        // RTCM 帧封装允许不足 2 字节的载荷，此时没有完整消息号，避免读取 CRC 作为消息号。
        stats_.last_msg = 0;
    }
}

void RtcmForwarder::reset() noexcept
{
    stats_ = RtcmStats{};
}

const RtcmStats &RtcmForwarder::stats() const noexcept
{
    return stats_;
}

} // namespace rtk::app
