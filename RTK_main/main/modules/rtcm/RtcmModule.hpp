// RTCM 模块（kLink 执行器）：累计分流器交付的 RTCM 帧统计，处理 `rtcm` 命令的查询与复位。
// 由 kLink 执行器任务调用；控制台经 rtos::Executor::call 投递命令，非 ISR 安全。
#pragma once
#include <span>

#include "data/AppData.hpp"
#include "cli/Console.hpp"
namespace app {
struct AppContext;
/** RTCM 模块（kLink 执行器）：消费批次视图并处理 rtcm 命令。 */
class RtcmModule final {
public:
    constexpr RtcmModule() noexcept = default;
    constexpr explicit RtcmModule(AppContext &context) noexcept : context_(&context) {}
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~RtcmModule() = default;
    RtcmModule(const RtcmModule &) = delete;
    RtcmModule &operator=(const RtcmModule &) = delete;
    RtcmModule(RtcmModule &&) = delete;
    RtcmModule &operator=(RtcmModule &&) = delete;
    /** 仅 kLink，非 ISR、非阻塞；读取本轮批次，返回 false（无跨轮积压）。 */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;
    /** 仅 kLink，非 ISR、非阻塞；校验并处理统计复位，状态写入 result。 */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /** 仅 kLink 或启动前读取，非 ISR；返回当前转发统计的只读引用。 */
    [[nodiscard]] const RtcmStats &stats() const noexcept;

private:
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    AppContext *context_ = nullptr;
    RtcmStats stats_{};
    RtcmLinkSnapshot baseline_{};
    uint32_t reset_epoch_ = 0;
    uint32_t rate_window_ms_ = 0;
    uint32_t rate_window_bytes_ = 0;
    bool rate_started_ = false;
};
/** 打印 [rtcm] 分节及 ADR-0006 兼容的 rtcm/stats 两行；仅 console REPL 任务读取 DataCenter。 */
void printRtcmStatus() noexcept;
/** RTCM 模块的控制台命令表；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> rtcmCmds() noexcept;
/** rtcm 子命令动词表，供 cli::addVerbs 登记。 */
[[nodiscard]] std::span<const cli::Verb> rtcmVerbs() noexcept;
} // namespace app
