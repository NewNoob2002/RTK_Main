// RTCM 模块（kLink 执行器）：累计分流器交付的 RTCM 帧统计，处理 `rtcm` 命令的查询与复位。
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "cli/console.hpp"
namespace app {
struct AppContext;
/** RTCM 模块（kLink 执行器）：消费批次视图并处理 rtcm 命令。 */
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
/** RTCM 模块的控制台命令表；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> rtcm_cmds() noexcept;
} // namespace app
