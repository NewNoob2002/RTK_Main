// 调试模块实现（ADR-0009）：追踪环安装、唯一格式化输出任务、周期报告与持久化绑定。
// 线程：init() 启动前单线程；start() 执行器启动阶段；onCommand 仅 kLink；输出任务 trace_out 独立运行。
// 均非 ISR；不分配堆。事件行与周期报告都经 cli 统一出口整行写出。

#include "DebugLine.hpp"
#include "modules/debug/DebugModule.hpp"

#include <algorithm>
#include <cstdio>

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "data/AppData.hpp"
#include "def/DebugDef.hpp"
#include "fw/Lock.hpp"
#include "fw/Log.hpp"
#include "modules/debug/DebugKeys.hpp"
#include "modules/debug/TraceText.hpp"
#include "modules/rtcm/RtcmForwarder.hpp"
#include "rtos/EspStatus.hpp"
#include "rtos/SpinLock.hpp"
#include "SppServer.h"

namespace app {

namespace {

using detail::clampLen;
using detail::writeLine;
/* 按字节拼装/拆解整数时用的掩码与十进制基数，避免魔法数字。 */
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kTenthsPerUnit = 10U;

constexpr uint32_t kReportMinSeconds = 0U;
constexpr uint32_t kReportMaxSeconds = 3600U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint32_t kBytesPerUnit = 1024U;
/**
 * @brief 把 C 驱动的追踪字段转发给统一追踪环
 * @param code: 驱动追踪码
 * @param handle: 句柄，保留低位作为追踪会话号
 * @param a: 事件第一个数值字段
 * @param b: 事件第二个数值字段
 * @note SPP 任务回调；不分配、不阻塞、不格式化，非 ISR。
 * @note SPP 驱动的追踪码 → fw::trace(kSpp, …)。驱动只给数值，翻译与格式化推迟到输出任务。
 */
void sppTraceForward(spp_server_trace_code_t code, uint32_t handle, uint32_t a, uint32_t b) noexcept
{
    fw::trace(fw::TraceCat::kSpp, static_cast<uint8_t>(code),
              static_cast<uint16_t>(handle & core::kTraceSessionMask), a, b);
}

/**
 * @brief 读取已校验长度的两字节小端整数
 * @param in: 输入字节视图
 * @retval 小端 u16 数值
 * @note 纯函数，线程/ISR 安全；调用方保证至少 2 字节。
 * @note 小端读取 u16；buf 长度已由调用方按键宽校验。
 */
uint16_t readU16(std::span<const uint8_t> in) noexcept
{
    // 两字节索引都在 in.size() ≥ 2 的前提下，移位结果仍在 16 位内。
    return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8U));
}

uint32_t readU32(std::span<const uint8_t> in) noexcept
{
    const uint32_t lo = static_cast<uint32_t>(readU16(in.first(2U)));
    const uint32_t hi = static_cast<uint32_t>(readU16(in.subspan(2U, 2U)));
    return (hi << 16U) | lo;
}

/**
 * @brief 按小端写出两字节整数
 * @param value: 待转换或写入的值
 * @param out: 输出缓冲或结果
 * @note 纯函数，线程/ISR 安全；调用方保证至少 2 字节。
 */
void writeU16(uint16_t value, std::span<uint8_t> out) noexcept
{
    out[0] = static_cast<uint8_t>(value & kByteMask);
    out[1] = static_cast<uint8_t>((value >> 8U) & kByteMask);
}

void writeU32(uint32_t value, std::span<uint8_t> out) noexcept
{
    for (size_t i = 0U; i < 4U; ++i) {
        // value 只有 4 字节，按位取字节不会越界。
        out[i] = static_cast<uint8_t>((value >> (8U * i)) & kByteMask);
    }
}

} // namespace

fw::Status DebugModule::bindStorage(const StorageView &storage) noexcept
{
    if (installed_ || task_.running()) {
        return fw::Status::kInvalidState;
    }
    if ((storage.traceRing.data() == nullptr) || (storage.traceRing.size() < kRingEvents) ||
        (storage.logRing.data() == nullptr) || (storage.logRing.size() < kLogRingBytes) ||
        (storage.taskStack.data() == nullptr) ||
        ((storage.taskStack.size() * sizeof(StackType_t)) < kTaskStackBytes)) {
        return fw::Status::kInvalidArg;
    }
    storage_ = {
        .traceRing = storage.traceRing.first(kRingEvents),
        .logRing = storage.logRing.first(kLogRingBytes),
        .taskStack = storage.taskStack.first(kTaskStackBytes / sizeof(StackType_t)),
    };
    storage_bound_ = true;
    return fw::Status::kOk;
}

/**
 * @brief  安装 trace 环与 spp 钩子，沿用早期配置的诊断设置
 * @retval fw::Result::kOk 成功；kInvalidState 存储未绑定
 */
