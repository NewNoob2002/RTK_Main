// 调试模块实现（ADR-0009）：追踪环安装、唯一格式化输出任务、周期报告与持久化绑定。
// 线程：init() 启动前单线程；start() 执行器启动阶段；on_command 仅 kLink；输出任务 trace_out 独立运行。
// 均非 ISR；不分配堆。事件行与周期报告都经 cli 统一出口整行写出。

#include "modules/debug/debug_module.hpp"

#include <cstdio>

#include "app_context.hpp"
#include "cli/out.hpp"
#include "data/app_data.hpp"
#include "def/debug_def.hpp"
#include "fw/lock.hpp"
#include "modules/debug/debug_keys.hpp"
#include "modules/debug/trace_text.hpp"
#include "modules/rtcm/rtcm_forwarder.hpp"
#include "rtos/esp_status.hpp"
#include "rtos/spin_lock.hpp"
#include "spp_server.h"

namespace app {

namespace {
/* 按字节拼装/拆解整数时用的掩码与十进制基数，避免魔法数字。 */
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kTenthsPerUnit = 10U;

constexpr uint32_t kReportMinSeconds = 0U;
constexpr uint32_t kReportMaxSeconds = 3600U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint32_t kBytesPerUnit = 1024U;
/* 事件行 = "[ssss.mmm] " 前缀 + 正文 + "\r\n"；前缀最长 "[4294967.295] " 14 字节，取 24 留余量。 */
constexpr size_t kLinePrefixBytes = 24U;

/** 整行写控制台：经 cli 统一出口一次写出（ADR-0009 §2.8），与命令回复、ESP_LOG 共用一把锁，不会穿插。 */
void write_line(const char *text, size_t len) noexcept
{
    cli::out_write(text, len);
}

/** snprintf 结果截断到缓冲区内的实际长度（负值视为 0）。 */
size_t clamp_len(int written, size_t cap) noexcept
{
    if (written <= 0) {
        return 0U;
    }
    const auto len = static_cast<size_t>(written);
    return (len < cap) ? len : (cap - 1U);
}

// SPP 驱动的追踪码 → fw::trace(kSpp, …)。驱动只给数值，翻译与格式化推迟到输出任务。
void spp_trace_forward(spp_server_trace_code_t code, uint32_t handle, uint32_t a, uint32_t b) noexcept
{
    fw::trace(fw::TraceCat::kSpp, static_cast<uint8_t>(code), static_cast<uint16_t>(handle & core::kTraceSessionMask),
              a, b);
}

/** 小端读取 u16；buf 长度已由调用方按键宽校验。 */
uint16_t read_u16(std::span<const uint8_t> in) noexcept
{
    // 两字节索引都在 in.size() ≥ 2 的前提下，移位结果仍在 16 位内。
    return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8U));
}

/** 小端读取 u32。 */
uint32_t read_u32(std::span<const uint8_t> in) noexcept
{
    const uint32_t lo = static_cast<uint32_t>(read_u16(in.first(2U)));
    const uint32_t hi = static_cast<uint32_t>(read_u16(in.subspan(2U, 2U)));
    return (hi << 16U) | lo;
}

void write_u16(uint16_t value, std::span<uint8_t> out) noexcept
{
    out[0] = static_cast<uint8_t>(value & kByteMask);
    out[1] = static_cast<uint8_t>((value >> 8U) & kByteMask);
}

void write_u32(uint32_t value, std::span<uint8_t> out) noexcept
{
    for (size_t i = 0U; i < 4U; ++i) {
        // value 只有 4 字节，按位取字节不会越界。
        out[i] = static_cast<uint8_t>((value >> (8U * i)) & kByteMask);
    }
}

} // namespace

