// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "rtk/console/console.hpp"
namespace rtk::app {
struct RtcmModule {
    RtcmForwarder forwarder{};
    uint32_t batch_version = 0;
};
extern RtcmModule s_rtcm_module;
// 仅 kLink 调用，非 ISR；读取 bt_link 已发布且本轮保持有效的批次 Slot。
[[nodiscard]] bool rtcm_poll(void *self, uint32_t now_ms) noexcept;
void rtcm_command(void *self, const ::app::Command &cmd, ::app::CommandResult &result) noexcept;
[[nodiscard]] std::span<const rtk::console::Cmd> rtcm_cmds() noexcept;
inline constexpr ::app::ModuleOps kRtcmOps{nullptr, nullptr, rtcm_poll, rtcm_command};
} // namespace rtk::app
