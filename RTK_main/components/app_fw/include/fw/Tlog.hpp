// 调试文本日志通道（ADR-0009 §2.11）：与 trace() 并行的第二条通道，承载“句子 + 可选原始字节”。
// trace() 面向实时路径（只记 16 B 事件，不格式化）；tlog() 面向非实时任务（io 执行器、kLink 上的非热路径、
// 控制台任务），由调用方在自己的栈上格式化一行文本，再整条压入变长记录环 LogRing，由同一个读者任务
// （main 的 DebugModule::trace_out）按时间与 TraceRing 合并输出，保证日志、事件、命令回复互不穿插。
// 开关（ADR-0009 §2.12）：文本行按类别日志级别过滤（fw/Log.hpp）；十六进制掩码（log_set_hex_mask）在 D 级之上
// 额外控制 tlog_hex() 的原始字节转储。禁止在 ISR 中调用；不分配堆。纯 C++20，不依赖 IDF/FreeRTOS。
// 冻结接口（ADR-0009 §3；§2.12 增加 LogLevel 与记录头 level 字段）。新代码用 fw/Log.hpp 的 FW_LOG* 宏，
// tlog() 等价于 kInfo 级别，保留给已有调用点。
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
    uint32_t t_ms =
        0; // 记录时间（ms，环通路用 trace_install 的时钟，直写通路用 log_install_direct 的时钟）
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
 * 安装文本通道 sink（启动阶段、单线程调用一次；时钟沿用 trace_install 注入的时钟，须先安装 trace）。
 * fn 为 nullptr → kInvalidArg；trace 未安装 → kInvalidState；重复安装 → kInvalidState。未安装时 tlog() 什么也不做。
 */
[[nodiscard]] Result log_install(LogSinkFn fn, void *ctx) noexcept;

/** 十六进制级别掩码（原子，任意任务可调用）；超出 kTraceAllMask 的位被忽略。只有类别掩码同时打开时才生效。 */
void log_set_hex_mask(uint32_t mask) noexcept;
[[nodiscard]] uint32_t log_hex_mask() noexcept;

#if FW_TRACE_ENABLED
/** 该类别的十六进制转储是否打开（十六进制掩码置位且该类别日志级别为 D，ADR-0009 §2.12）。 */
[[nodiscard]] bool log_hex_on(TraceCat cat) noexcept;

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
 * @param  cat: 类别；log_hex_on(cat) 为假时立即返回
 * @param  bytes: 原始字节；只复制前 kLogHexMax 字节，记录头 total 保留原长
 * @note   约束同 tlog。
 */
void tlog_hex(TraceCat cat, std::span<const uint8_t> bytes) noexcept;
#else
[[nodiscard]] inline bool log_hex_on(TraceCat /*cat*/) noexcept
{
    return false;
}

inline void tlog(TraceCat /*cat*/, const char * /*fmt*/, ...) noexcept {}

inline void tlog_hex(TraceCat /*cat*/, std::span<const uint8_t> /*bytes*/) noexcept {}
#endif

/** LogRing 统计；init() 后清零。 */
struct LogRingStats {
    uint32_t pushed = 0;   // 成功压入的记录数
    uint32_t dropped = 0;  // 空间不足丢弃的记录数（累计）
    uint32_t max_used = 0; // 历史最大占用字节数
};

/**
 * 变长记录环：多写者（push，加锁）单读者（peek_time/pop，加锁）。一条记录 = LogHeader + payload，
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

    /** 启动前调用。storage 过小 → kInvalidArg；重复 init → kInvalidState。lock 被拷贝保存。 */
    [[nodiscard]] Result init(std::span<uint8_t> storage, const LockOps &lock) noexcept;

    /** 压入一条；未 init、payload 超过 hdr.len 声明或空间不足 → false（空间不足时 dropped+1）。hdr.len 必须等于
     * payload.size()。 */
    bool push(const LogHeader &hdr, std::span<const uint8_t> payload) noexcept;

    /** 最旧记录的时间戳；空或未 init → false。读者据此与 TraceRing 按时间合并。 */
    [[nodiscard]] bool peek_time(uint32_t &t_ms) const noexcept;

    /**
     * 弹出最旧记录：头写入 hdr，payload 复制到 out（out.size() 不足时截断复制，记录仍整体移除），
     * copied = 实际复制字节数。空或未 init → false。
     */
    [[nodiscard]] bool pop(LogHeader &hdr, std::span<uint8_t> out, std::size_t &copied) noexcept;

    /** 取走自上次调用以来新增的丢弃数。 */
    [[nodiscard]] uint32_t take_dropped() noexcept;

    [[nodiscard]] LogRingStats stats() const noexcept;
    /** 当前占用字节数。 */
    [[nodiscard]] std::size_t used() const noexcept;

    /** 适配 LogSinkFn：ctx 必须是 LogRing*。 */
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
