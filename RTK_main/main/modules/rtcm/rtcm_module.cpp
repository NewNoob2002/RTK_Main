// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "modules/rtcm/rtcm_module.hpp"
#include "esp_log.h"

namespace rtk::app {
constinit RtcmModule s_rtcm_module{};
namespace {
constexpr size_t kRtcmHeaderBytes = 3;
constexpr size_t kRtcmOverheadBytes = 6;
constexpr uint8_t kLengthHighMask = 3;
} // namespace
bool rtcm_poll(void *self, uint32_t /*now_ms*/) noexcept
{
    // ModuleOps 固定绑定 RtcmModule，所有访问同属 kLink。
    auto &module = *static_cast<RtcmModule *>(self);
    uint32_t version = 0;
    const auto batch = data::kRtcmBatch.read(version);
    if (version == module.batch_version) {
        return false;
    }
    module.batch_version = version;
    auto wire = batch.wire;
    while (!wire.empty()) {
        if (wire.size() < kRtcmHeaderBytes) {
            ESP_LOGE("rtcm", "truncated batch header");
            break;
        }
        const size_t len = ((wire[1] & kLengthHighMask) << 8U) + wire[2] + kRtcmOverheadBytes;
        if (len > wire.size()) {
            ESP_LOGE("rtcm", "truncated batch frame");
            break;
        }
        module.forwarder.forward(core::Role::kPhone, wire.first(len));
        wire = wire.subspan(len);
    }
    for (uint32_t i = 0; i < batch.rejected; ++i) {
        module.forwarder.forward(core::Role::kNone, {});
    }
    data::kRtcmStats.write(module.forwarder.stats());
    return false;
}
void rtcm_command(void *self, const ::app::Command &cmd, ::app::CommandResult &result) noexcept
{
    if (cmd.verb != verb(RtcmVerb::kReset) || cmd.arg_len != 0U) {
        result.status = ::app::Result::kInvalidArg;
        return;
    }
    // 固定绑定 RtcmModule；reset 不清除分流器半帧，只通知 bt_link 清统计。
    auto &module = *static_cast<RtcmModule *>(self);
    module.forwarder.reset();
    data::kRtcmStats.write(module.forwarder.stats());
    data::kRtcmReset.write(data::kRtcmReset.read() + 1U);
    result.status = ::app::Result::kOk;
}
} // namespace rtk::app
