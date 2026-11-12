// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "rtk/console/console.hpp"
namespace rtk::app {
struct AppContext;
class RtcmModule final {
public:
    constexpr explicit RtcmModule(AppContext &context) noexcept : context_(context) {}
    ~RtcmModule() = default;
    RtcmModule(const RtcmModule &) = delete;
    RtcmModule &operator=(const RtcmModule &) = delete;
    RtcmModule(RtcmModule &&) = delete;
    RtcmModule &operator=(RtcmModule &&) = delete;
    /** 仅 kLink，非 ISR、非阻塞；读取本轮批次，返回 false（无跨轮积压）。 */
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    /** 仅 kLink，非 ISR、非阻塞；校验并处理统计复位，状态写入 result。 */
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /** 仅 kLink 或启动前读取，非 ISR；返回当前转发统计的只读引用。 */
    [[nodiscard]] const RtcmStats &stats() const noexcept;

private:
    AppContext &context_;
    RtcmForwarder forwarder_{};
    uint32_t batch_version_ = 0;
};
[[nodiscard]] std::span<const rtk::console::Cmd> rtcm_cmds() noexcept;
} // namespace rtk::app
