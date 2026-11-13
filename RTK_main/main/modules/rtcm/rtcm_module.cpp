// RTCM 模块实现：处理分流器交付的 RTCM 批次统计与 `rtcm` 控制台命令（只在 kLink 任务或启动前调用）。
#include "modules/rtcm/rtcm_module.hpp"
#include "app_context.hpp"
#include "fw/tlog.hpp"

namespace app {
namespace {
constexpr size_t kRtcmHeaderBytes = 3;
constexpr size_t kRtcmOverheadBytes = 6;
constexpr uint8_t kLengthHighMask = 3;
constexpr unsigned kLengthHighShift = 8;
constexpr uint32_t kRateWindowMs = 1000U;
constexpr uint64_t kMsPerSecond = 1000ULL;
} // namespace
bool RtcmModule::poll(uint32_t now_ms) noexcept
{
    if (!rate_started_) {
        rate_started_ = true;
        rate_window_ms_ = now_ms;
        rate_window_bytes_ = forwarder_.stats().bytes;
    }

    bool dirty = false;
    uint32_t version = 0;
    const auto batch = context_.data.rtcm_batch.read(version);
    if (version != batch_version_) {
        /* 只消费新批次，仍按原头部长度逐帧转发。 */
        batch_version_ = version;
        auto wire = batch.wire;
        while (!wire.empty()) {
            if (wire.size() < kRtcmHeaderBytes) {
                fw::tlog(fw::TraceCat::kRtcm, "batch truncated, %u bytes left is shorter than an RTCM header", static_cast<unsigned>(wire.size()));
                break;
            }
            const size_t len = ((wire[1] & kLengthHighMask) << kLengthHighShift) + wire[2] + kRtcmOverheadBytes;
            if (len > wire.size()) {
                fw::tlog(fw::TraceCat::kRtcm, "batch truncated, frame needs %u bytes but %u remain", static_cast<unsigned>(len), static_cast<unsigned>(wire.size()));
                break;
            }
            const auto frame = wire.first(len);
            forwarder_.forward(core::Role::kPhone, frame);
#if RTK_GNSS_ENABLED
            // 管道满时 RtcmPipe::push 内部已计数并打 kPipeDrop 追踪，此处尽力投递即可。
            static_cast<void>(context_.rtcm_pipe.push(frame));
#endif
            wire = wire.subspan(len);
        }
        /* 先累计角色拒绝，再发布该批次的完整统计。 */
        for (uint32_t i = 0; i < batch.rejected; ++i) {
            forwarder_.forward(core::Role::kNone, {});
        }
        dirty = true;
    }

    const uint32_t elapsed = now_ms - rate_window_ms_;
    if (elapsed >= kRateWindowMs) {
        const uint32_t total_bytes = forwarder_.stats().bytes;
        const uint32_t delta = total_bytes - rate_window_bytes_;
        // elapsed >= 1000，乘以 1000 不会溢出 uint64_t；且字节增量小于 4GB/s，转换到 uint32_t 安全。
        const uint32_t bps = static_cast<uint32_t>((static_cast<uint64_t>(delta) * kMsPerSecond) / elapsed);
        if (bps != forwarder_.stats().bytes_per_sec) {
            forwarder_.set_bytes_per_sec(bps);
            dirty = true;
        }
        rate_window_ms_ = now_ms;
        rate_window_bytes_ = total_bytes;
    }

    if (dirty) {
        context_.data.rtcm_stats.write(forwarder_.stats());
    }
    return false;
}
void RtcmModule::on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.verb != verb(RtcmVerb::kReset) || cmd.arg_len != 0U) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    /* 不清除分流器半帧；先清统计，再通知 bt_link 复位统计世代。 */
    forwarder_.reset();
    rate_started_ = false;
    rate_window_ms_ = 0U;
    rate_window_bytes_ = 0U;
    context_.data.rtcm_stats.write(forwarder_.stats());
    context_.data.rtcm_reset.write(context_.data.rtcm_reset.read() + 1U);
    result.status = fw::Result::kOk;
}
const RtcmStats &RtcmModule::stats() const noexcept
{
    return forwarder_.stats();
}
} // namespace app
