// 调试模块（kLink 执行器）：追踪环、唯一格式化输出任务与 debug 命令组的状态所有者（ADR-0009）。
// init() 只在启动前单线程调用；on_command 只在 kLink 执行器任务调用；trace_out 任务独立运行。
// 均非 ISR；不分配堆（环与任务栈都是静态存储）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "cli/console.hpp"
#include "fw/command.hpp"
#include "fw/store_key.hpp"
#include "fw/trace.hpp"
#include "rtos/static_task.hpp"

namespace app {
struct AppContext;

/**
 * 调试模块：拥有 TraceRing 与输出任务。
 * 事件由各任务通过 fw::trace() 写入环，本模块的输出任务是唯一的格式化者。
 */
class DebugModule final {
public:
    /** 环容量：256 条 × 16 字节 = 4 KB 静态存储，够覆盖 20 ms 输出间隔的突发。 */
    static constexpr size_t kRingEvents = 256;
    /** 输出任务轮询周期；有事件时立即输出，不必等满一个周期。 */
    static constexpr uint32_t kPollPeriodMs = 20;
    /** 单行事件文本上限，超长由 format_event 截断。 */
    static constexpr size_t kLineBytes = 192;
    /** 输出任务栈；T3 上板 7 类全开、40 KB/s 负载下实测占用约 1.7 KB（HWM 2412/4096），取 3 KB 留约 1.3 KB 余量。 */
    static constexpr size_t kTaskStackBytes = 3072;

    constexpr explicit DebugModule(AppContext &context) noexcept : context_(context) {}
    ~DebugModule() = default;
    DebugModule(const DebugModule &) = delete;
    DebugModule &operator=(const DebugModule &) = delete;
    DebugModule(DebugModule &&) = delete;
    DebugModule &operator=(DebugModule &&) = delete;

    /** 启动前单线程：初始化环并安装 sink 与 SPP 钩子。幂等，重复调用返回 kOk。 */
    [[nodiscard]] fw::Result init() noexcept;

    /** 创建静态输出任务；重复调用返回 kBusy。仅执行器启动阶段调用。 */
    [[nodiscard]] fw::Result start() noexcept;

    /** 仅 kLink：debug report/save 动词。 */
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    /** 本模块登记的持久化键（ADR-0009 §2.4），由 storage 模块在启动时加载。 */
    [[nodiscard]] static std::span<const fw::StoreKey> keys() noexcept;

    // 以下四个适配 fw::StoreBinding：obj 由 storage 传入，但表是 constexpr、无法保存实例地址，
    // 因此统一经 app_context() 定位唯一实例（模块是单例）。
    static fw::Result load_mask(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_mask(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    static fw::Result load_report(void *obj, std::span<const uint8_t> in) noexcept;
    static fw::Result save_report(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;

    /** 报告周期（秒，0 = 关闭）。跨任务读，非 ISR。 */
    [[nodiscard]] uint16_t report_period_s() const noexcept
    {
        // 32 位对齐的单字读取在 ESP32 上是原子的；仅用于人为读取的控制台输出。
        return report_s_;
    }

    /** 环统计快照（console 任务读取，内部已加锁，非 ISR）。 */
    [[nodiscard]] fw::TraceRingStats ring_stats() const noexcept
    {
        return ring_.stats();
    }

    /** 环中待输出事件数（console 任务读取，内部已加锁，非 ISR）。 */
    [[nodiscard]] size_t ring_depth() const noexcept
    {
        return ring_.depth();
    }

private:
    static void task_entry(void *arg) noexcept;
    /** 输出任务主循环：取事件、打印丢弃计数、按周期报告。 */
    void run() noexcept;
    /** 一次性取空环，非空返回 true 表示本轮打印过事件。 */
    bool pump_events(char *line, size_t cap) noexcept;
    /** 到点输出一行周期报告。 */
    void pump_report(uint32_t now_ms) noexcept;

    AppContext &context_;
    fw::TraceRing ring_{};
    std::array<fw::TraceEvent, kRingEvents> ring_storage_{};
    portMUX_TYPE ring_mux_ = portMUX_INITIALIZER_UNLOCKED;
    rtos::StaticTask<kTaskStackBytes> task_{};
    bool installed_ = false;
    uint16_t report_s_ = 0;
    uint32_t last_report_ms_ = 0;
    bool report_started_ = false;
    // 周期报告的增量基线（只在输出任务里读写）。
    uint32_t last_rtcm_bytes_ = 0;
    uint32_t last_rtcm_frames_ = 0;
    uint32_t last_rtcm_drop_ = 0;
    uint32_t last_prod_frames_ = 0;
};

/** debug 命令表。 */
[[nodiscard]] std::span<const cli::Cmd> debug_cmds() noexcept;

/** debug 子命令动词表，供 cli::add_verbs 登记。 */
[[nodiscard]] std::span<const cli::Verb> debug_verbs() noexcept;

} // namespace app