fw::Result DebugModule::init() noexcept
{
    // 幂等：app_main 在 storage.init() 之前先装环（要接住启动期的 kStore 事件），
    // 随后 Executor::init() 会按模块表再调一次；重复调用不再改动任何全局状态。
    if (installed_) {
        return fw::Result::kOk;
    }
    if (!storage_bound_) {
        return fw::Result::kInvalidState;
    }
    portMUX_INITIALIZE(&ring_mux_);
    const fw::LockOps ops = rtos::spinLockOps(ring_mux_);
    const fw::Result ringResult = ring_.init(storage_.traceRing, ops);
    if (ringResult != fw::Result::kOk) {
        return ringResult;
    }
    const fw::Result installResult = fw::traceInstall(&fw::TraceRing::sink, &ring_, &rtos::nowMs);
    if (installResult != fw::Result::kOk) {
        return installResult;
    }
    const fw::Result logResult = log_ring_.init(storage_.logRing, ops);
    if (logResult != fw::Result::kOk) {
        return logResult;
    }
    // 文本环此处只初始化；启动结束时 attachLog() 才安装（ADR-0009 §2.12），启动期日志走直写。
    spp_server_set_trace_hook(&sppTraceForward);
    installed_ = true;
    return fw::Result::kOk;
}

fw::Result DebugModule::attachLog() noexcept
{
    if (log_attached_) {
        return fw::Result::kOk;
    }
    if (!installed_) {
        return fw::Result::kInvalidState;
    }
    const fw::Result result = fw::logInstall(&fw::LogRing::sink, &log_ring_);
    if (result != fw::Result::kOk) {
        return result;
    }
    log_attached_ = true;
    return fw::Result::kOk;
}

fw::Result DebugModule::start() noexcept
{
    if (!installed_ || !storage_bound_) {
        return fw::Result::kInvalidState;
    }
    if (task_.running()) {
        return fw::Result::kBusy;
    }
    // 最低优先级、控制台核：只在没有其它工作时占用 CPU。
    const fw::Status status =
        task_.start(storage_.taskStack, "trace_out", 1U, 0, &DebugModule::taskEntry, this);
    if (status == fw::Status::kOk) {
        return fw::Result::kOk;
    }
    return (status == fw::Status::kNoSpace) ? fw::Result::kNoSpace : fw::Result::kInvalidArg;
}

void DebugModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    switch (static_cast<DebugVerb>(cmd.verb)) {
    case DebugVerb::kReport: {
        if (cmd.argLen != 2U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        const std::span<const uint8_t> args{cmd.args.data(), cmd.argLen};
        const uint32_t seconds = readU16(args);
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
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        const fw::Result saved = ctx().storage.saveKeys(keys());
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

fw::Result DebugModule::loadMask(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 4U) {
        return fw::Result::kInvalidArg;
    }
    const uint32_t mask = readU32(in);
    // traceSetMask 会清掉越界位，因此无需在此重复校验范围。
    fw::traceSetMask(mask);
    return fw::Result::kOk;
}

fw::Result DebugModule::saveMask(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < 4U) {
        return fw::Result::kNoSpace;
    }
    writeU32(fw::traceMask(), out);
    len = 4U;
    return fw::Result::kOk;
}

fw::Result DebugModule::loadReport(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 2U) {
        return fw::Result::kInvalidArg;
    }
    const uint16_t seconds = readU16(in);
    if (seconds > kReportMaxSeconds) {
        // 越界值视为损坏：保持关闭，调用方据此决定是否覆盖。
        return fw::Result::kInvalidArg;
    }
#if FW_TRACE_ENABLED
    appContext().debug.applyBootReport(seconds);
#endif
    return fw::Result::kOk;
}

fw::Result DebugModule::saveReport(const void * /*obj*/, std::span<uint8_t> out,
                                   size_t &len) noexcept
{
    if (out.size() < 2U) {
        return fw::Result::kNoSpace;
    }
#if FW_TRACE_ENABLED
    writeU16(appContext().debug.report_s_, out);
#else
    writeU16(0U, out);
#endif
    len = 2U;
    return fw::Result::kOk;
}

fw::Result DebugModule::loadHex(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 4U) {
        return fw::Result::kInvalidArg;
    }
    // log_set_hex_mask 会清掉越界位，无需在此重复校验范围。
    fw::logSetHexMask(readU32(in));
    return fw::Result::kOk;
}

fw::Result DebugModule::saveHex(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < 4U) {
        return fw::Result::kNoSpace;
    }
    writeU32(fw::logHexMask(), out);
    len = 4U;
    return fw::Result::kOk;
}

fw::Result DebugModule::loadLoglv(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 4U) {
        return fw::Result::kInvalidArg;
    }
    // log_set_levels 按编译期上限截断每个字段并忽略未用高位。
    fw::logSetLevels(readU32(in));
    return fw::Result::kOk;
}

