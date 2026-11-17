// 调试文本日志通道（ADR-0009 §2.11）：调用方在自己栈上格式化一行文本，整条压入变长记录环 LogRing，由读者任务与 TraceRing 合并输出。
// 面向非实时任务（io 执行器、控制台），禁止在 ISR 中调用；不分配堆，纯 C++20、不依赖 IDF/FreeRTOS。
// 过滤 = 类别日志级别（fw/Log.hpp）+ 十六进制掩码（logSetHexMask）；新代码用 FW_LOG* 宏，tlog() 只留给已有调用点。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Lock.hpp"
#include "fw/Result.hpp"
#include "fw/Trace.hpp"

namespace fw {

/** 一条文本记录的最大字节数（不含结尾 0）；超出部分截断并以 "..." 结尾。调用方栈上需要同样大小的缓冲。 */
inline constexpr std::size_t kLogTextMax = 160U;
/** 一条十六进制记录最多保留的原始字节数；超出部分只记总长，由读者打印 "(+N bytes not shown)"。 */
inline constexpr std::size_t kLogHexMax = 256U;

enum class LogKind : uint8_t {
    kText = 0, // payload = 文本（无结尾 0、无换行）
    kHex = 1,  // payload = 原始字节（≤ kLogHexMax），total = 原始长度
};

/** 变长记录头；payload 紧随其后存放在环中。 */
/**
 * 日志级别（ADR-0009 §2.12）；数值越小越严重。kError/kWarn 不受类别掩码限制，总是记录；
 * kInfo/kDebug 只在类别打开且不高于运行期级别（log_set_level）时记录。
 */
enum class LogLevel : uint8_t {
    kError = 0,
    kWarn = 1,
    kInfo = 2,
    kDebug = 3,
};

struct LogHeader {
    uint32_t tMs =
        0; // 记录时间（ms，环通路用 traceInstall 的时钟，直写通路用 logInstallDirect 的时钟）
    TraceCat cat = TraceCat::kLink;
    LogKind kind = LogKind::kText;
    LogLevel level = LogLevel::kInfo; // kHex 记录沿用 kInfo（与同一调用点的文本句子配对）
    uint8_t reserved = 0;             // 保持 12 B 布局，写 0
    uint16_t len = 0;                 // payload 字节数
    uint16_t total = 0; // kHex：原始字节总数（≥ len，超过 65535 记 65535）；kText：等于 len
};
inline constexpr std::size_t kLogHeaderBytes = 12U; // 环内每条记录的固定开销
static_assert(sizeof(LogHeader) == kLogHeaderBytes, "LogHeader must stay 12 bytes");

/** sink：把一条记录交给存储（通常是 LogRing::push）；必须非阻塞、可被多个任务并发调用（自行加锁）。 */
using LogSinkFn = void (*)(void *ctx, const LogHeader &hdr,
                           std::span<const uint8_t> payload) noexcept;

/**
 * @brief  安装文本通道 sink，时钟沿用 traceInstall 注入的时钟。
 * @param  fn: 记录存储回调（通常是 LogRing::push 的适配函数）。
 * @param  ctx: 原样传给 fn 的上下文指针。
 * @retval Result::kOk 安装成功
 * @retval Result::kInvalidArg fn 为 nullptr
 * @retval Result::kInvalidState trace 尚未安装，或已安装过
 * @note   仅启动阶段单线程调用一次，非 ISR；未安装时 tlog() 什么也不做。
 */
[[nodiscard]] Result logInstall(LogSinkFn fn, void *ctx) noexcept;

/**
 * @brief  设置十六进制转储的类别掩码。
 * @param  mask: 位图，bit n 对应 TraceCat n；超出 kTraceAllMask 的位被忽略。
 * @note   原子写，任意任务可调用；只有该类别日志级别为 D 时才生效。
 */
void logSetHexMask(uint32_t mask) noexcept;
/**
 * @brief  读取十六进制转储的类别掩码。
 * @retval 当前掩码（恒 ⊆ kTraceAllMask）
 * @note   原子读，任意任务可调用，不阻塞。
 */
[[nodiscard]] uint32_t logHexMask() noexcept;

#if FW_TRACE_ENABLED
/**
 * @brief  判断该类别的十六进制转储是否打开。
 * @param  cat: 追踪类别。
 * @retval true  该类别日志级别为 D 且掩码对应位置位
 * @retval false 其余情况（含追踪总开关关闭）
 * @note   原子读，任意任务可调用，不阻塞。
 */
[[nodiscard]] bool logHexOn(TraceCat cat) noexcept;

/**
 * @brief  记录一行 kInfo 级文本（ADR-0009 §2.11）；等价 FW_LOGI，但追踪关闭时为空。已废弃，新代码用 fw/Log.hpp
 * @param  cat: 类别；该类别日志级别低于 I 时立即返回，不做格式化
 * @param  fmt: printf 格式
 * @note   在调用方栈上格式化到 kLogTextMax 缓冲（约需 0.8 KB 栈余量）；非阻塞（环满丢弃并计数）。
 *         只能在非实时任务调用：禁止 BT 回调任务、spp 发送路径、link 执行器逐帧热路径。
 */
void tlog(TraceCat cat, const char *fmt, ...) noexcept __attribute__((format(printf, 2, 3)));

/**
 * @brief  记录原始字节的十六进制转储
 * @param  cat: 类别；logHexOn(cat) 为假时立即返回
 * @param  bytes: 原始字节；只复制前 kLogHexMax 字节，记录头 total 保留原长
 * @note   约束同 tlog。
 */
void tlogHex(TraceCat cat, std::span<const uint8_t> bytes) noexcept;
#else
[[nodiscard]] inline bool logHexOn(TraceCat /*cat*/) noexcept
{
    return false;
}

inline void tlog(TraceCat /*cat*/, const char * /*fmt*/, ...) noexcept {}

inline void tlogHex(TraceCat /*cat*/, std::span<const uint8_t> /*bytes*/) noexcept {}
#endif

/** LogRing 统计；init() 后清零。 */
struct LogRingStats {
    uint32_t pushed = 0;  // 成功压入的记录数
    uint32_t dropped = 0; // 空间不足丢弃的记录数（累计）
    uint32_t maxUsed = 0; // 历史最大占用字节数
};

/**
 * 变长记录环：多写者（push，加锁）单读者（peekTime/pop，加锁）。一条记录 = LogHeader + payload，
 * 整条放得下才压入，否则丢弃并计数，绝不阻塞、绝不只写半条。记录可以跨越存储末尾回绕。
 * 存储由调用方提供（≥ sizeof(LogHeader) + kLogTextMax），不分配堆。不可拷贝/移动。
 */
class LogRing {
public:
    constexpr LogRing() noexcept = default;
    LogRing(const LogRing &) = delete;
    LogRing &operator=(const LogRing &) = delete;
    LogRing(LogRing &&) = delete;
    LogRing &operator=(LogRing &&) = delete;
    ~LogRing() = default;

