// RTCM 模块实现：处理分流器交付的 RTCM 批次统计与 `rtcm` 控制台命令（只在 kLink 任务或启动前调用）。
#include "modules/rtcm/rtcm_module.hpp"
#include "app_context.hpp"
#include "esp_log.h"

namespace rtk::app {
namespace {
constexpr size_t kRtcmHeaderBytes = 3;
constexpr size_t kRtcmOverheadBytes = 6;
constexpr uint8_t kLengthHighMask = 3;
constexpr unsigned kLengthHighShift = 8;
} // namespace
bool RtcmModule::poll(uint32_t /*now_ms*/) noexcept
{
    uint32_t version = 0;
    const auto batch = context_.data.rtcm_batch.read(version);
    if (version == batch_version_) {
        return false;
    }
    /* 只消费新批次，仍按原头部长度逐帧转发。 */
    batch_version_ = version;
    auto wire = batch.wire;
    while (!wire.empty()) {
        if (wire.size() < kRtcmHeaderBytes) {
            ESP_LOGE("rtcm", "truncated batch header");
            break;
        }
        const size_t len = ((wire[1] & kLengthHighMask) << kLengthHighShift) + wire[2] + kRtcmOverheadBytes;
        if (len > wire.size()) {
            ESP_LOGE("rtcm", "truncated batch frame");
            break;
        }
        forwarder_.forward(core::Role::kPhone, wire.first(len));
        wire = wire.subspan(len);
    }
    /* 先累计角色拒绝，再发布该批次的完整统计。 */
    for (uint32_t i = 0; i < batch.rejected; ++i) {
        forwarder_.forward(core::Role::kNone, {});
    }
    context_.data.rtcm_stats.write(forwarder_.stats());
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
    context_.data.rtcm_stats.write(forwarder_.stats());
    context_.data.rtcm_reset.write(context_.data.rtcm_reset.read() + 1U);
    result.status = fw::Result::kOk;
}
const RtcmStats &RtcmModule::stats() const noexcept
{
    return forwarder_.stats();
}
} // namespace rtk::app