fw::Result DebugModule::saveLoglv(const void * /*obj*/, std::span<uint8_t> out,
                                  size_t &len) noexcept
{
    if (out.size() < 4U) {
        return fw::Result::kNoSpace;
    }
    writeU32(fw::logLevels(), out);
    len = 4U;
    return fw::Result::kOk;
}

void DebugModule::taskEntry(void *arg) noexcept
{
    auto *self = static_cast<DebugModule *>(arg);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    if (!self->ctx().awaitBoot(BootTask::kTrace, false)) {
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
        const bool printed = pumpEvents(line.data(), line.size());
        pumpReport(rtos::nowMs());
        // 有事件时只让出一个 tick，尽快清空环；空闲时按轮询周期等待。
        vTaskDelay(printed ? 1U : pdMS_TO_TICKS(kPollPeriodMs));
    }
}

bool DebugModule::pumpDrops(char *out, size_t cap) noexcept
{
    bool printed = false;
    const uint32_t traceDropped = ring_.takeDropped();
    if (traceDropped > 0U) {
        const int n = std::snprintf(out, cap, "[trace] %lu events dropped\r\n",
                                    static_cast<unsigned long>(traceDropped));
        writeLine(out, clampLen(n, cap));
        printed = true;
    }
    const uint32_t logDropped = log_ring_.takeDropped();
    if (logDropped > 0U) {
        const int n = std::snprintf(out, cap, "[log] %lu lines dropped\r\n",
                                    static_cast<unsigned long>(logDropped));
        writeLine(out, clampLen(n, cap));
        printed = true;
    }
    return printed;
}

void DebugModule::pumpReport(uint32_t nowMs) noexcept
{
    if (report_s_ == 0U) {
        report_started_ = false;
        return;
    }
    if (!report_started_) {
        last_report_ms_ = nowMs;
        report_started_ = true;
        return;
    }
    const uint32_t periodMs = static_cast<uint32_t>(report_s_) * kMsPerSecond;
    const uint32_t elapsed = nowMs - last_report_ms_;
    if (elapsed < periodMs) {
        return;
    }
    last_report_ms_ = nowMs;

    const RtcmStats rtcm = ctx().data.rtcmStats.read();
    const LinkStats stats = ctx().data.linkStats.read();
    const SystemStatus system = ctx().data.systemStatus.read();
    const rtos::ExecStats exec = ctx().data.execStats.read();
    const fw::TraceRingStats ringStats = ring_.stats();

    const uint32_t frames = rtcm.frames - last_rtcm_frames_;
    const uint32_t bytes = rtcm.bytes - last_rtcm_bytes_;
    const uint32_t dropped = rtcm.rejectedRole - last_rtcm_drop_;
    const uint32_t prod = stats.framesRx - last_prod_frames_;
    last_rtcm_frames_ = rtcm.frames;
    last_rtcm_bytes_ = rtcm.bytes;
    last_rtcm_drop_ = rtcm.rejectedRole;
    last_prod_frames_ = stats.framesRx;

    // 先换算成“每秒”，再拆成整数字节/十分之一 KB，避免浮点与 64 位开销。
    const uint32_t kbps10 =
        (bytes / (elapsed / kMsPerSecond + 1U)) * kTenthsPerUnit / kBytesPerUnit;
    const uint32_t fps = frames / (elapsed / kMsPerSecond + 1U);
    const uint32_t prodFps = prod / (elapsed / kMsPerSecond + 1U);
    cli::outPrintf("[report] rtcm %lu.%luKB/s %luf/s drop+%lu | prod %lu/s | heap %luK min %luK | "
                   "stk link %lu.%luK | tx accepted %luB completed %luB | trace drop %lu\n",
                   static_cast<unsigned long>(kbps10 / kTenthsPerUnit),
                   static_cast<unsigned long>(kbps10 % kTenthsPerUnit),
                   static_cast<unsigned long>(fps), static_cast<unsigned long>(dropped),
                   static_cast<unsigned long>(prodFps),
                   static_cast<unsigned long>(system.freeHeap / kBytesPerUnit),
                   static_cast<unsigned long>(system.minFreeHeap / kBytesPerUnit),
                   static_cast<unsigned long>((exec.stackFree * kTenthsPerUnit / kBytesPerUnit) /
                                              kTenthsPerUnit),
                   static_cast<unsigned long>((exec.stackFree * kTenthsPerUnit / kBytesPerUnit) %
                                              kTenthsPerUnit),
                   static_cast<unsigned long>(stats.txAccepted), // uint32_t→unsigned long 不窄化
                   static_cast<unsigned long>(stats.txCompleted),
                   static_cast<unsigned long>(ringStats.dropped));
}

} // namespace app
