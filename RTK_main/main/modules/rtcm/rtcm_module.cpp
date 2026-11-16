// RTCM 模块实现：读取 kLink 直推侧的累计统计快照，计算吞吐率并管理 reset_epoch（只在 kLink 任务或启动前调用）。
#include "modules/rtcm/rtcm_module.hpp"
#include "app_context.hpp"

namespace app {
namespace {
constexpr uint32_t kRateWindowMs = 1000U;
constexpr uint64_t kMsPerSecond = 1000ULL;
} // namespace
bool RtcmModule::poll(uint32_t now_ms) noexcept
{
    if (!rate_started_) {
        rate_started_ = true;
        rate_window_ms_ = now_ms;
        rate_window_bytes_ = stats_.bytes;
    }

    const RtcmLinkSnapshot snap = ctx().bt_link.rtcm_snapshot();
    RtcmStats next = stats_;
    if (snap.reset_epoch == reset_epoch_) {
        next.frames = snap.frames;
        next.bytes = snap.bytes;
        next.crc32 = snap.crc32;
        next.last_msg = snap.last_msg;
        next.rejected_role = snap.rejected_role;
        next.pipe_accepted_frames = snap.pipe_accepted_frames;
        next.pipe_accepted_bytes = snap.pipe_accepted_bytes;
        next.pipe_dropped_frames = snap.pipe_dropped_frames;
        next.pipe_dropped_bytes = snap.pipe_dropped_bytes;
    } else {
        // bt_link 尚未执行新一轮 reset 切换时，按 reset 时基线扣减增量，避免回滚旧累计。
        next.frames = (snap.frames >= baseline_.frames) ? (snap.frames - baseline_.frames) : 0U;
        next.bytes = (snap.bytes >= baseline_.bytes) ? (snap.bytes - baseline_.bytes) : 0U;
        next.rejected_role =
            (snap.rejected_role >= baseline_.rejected_role) ? (snap.rejected_role - baseline_.rejected_role) : 0U;
        next.pipe_accepted_frames = (snap.pipe_accepted_frames >= baseline_.pipe_accepted_frames)
                                        ? (snap.pipe_accepted_frames - baseline_.pipe_accepted_frames)
                                        : 0U;
        next.pipe_accepted_bytes = (snap.pipe_accepted_bytes >= baseline_.pipe_accepted_bytes)
                                       ? (snap.pipe_accepted_bytes - baseline_.pipe_accepted_bytes)
                                       : 0U;
        next.pipe_dropped_frames = (snap.pipe_dropped_frames >= baseline_.pipe_dropped_frames)
                                       ? (snap.pipe_dropped_frames - baseline_.pipe_dropped_frames)
                                       : 0U;
        next.pipe_dropped_bytes = (snap.pipe_dropped_bytes >= baseline_.pipe_dropped_bytes)
                                      ? (snap.pipe_dropped_bytes - baseline_.pipe_dropped_bytes)
                                      : 0U;
        next.crc32 = 0U;
        next.last_msg = 0U;
    }
    next.reset_epoch = reset_epoch_;

    bool dirty = (next.frames != stats_.frames) || (next.bytes != stats_.bytes) || (next.crc32 != stats_.crc32) ||
                 (next.last_msg != stats_.last_msg) || (next.rejected_role != stats_.rejected_role) ||
                 (next.pipe_accepted_frames != stats_.pipe_accepted_frames) ||
                 (next.pipe_accepted_bytes != stats_.pipe_accepted_bytes) ||
                 (next.pipe_dropped_frames != stats_.pipe_dropped_frames) ||
                 (next.pipe_dropped_bytes != stats_.pipe_dropped_bytes) || (next.reset_epoch != stats_.reset_epoch);

    const uint32_t elapsed = now_ms - rate_window_ms_;
    if (elapsed >= kRateWindowMs) {
        const uint32_t total_bytes = next.bytes;
        const uint32_t delta = total_bytes - rate_window_bytes_;
        // elapsed >= 1000，乘以 1000 不会溢出 uint64_t；且字节增量小于 4GB/s，转换到 uint32_t 安全。
        const uint32_t bps = static_cast<uint32_t>((static_cast<uint64_t>(delta) * kMsPerSecond) / elapsed);
        if (bps != next.bytes_per_sec) {
            next.bytes_per_sec = bps;
            dirty = true;
        }
        rate_window_ms_ = now_ms;
        rate_window_bytes_ = total_bytes;
    }

    if (dirty) {
        stats_ = next;
        ctx().data.rtcm_stats.write(stats_);
    }
    return false;
}
void RtcmModule::on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.verb != verb(RtcmVerb::kReset) || cmd.arg_len != 0U) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    /* 不清除分流器半帧；先清统计与速率窗口，再递增 reset_epoch 并通知 bt_link 复位统计世代。 */
    ++reset_epoch_;
    baseline_ = ctx().bt_link.rtcm_snapshot();
    stats_ = RtcmStats{};
    stats_.reset_epoch = reset_epoch_;
    rate_started_ = false;
    rate_window_ms_ = 0U;
    rate_window_bytes_ = 0U;
    ctx().data.rtcm_stats.write(stats_);
    ctx().data.demux_stats.write(DemuxStats{});
    ctx().data.rtcm_reset.write(reset_epoch_);
    result.status = fw::Result::kOk;
}
const RtcmStats &RtcmModule::stats() const noexcept
{
    return stats_;
}
} // namespace app
