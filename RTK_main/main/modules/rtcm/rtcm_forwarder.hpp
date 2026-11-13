// RTCM 出口的单一所有者是链路任务；本阶段只统计，GNSS UART 管道由后续 ADR 定义。
#pragma once

#include <cstdint>
#include <span>

#include "core/types.hpp"

namespace app {

/** RTCM 转发统计快照。 */
struct RtcmStats {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint32_t crc32 = 0;
    uint16_t last_msg = 0;
    uint32_t rejected_role = 0;
    uint32_t bytes_per_sec = 0;
    uint32_t pipe_accepted_frames = 0;
    uint32_t pipe_accepted_bytes = 0;
    uint32_t pipe_dropped_frames = 0;
    uint32_t pipe_dropped_bytes = 0;
    uint32_t reset_epoch = 0;
};

/** RTCM 帧计数与 CRC-32 累计；无锁，仅 kLink 内部使用。 */
class RtcmForwarder {
public:
    constexpr RtcmForwarder() noexcept = default;
    RtcmForwarder(const RtcmForwarder &) = delete;
    RtcmForwarder &operator=(const RtcmForwarder &) = delete;
    ~RtcmForwarder() noexcept = default;

    /** 仅接收分流器已校验的完整 RTCM 线路帧。线程安全：否；仅链路任务，非 ISR。 */
    void forward(core::Role role, std::span<const uint8_t> wire) noexcept;

    /** 清零全部 RTCM 计数与累计 CRC。线程安全：否；仅链路任务，非 ISR。 */
    void reset() noexcept;

    /** 更新最近窗口测得的吞吐速率（字节/秒）。线程安全：否；仅链路任务，非 ISR。 */
    void set_bytes_per_sec(uint32_t bps) noexcept;

    /** 线程安全：否；仅链路任务，非 ISR。跨任务查询须使用 data::kRtcmStats 主题。 */
    [[nodiscard]] const RtcmStats &stats() const noexcept;

private:
    RtcmStats stats_{};
};

} // namespace app
