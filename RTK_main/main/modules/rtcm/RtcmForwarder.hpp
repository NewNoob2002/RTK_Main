// RTCM 转发统计的跨模块接口：按角色累计帧数、字节数与 CRC-32（GNSS UART 管道由后续 ADR 定义）。
// 写入方是 kLink 任务的分流回调，读取方是同任务上的 RTCM 模块；无内部同步，非 ISR 安全。
#pragma once

#include <cstdint>
#include <span>

#include "core/Types.hpp"

namespace app {

/** RTCM 转发统计快照。 */
struct RtcmStats {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint32_t crc32 = 0;
    uint16_t lastMsg = 0;
    uint32_t rejectedRole = 0;
    uint32_t rejectedMode = 0;
    uint32_t bytesPerSec = 0;
    uint32_t pipeAcceptedFrames = 0;
    uint32_t pipeAcceptedBytes = 0;
    uint32_t pipeDroppedFrames = 0;
    uint32_t pipeDroppedBytes = 0;
    uint32_t baseTxFrames = 0;
    uint32_t baseTxBytes = 0;
    uint32_t baseTxDrop = 0;
    uint32_t resetEpoch = 0;
};

/** RTCM 帧计数与 CRC-32 累计；无锁，仅 kLink 内部使用。 */
class RtcmForwarder {
public:
    constexpr RtcmForwarder() noexcept = default;
    RtcmForwarder(const RtcmForwarder &) = delete;
    RtcmForwarder &operator=(const RtcmForwarder &) = delete;
    ~RtcmForwarder() noexcept = default;

    /**
     * @brief 仅接收分流器已校验的完整 RTCM 线路帧。线程安全：否；仅链路任务，非 ISR。
     * @param role: 帧来源会话角色
     * @param wire: 已校验的线路帧字节
     * @note 仅 kLink，非 ISR、非阻塞；拒绝非 PHONE 来源。
     */
    void forward(core::Role role, std::span<const uint8_t> wire) noexcept;

    /**
     * @brief 清零全部 RTCM 计数与累计 CRC。线程安全：否；仅链路任务，非 ISR。
     */
    void reset() noexcept;

    /**
     * @brief 更新最近窗口测得的吞吐速率（字节/秒）。线程安全：否；仅链路任务，非 ISR。
     * @param bps: 最近窗口吞吐（字节/秒）
     * @note 仅 kLink，非 ISR、非阻塞。
     */
    void setBytesPerSec(uint32_t bps) noexcept;

    /**
     * @brief 线程安全：否；仅链路任务，非 ISR。跨任务查询须使用 data::kRtcmStats 主题。
     * @retval 当前累计统计的只读引用
     * @note 仅 kLink，非 ISR、非阻塞；跨任务读主题快照。
     */
    [[nodiscard]] const RtcmStats &stats() const noexcept;

private:
    RtcmStats stats_{};
};

} // namespace app
