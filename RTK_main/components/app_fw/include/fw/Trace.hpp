// 运行时调试追踪：类别位掩码 + 16 字节定长事件 + 多写者单读者定长环（ADR-0009 §2.1/§3）。
// 实时路径只调 trace()：查掩码、取时间、交给 sink，不格式化、不做 I/O；格式化与输出由读者任务完成。
// 禁止在 ISR 中调用；不分配堆，跨任务互斥用注入的 LockOps，纯 C++20、不依赖 IDF/FreeRTOS。
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

/**
 * @brief  把类别枚举换算成掩码位。
 * @param  cat: 追踪类别。
 * @retval 1U << cat 的单个位（调用方保证 cat < TraceCat::kCount）
 * @note   constexpr、无状态，任意上下文可用。
 */
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
 * @brief  安装 sink 与时钟，供 trace() 使用。
 * @param  fn: 事件存储回调（通常是 TraceRing::push 的适配函数）。
 * @param  ctx: 原样传给 fn 的上下文指针。
 * @param  clock: 毫秒时钟回调，在调用 trace() 的任务上下文中执行，须非阻塞。
 * @retval Result::kOk 安装成功
 * @retval Result::kInvalidArg fn 或 clock 为 nullptr
 * @retval Result::kInvalidState 已经安装过
 * @note   仅启动阶段单线程调用一次，非 ISR；未安装时 trace() 什么也不做。
 */
[[nodiscard]] Result traceInstall(TraceSinkFn fn, void *ctx, TraceClockFn clock) noexcept;

/**
 * @brief  设置类别掩码。
 * @param  mask: 位图，bit n 对应 TraceCat n；超出 kTraceAllMask 的位被忽略。
 * @note   原子写，任意任务可调用，不阻塞。
 */
void traceSetMask(uint32_t mask) noexcept;
/**
 * @brief  读取类别掩码。
 * @retval 当前掩码（恒 ⊆ kTraceAllMask）
 * @note   原子读，任意任务可调用，不阻塞。
 */
[[nodiscard]] uint32_t traceMask() noexcept;

#if FW_TRACE_ENABLED
/**
 * @brief  判断类别是否打开。
 * @param  cat: 追踪类别。
 * @retval true  掩码对应位置位
 * @retval false 类别关闭（总开关关闭时恒为 false）
 * @note   原子 relaxed 读，任意任务可调用；调用方可先判断再准备昂贵参数。
 */
[[nodiscard]] bool traceOn(TraceCat cat) noexcept;

/**
 * @brief  记录一条 16 字节定长事件。
 * @param  cat: 追踪类别，决定掩码位。
 * @param  code: 事件码，含义由 main 侧的文本表约定。
 * @param  session: 会话号，无关时填 0。
 * @param  a: 参数 a，含义由 (cat, code) 决定。
 * @param  b: 参数 b，含义由 (cat, code) 决定。
 * @note   实时路径调用：非阻塞、不格式化、不做 I/O；类别关闭或未安装 sink 时立即返回；不可在 ISR 中调用。
 */
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

    /**
     * @brief  初始化事件环，绑定调用方存储与锁。
     * @param  storage: 事件存储，容量须 ≥ 2 且 ≤ UINT32_MAX。
     * @param  lock: 记录锁；lock/unlock 须同为 nullptr 或同为非 nullptr。
     * @retval Result::kOk 初始化成功
     * @retval Result::kInvalidArg 容量小于 2、超过 UINT32_MAX，或锁表不成对
     * @retval Result::kInvalidState 已经初始化过
     * @note   仅启动前单线程调用，非 ISR；持注入锁，不阻塞、不分配堆。
     */
    [[nodiscard]] Result init(std::span<TraceEvent> storage, const LockOps &lock) noexcept;

    /**
     * @brief  压入一条事件。
     * @param  ev: 待记录的事件。
     * @retval true  写入成功
     * @retval false 未初始化或环已满（满时 dropped+1）
     * @note   多写者任务并发调用，持注入锁，非 ISR；绝不阻塞、绝不覆盖未读事件。
     */
    bool push(const TraceEvent &ev) noexcept;

    /**
     * @brief  弹出最旧事件。
     * @param[out] out: 接收事件；失败时不修改。
     * @retval true  取到事件
     * @retval false 环为空或未初始化
     * @note   单读者任务调用，持注入锁，非 ISR。
     */
    [[nodiscard]] bool pop(TraceEvent &out) noexcept;

    /**
     * @brief  读最旧事件的时间戳。
     * @param[out] tMs: 最旧事件的毫秒时间戳；失败时不修改。
     * @retval true  取到时间戳
     * @retval false 环为空或未初始化
     * @note   单读者任务调用，持注入锁，非 ISR；读者据此与 LogRing 按时间合并（ADR-0009 §2.11）。
     */
    [[nodiscard]] bool peekTime(uint32_t &tMs) const noexcept;

    /**
     * @brief  取走自上次调用以来新增的丢弃计数。
     * @retval 新增丢弃的事件数；返回后计数清零
     * @note   单读者任务调用，持注入锁，非 ISR；读者用它打印 "[trace] dropped N"。
     */
    [[nodiscard]] uint32_t takeDropped() noexcept;

    /**
     * @brief  读取累计统计。
     * @retval 环的统计快照
     * @note   任意任务调用，持注入锁，非 ISR。
     */
    [[nodiscard]] TraceRingStats stats() const noexcept;
    /**
     * @brief  读取当前事件数。
     * @retval 环内待读事件数；未初始化时为 0
     * @note   任意任务调用，持注入锁，非 ISR。
     */
    [[nodiscard]] size_t depth() const noexcept;

    /**
     * @brief  把 TraceSinkFn 调用转成 push。
     * @param  ctx: 必须是 TraceRing*。
     * @param  ev: 待记录的事件。
     * @note   可由多个写者任务并发调用（内部持锁），非 ISR；不阻塞、不分配堆。
     */
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