fw::Result DebugModule::init() noexcept
{
    // 幂等：app_main 在 storage.init() 之前先装环（要接住启动期的 kStore 事件），
    // 随后 Executor::init() 会按模块表再调一次；重复调用不再改动任何全局状态。
    if (installed_) {
        return fw::Result::kOk;
    }
    const fw::LockOps ops = rtos::spin_lock_ops(ring_mux_);
    const fw::Result ring_result = ring_.init(ring_storage_, ops);
    if (ring_result != fw::Result::kOk) {
        return ring_result;
    }
    const fw::Result install_result = fw::trace_install(&fw::TraceRing::sink, &ring_, &rtos::now_ms);
    if (install_result != fw::Result::kOk) {
        return install_result;
    }
    // storage 在启动早期就把 NVS 掩码写进全局原子量；这里不覆盖，避免丢掉已加载的设置。
    fw::trace_set_mask(fw::trace_mask());
    spp_server_set_trace_hook(&spp_trace_forward);
    installed_ = true;
    return fw::Result::kOk;
}

fw::Result DebugModule::start() noexcept
{
    if (task_.running()) {
        return fw::Result::kBusy;
    }
    // 最低优先级、控制台核：只在没有其它工作时占用 CPU。
    const fw::Status status = task_.start("trace_out", 1U, 0, &DebugModule::task_entry, this);
    if (status == fw::Status::kOk) {
        return fw::Result::kOk;
    }
    return (status == fw::Status::kNoSpace) ? fw::Result::kNoSpace : fw::Result::kInvalidArg;
}

void DebugModule::on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    switch (static_cast<DebugVerb>(cmd.verb)) {
    case DebugVerb::kReport: {
        if (cmd.arg_len != 2U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        const std::span<const uint8_t> args{cmd.args.data(), cmd.arg_len};
        const uint32_t seconds = read_u16(args);
        if ((seconds < kReportMinSeconds) || (seconds > kReportMaxSeconds)) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        // 窄化安全：seconds 已限 ≤ 3600。
        report_s_ = static_cast<uint16_t>(seconds);
        report_started_ = false;
        result.status = fw::Result::kOk;
        result.detail = static_cast<int32_t>(report_s_);
        return;
    }
    case DebugVerb::kSave: {
        if (cmd.arg_len != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        const fw::Result saved = context_.storage.save_keys(keys());
        result.status = saved;
        return;
    }
    default:
        result.status = fw::Result::kInvalidArg;
        return;
    }
}

std::span<const fw::StoreKey> DebugModule::keys() noexcept
{
    return {debug::kKeys};
}

fw::Result DebugModule::load_mask(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 4U) {
        return fw::Result::kInvalidArg;
    }
    const uint32_t mask = read_u32(in);
    // trace_set_mask 会清掉越界位，因此无需在此重复校验范围。
    fw::trace_set_mask(mask);
    return fw::Result::kOk;
}

fw::Result DebugModule::save_mask(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < 4U) {
        return fw::Result::kNoSpace;
    }
    write_u32(fw::trace_mask(), out);
    len = 4U;
    return fw::Result::kOk;
}

fw::Result DebugModule::load_report(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 2U) {
        return fw::Result::kInvalidArg;
    }
    const uint16_t seconds = read_u16(in);
    if (seconds > kReportMaxSeconds) {
        // 越界值视为损坏：保持关闭，调用方据此决定是否覆盖。
        return fw::Result::kInvalidArg;
    }
    app_context().debug.report_s_ = seconds;
    app_context().debug.report_started_ = false;
    return fw::Result::kOk;
}

fw::Result DebugModule::save_report(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < 2U) {
        return fw::Result::kNoSpace;
    }
    write_u16(app_context().debug.report_s_, out);
    len = 2U;
    return fw::Result::kOk;
}

void DebugModule::task_entry(void *arg) noexcept
{
    auto *self = static_cast<DebugModule *>(arg);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    self->run();
    vTaskDelete(nullptr);
}

void DebugModule::run() noexcept
{
    std::array<char, kLineBytes> line{};
    for (;;) {
        const bool printed = pump_events(line.data(), line.size());
        pump_report(rtos::now_ms());
        // 有事件时只让出一个 tick，尽快清空环；空闲时按轮询周期等待。
        vTaskDelay(printed ? 1U : pdMS_TO_TICKS(kPollPeriodMs));
    }
}

