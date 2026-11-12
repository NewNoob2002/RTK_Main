// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
// RTCM 出口的单一所有者是链路任务；本阶段只统计，GNSS UART 管道由后续 ADR 定义。
#pragma once

#include <cstdint>
#include <span>

#include "rtk/core/types.hpp"

namespace rtk::app {

struct RtcmStats {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint32_t crc32 = 0;
    uint16_t last_msg = 0;
    uint32_t rejected_role = 0;
};

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

    /** 线程安全：否；仅链路任务，非 ISR。跨任务查询须使用 data::kRtcmStats 主题。 */
    [[nodiscard]] const RtcmStats &stats() const noexcept;

private:
    RtcmStats stats_{};
};

} // namespace rtk::app
