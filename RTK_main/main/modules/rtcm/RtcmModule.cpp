// RTCM 模块实现：读取 kLink 直推侧的累计统计快照，计算吞吐率并管理 resetEpoch。
// 只在 kLink 任务或启动前调用；只读快照与累计，不阻塞，非 ISR 安全。
#include "modules/rtcm/RtcmModule.hpp"
#include "AppContext.hpp"

namespace app {
namespace {
constexpr uint32_t kRateWindowMs = 1000U;
constexpr uint64_t kMsPerSecond = 1000ULL;
} // namespace
void RtcmModule::startRateWindow(uint32_t nowMs) noexcept
{
    if (rate_started_) {
        return;
    }
    rate_started_ = true;
    rate_window_ms_ = nowMs;
    rate_window_bytes_ = stats_.bytes;
}

void RtcmModule::takeSnapshot(const RtcmLinkSnapshot &snap, RtcmStats &next) const noexcept
{
    if (snap.resetEpoch == reset_epoch_) {
        next.frames = snap.frames;
        next.bytes = snap.bytes;
        next.crc32 = snap.crc32;
        next.lastMsg = snap.lastMsg;
        next.rejectedRole = snap.rejectedRole;
        next.pipeAcceptedFrames = snap.pipeAcceptedFrames;
        next.pipeAcceptedBytes = snap.pipeAcceptedBytes;
        next.pipeDroppedFrames = snap.pipeDroppedFrames;
        next.pipeDroppedBytes = snap.pipeDroppedBytes;
        return;
    }
    // bt_link 尚未执行新一轮 reset 切换时，按 reset 时基线扣减增量，避免回滚旧累计。
    next.frames = (snap.frames >= baseline_.frames) ? (snap.frames - baseline_.frames) : 0U;
    next.bytes = (snap.bytes >= baseline_.bytes) ? (snap.bytes - baseline_.bytes) : 0U;
    next.rejectedRole = (snap.rejectedRole >= baseline_.rejectedRole)
                            ? (snap.rejectedRole - baseline_.rejectedRole)
                            : 0U;
    next.pipeAcceptedFrames = (snap.pipeAcceptedFrames >= baseline_.pipeAcceptedFrames)
                                  ? (snap.pipeAcceptedFrames - baseline_.pipeAcceptedFrames)
                                  : 0U;
    next.pipeAcceptedBytes = (snap.pipeAcceptedBytes >= baseline_.pipeAcceptedBytes)
                                 ? (snap.pipeAcceptedBytes - baseline_.pipeAcceptedBytes)
                                 : 0U;
    next.pipeDroppedFrames = (snap.pipeDroppedFrames >= baseline_.pipeDroppedFrames)
                                 ? (snap.pipeDroppedFrames - baseline_.pipeDroppedFrames)
                                 : 0U;
    next.pipeDroppedBytes = (snap.pipeDroppedBytes >= baseline_.pipeDroppedBytes)
                                ? (snap.pipeDroppedBytes - baseline_.pipeDroppedBytes)
                                : 0U;
    next.crc32 = 0U;
    next.lastMsg = 0U;
}

bool RtcmModule::refreshRate(uint32_t nowMs, RtcmStats &next) noexcept
{
    const uint32_t elapsed = nowMs - rate_window_ms_;
    if (elapsed < kRateWindowMs) {
        return false;
    }
    const uint32_t totalBytes = next.bytes;
    const uint32_t delta = totalBytes - rate_window_bytes_;
    // elapsed >= 1000，乘以 1000 不会溢出 uint64_t；且字节增量小于 4GB/s，转换到 uint32_t 安全。
    const uint32_t bps =
        static_cast<uint32_t>((static_cast<uint64_t>(delta) * kMsPerSecond) / elapsed);
    rate_window_ms_ = nowMs;
    rate_window_bytes_ = totalBytes;
    if (bps == next.bytesPerSec) {
        return false;
    }
    next.bytesPerSec = bps;
    return true;
}

bool RtcmModule::poll(uint32_t nowMs) noexcept
{
    startRateWindow(nowMs);
    RtcmStats next = stats_;
    takeSnapshot(ctx().bt_link.rtcmSnapshot(), next);
    next.resetEpoch = reset_epoch_;

    const bool changed = (next.frames != stats_.frames) || (next.bytes != stats_.bytes) ||
                         (next.crc32 != stats_.crc32) || (next.lastMsg != stats_.lastMsg) ||
                         (next.rejectedRole != stats_.rejectedRole) ||
                         (next.pipeAcceptedFrames != stats_.pipeAcceptedFrames) ||
                         (next.pipeAcceptedBytes != stats_.pipeAcceptedBytes) ||
                         (next.pipeDroppedFrames != stats_.pipeDroppedFrames) ||
                         (next.pipeDroppedBytes != stats_.pipeDroppedBytes) ||
                         (next.resetEpoch != stats_.resetEpoch);
    const bool rateRolled = refreshRate(nowMs, next);
    if (!changed && !rateRolled) {
        return false;
    }
    stats_ = next;
    ctx().data.rtcmStats.write(stats_);
    return false;
}

void RtcmModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.verb != verb(RtcmVerb::kReset) || cmd.argLen != 0U) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    /* 不清除分流器半帧；先清统计与速率窗口，再递增 resetEpoch 并通知 bt_link 复位统计世代。 */
    ++reset_epoch_;
    baseline_ = ctx().bt_link.rtcmSnapshot();
    stats_ = RtcmStats{};
    stats_.resetEpoch = reset_epoch_;
    rate_started_ = false;
    rate_window_ms_ = 0U;
    rate_window_bytes_ = 0U;
    ctx().data.rtcmStats.write(stats_);
    ctx().data.demuxStats.write(DemuxStats{});
    ctx().data.rtcmReset.write(reset_epoch_);
    result.status = fw::Result::kOk;
}
const RtcmStats &RtcmModule::stats() const noexcept
{
    return stats_;
}
} // namespace app