    /**
     * @brief  初始化记录环，绑定调用方存储与锁。
     * @param  storage: 环存储，长度须 ≥ kLogHeaderBytes + kLogTextMax 且 ≤ UINT32_MAX。
     * @param  lock: 记录锁；lock/unlock 须同为 nullptr 或同为非 nullptr。
     * @retval Result::kOk 初始化成功
     * @retval Result::kInvalidArg 存储过小或过大、锁表不成对
     * @retval Result::kInvalidState 已经初始化过
     * @note   仅启动前单线程调用，非 ISR；持注入锁，不阻塞、不分配堆。
     */
    [[nodiscard]] Result init(std::span<uint8_t> storage, const LockOps &lock) noexcept;

    /**
     * @brief  压入一条完整记录（头 + 负载）。
     * @param  hdr: 记录头，hdr.len 必须等于 payload.size()。
     * @param  payload: 负载字节。
     * @retval true  整条写入成功
     * @retval false 未初始化、长度不符或空间不足（空间不足时 dropped+1）
     * @note   多写者任务并发调用，持注入锁，非 ISR；绝不只写半条、不阻塞。
     */
    bool push(const LogHeader &hdr, std::span<const uint8_t> payload) noexcept;

    /**
     * @brief  读最旧记录的时间戳。
     * @param[out] tMs: 最旧记录的毫秒时间戳；失败时不修改。
     * @retval true  取到时间戳
     * @retval false 环为空或未初始化
     * @note   单读者任务调用，持注入锁，非 ISR；读者据此与 TraceRing 按时间合并。
     */
    [[nodiscard]] bool peekTime(uint32_t &tMs) const noexcept;

    /**
     * @brief  弹出最旧记录。
     * @param[out] hdr: 记录头；失败时不修改。
     * @param[out] out: 接收负载的缓冲，不足时截断复制但记录仍整体移除。
     * @param[out] copied: 实际复制的负载字节数。
     * @retval true  记录已移除（可能被截断）
     * @retval false 环为空或未初始化
     * @note   单读者任务调用，持注入锁，非 ISR。
     */
    [[nodiscard]] bool pop(LogHeader &hdr, std::span<uint8_t> out, std::size_t &copied) noexcept;

    /**
     * @brief  取走自上次调用以来新增的丢弃计数。
     * @retval 新增丢弃的记录数；返回后计数清零
     * @note   单读者任务调用，持注入锁，非 ISR。
     */
    [[nodiscard]] uint32_t takeDropped() noexcept;

    /**
     * @brief  读取累计统计。
     * @retval 环的统计快照
     * @note   任意任务调用，持注入锁，非 ISR。
     */
    [[nodiscard]] LogRingStats stats() const noexcept;
    /**
     * @brief  读取当前占用字节数。
     * @retval 环内已用字节数
     * @note   任意任务调用，持注入锁，非 ISR。
     */
    [[nodiscard]] std::size_t used() const noexcept;

    /**
     * @brief  把 LogSinkFn 调用转成 push。
     * @param  ctx: 必须是 LogRing*。
     * @param  hdr: 记录头。
     * @param  payload: 负载。
     * @note   可由多个写者任务并发调用（内部持锁），非 ISR；不阻塞、不分配堆。
     */
    static void sink(void *ctx, const LogHeader &hdr, std::span<const uint8_t> payload) noexcept;

private:
    std::span<uint8_t> buf_{};
    LockOps lock_{};
    std::size_t head_ = 0; // 下一条写入位置
    std::size_t tail_ = 0; // 最旧记录位置
    std::size_t used_ = 0;
    uint32_t dropped_reported_ = 0;
    LogRingStats stats_{};
    bool ready_ = false;
};

} // namespace fw