bool DebugModule::pump_events(char *line, size_t cap) noexcept
{
    std::array<char, kLineBytes + kLinePrefixBytes> out{};
    const uint32_t dropped = ring_.take_dropped();
    if (dropped > 0U) {
        const int n =
            std::snprintf(out.data(), out.size(), "[trace] dropped %lu\r\n", static_cast<unsigned long>(dropped));
        write_line(out.data(), clamp_len(n, out.size()));
    }
    bool printed = false;
    fw::TraceEvent event{};
    while (ring_.pop(event)) {
        debug::format_event(event, line, cap);
        const uint32_t seconds = event.t_ms / kMsPerSecond;
        const uint32_t millis = event.t_ms % kMsPerSecond;
        const int n = std::snprintf(out.data(), out.size(), "[%4lu.%03lu] %s\r\n", static_cast<unsigned long>(seconds),
                                    static_cast<unsigned long>(millis), line);
        write_line(out.data(), clamp_len(n, out.size()));
        printed = true;
    }
    return printed;
}

void DebugModule::pump_report(uint32_t now_ms) noexcept
{
    if (report_s_ == 0U) {
        report_started_ = false;
        return;
    }
    if (!report_started_) {
        last_report_ms_ = now_ms;
        report_started_ = true;
        return;
    }
    const uint32_t period_ms = static_cast<uint32_t>(report_s_) * kMsPerSecond;
    const uint32_t elapsed = now_ms - last_report_ms_;
    if (elapsed < period_ms) {
        return;
    }
    last_report_ms_ = now_ms;

    const RtcmStats rtcm = context_.data.rtcm_stats.read();
    const LinkStats stats = context_.data.link_stats.read();
    const SystemStatus system = context_.data.system_status.read();
    const rtos::ExecStats exec = context_.data.exec_stats.read();
    const fw::TraceRingStats ring_stats = ring_.stats();

    const uint32_t frames = rtcm.frames - last_rtcm_frames_;
    const uint32_t bytes = rtcm.bytes - last_rtcm_bytes_;
    const uint32_t dropped = rtcm.rejected_role - last_rtcm_drop_;
    const uint32_t prod = stats.frames_rx - last_prod_frames_;
    last_rtcm_frames_ = rtcm.frames;
    last_rtcm_bytes_ = rtcm.bytes;
    last_rtcm_drop_ = rtcm.rejected_role;
    last_prod_frames_ = stats.frames_rx;

    // 先换算成“每秒”，再拆成整数字节/十分之一 KB，避免浮点与 64 位开销。
    const uint32_t kbps10 = (bytes / (elapsed / kMsPerSecond + 1U)) * kTenthsPerUnit / kBytesPerUnit;
    const uint32_t fps = frames / (elapsed / kMsPerSecond + 1U);
    const uint32_t prod_fps = prod / (elapsed / kMsPerSecond + 1U);
    cli::out_printf("[report] rtcm %lu.%luKB/s %luf/s drop+%lu | prod %lu/s | heap %luK min %luK | "
           "stk link %lu.%luK tx %lu.%luK | trace drop %lu\n",
           static_cast<unsigned long>(kbps10 / kTenthsPerUnit), static_cast<unsigned long>(kbps10 % kTenthsPerUnit),
           static_cast<unsigned long>(fps), static_cast<unsigned long>(dropped), static_cast<unsigned long>(prod_fps),
           static_cast<unsigned long>(system.free_heap / kBytesPerUnit),
           static_cast<unsigned long>(system.min_free_heap / kBytesPerUnit),
           static_cast<unsigned long>((exec.stack_free * kTenthsPerUnit / kBytesPerUnit) / kTenthsPerUnit),
           static_cast<unsigned long>((exec.stack_free * kTenthsPerUnit / kBytesPerUnit) % kTenthsPerUnit),
           static_cast<unsigned long>((stats.tx_stack_free * kTenthsPerUnit / kBytesPerUnit) / kTenthsPerUnit),
           static_cast<unsigned long>((stats.tx_stack_free * kTenthsPerUnit / kBytesPerUnit) % kTenthsPerUnit),
           static_cast<unsigned long>(ring_stats.dropped));
}

} // namespace app
