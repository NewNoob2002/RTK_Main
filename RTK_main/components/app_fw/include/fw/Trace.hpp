// 运行时调试追踪：类别位掩码 + 16 字节定长事件 + 多写者单读者定长环（ADR-0009 §2.1）。
// 实时路径（link 任务、发送任务、BT 回调任务）只调用 trace()：检查掩码、取时间、交给 sink，不格式化、不做 I/O。
// 格式化与输出由读者任务（main 的 DebugModule）完成。禁止在 ISR 中调用。
// 纯 C++20，不依赖 IDF/FreeRTOS；跨任务互斥通过注入的 LockOps。
// 冻结接口（ADR-0009 §3）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Lock.hpp"
#include "fw/Result.hpp"

// 编译期总开关（ADR-0009 §2.6）：0 时 trace()/traceOn() 变为空内联，调用点整体被优化掉，
// 调用方无需 #if。ESP-IDF 构建由 app_fw 的 CMake 依据 CONFIG_RTK_TRACE_ENABLE 以 PUBLIC 宏传给所有依赖方；
// 其他构建（host 测试、外部工程）未定义时默认开启。
#ifndef FW_TRACE_ENABLED
// 必须是宏：CMake 以 -D 传入，且 Tlog.hpp 等需在预处理阶段据此选择内联空实现。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FW_TRACE_ENABLED 1
#endif

namespace fw {

/** 追踪类别；数值即掩码位号。新增类别只能追加在 kCount 之前，并同步 main 的名称/文本表。 */
enum class TraceCat : uint8_t {
    kLink = 0, // 会话打开/关闭、角色判定、超时
    kSpp,      // SPP 驱动：拥塞、写完成/失败、RX 溢出
    kRtcm,     // RTCM 成帧、拒绝、重同步
    kProto,    // 产品帧、应答、解析错误
    kAlarm,    // 报警状态与推送
    kStore,    // NVS 读写、去抖、重试
    kExec,     // 执行器轮次超时、投递失败
    kGnss,     // GNSS 状态机、探测/配置命令、管道丢帧（ADR-0010 §2.7）
    kSys,      // 启动、控制台、系统服务（ADR-0009 §2.12，主要承载日志）
    kCount,
};

inline constexpr uint32_t kTraceAllMask = (1U << static_cast<uint32_t>(TraceCat::kCount)) - 1U;

[[nodiscard]] constexpr uint32_t traceBit(TraceCat cat) noexcept
{
    return 1U << static_cast<uint32_t>(cat);
}

/** 一条追踪事件；各字段含义由 (cat, code) 决定，文本表在 main。session 无关时填 0。 */
struct TraceEvent {
    uint32_t tMs = 0; // 事件时间（ms，来自安装时注入的时钟）
    TraceCat cat = TraceCat::kLink;
    uint8_t code = 0;     // 类别内事件码
    uint16_t session = 0; // 会话句柄低 16 位
    uint32_t a = 0;       // 参数 1
    uint32_t b = 0;       // 参数 2
};
static_assert(sizeof(TraceEvent) == 16U, "TraceEvent must stay 16 bytes");

/** sink：把事件交给存储（通常是 TraceRing::push）；必须非阻塞、可被多个任务并发调用（自行加锁）。 */
using TraceSinkFn = void (*)(void *ctx, const TraceEvent &ev) noexcept;
using TraceClockFn = uint32_t (*)() noexcept;

/**
 * 安装 sink 与时钟（启动阶段、单线程调用一次）。fn 或 clock 为 nullptr → kInvalidArg；
 * 重复安装 → kInvalidState。未安装时 trace() 什么也不做。
 */
[[nodiscard]] Result traceInstall(TraceSinkFn fn, void *ctx, TraceClockFn clock) noexcept;

/** 设置/读取类别掩码（原子，任意任务可调用）；超出 kTraceAllMask 的位被忽略。 */
void traceSetMask(uint32_t mask) noexcept;
[[nodiscard]] uint32_t traceMask() noexcept;

#if FW_TRACE_ENABLED
/** 该类别是否打开（原子 relaxed 读）；调用方可先判断再准备昂贵的参数。 */
[[nodiscard]] bool traceOn(TraceCat cat) noexcept;

/** 记录一条事件：类别关闭或未安装 sink 时立即返回。非阻塞，不可在 ISR 中调用。 */
void trace(TraceCat cat, uint8_t code, uint16_t session, uint32_t a, uint32_t b) noexcept;
#else
// 关闭时恒为 false / 空操作；参数仍做类型检查，调用点不必改动。
[[nodiscard]] inline bool traceOn(TraceCat /*cat*/) noexcept
{
    return false;
}

inline void trace(TraceCat /*cat*/, uint8_t /*code*/, uint16_t /*session*/, uint32_t /*a*/,
                  uint32_t /*b*/) noexcept
{}
#endif

/** TraceRing 统计；init() 后清零。 */
struct TraceRingStats {
    uint32_t pushed = 0;   // 成功压入
    uint32_t dropped = 0;  // 环满丢弃（累计）
    uint32_t maxDepth = 0; // 历史最大占用
};

/**
 * 定长事件环：多写者（push，加锁）单读者（pop，加锁）；满了丢新事件并计数，绝不阻塞。
 * 存储由调用方提供，容量 = storage.size()（≥ 2），不分配堆。不可拷贝/移动。
 */
class TraceRing {
public:
    constexpr TraceRing() noexcept = default;
    TraceRing(const TraceRing &) = delete;
    TraceRing &operator=(const TraceRing &) = delete;
    TraceRing(TraceRing &&) = delete;
    TraceRing &operator=(TraceRing &&) = delete;
    ~TraceRing() = default;

    /** 启动前调用。storage.size() < 2 → kInvalidArg；重复 init → kInvalidState。lock 被拷贝保存。 */
    [[nodiscard]] Result init(std::span<TraceEvent> storage, const LockOps &lock) noexcept;

    /** 压入；未 init → false；满 → false 并 dropped+1。 */
    bool push(const TraceEvent &ev) noexcept;

    /** 弹出最旧事件；空或未 init → false。 */
    [[nodiscard]] bool pop(TraceEvent &out) noexcept;

    /** 最旧事件的时间戳；空或未 init → false。读者据此与 LogRing 按时间合并（ADR-0009 §2.11）。 */
    [[nodiscard]] bool peekTime(uint32_t &tMs) const noexcept;

    /** 取走自上次调用以来新增的丢弃数（读者用来打印 "[trace] dropped N"）。 */
    [[nodiscard]] uint32_t takeDropped() noexcept;

    [[nodiscard]] TraceRingStats stats() const noexcept;
    [[nodiscard]] size_t depth() const noexcept;

    /** 适配 TraceSinkFn：ctx 必须是 TraceRing*。 */
    static void sink(void *ctx, const TraceEvent &ev) noexcept;

private:
    std::span<TraceEvent> buf_{};
    LockOps lock_{};
    size_t head_ = 0; // 下一个写入位置
    size_t count_ = 0;
    uint32_t dropped_reported_ = 0;
    TraceRingStats stats_{};
    bool ready_ = false;
};

} // namespace fw
