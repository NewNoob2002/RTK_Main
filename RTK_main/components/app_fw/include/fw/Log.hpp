// 组件分级日志（ADR-0009 §2.12）：FW_LOGE/W/I/D(类别, 格式, ...)，各组件再包一层自己的宏（如 GNSS_LOGW）。
// 与 trace() 分工：trace 记录实时路径的高频 16 B 事件；日志记录状态变化、错误与周期统计（CODING-STYLE §5.1）。
// 去向：启动完成后 DebugModule 安装文本环（log_install）时写环，由 trace_out 任务按时间与 trace 事件合并输出；
// 否则写 log_install_direct 安装的直写 sink（启动早期、或 CONFIG_RTK_TRACE_ENABLE=n 的固件）；都没有时丢弃并计数。
// 过滤：编译期 FW_LOG_MAX_LEVEL（Kconfig RTK_LOG_LEVEL）之上的级别整句编译掉；
// 运行期按类别级别过滤（`debug log <cat> <e|w|i|d>`，默认全部 I），与追踪掩码（`debug trace`）相互独立。
// C 代码用 fw/LogC.h 的 FW_CLOG*。
// 线程：任意任务可调用，禁止 ISR、BT 回调任务与实时每轮路径（格式化在调用方栈上，约 0.8 KB）。不分配堆。
// 纯 C++20，不依赖 IDF/FreeRTOS。冻结接口（ADR-0009 §3）。
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fw/Result.hpp"
#include "fw/Tlog.hpp"
#include "fw/Trace.hpp"

/** 编译期最高日志级别：0=E 1=W 2=I 3=D；ESP-IDF 下由 CONFIG_RTK_LOG_LEVEL 经 CMake 定义。 */
#ifndef FW_LOG_MAX_LEVEL
#define FW_LOG_MAX_LEVEL 3
#endif

namespace fw {

/** 编译期上限（L0b 起与追踪开关无关：日志级别按类别独立设置，追踪关闭的固件同样输出 I 级）。 */
inline constexpr LogLevel kLogMaxLevel = static_cast<LogLevel>(FW_LOG_MAX_LEVEL);

/** 每个类别占 2 bit 的打包级别表（类别 i 在 bit 2i..2i+1）；用于 NVS 持久化。 */
inline constexpr uint32_t kLogLevelBits = 2U;
inline constexpr uint32_t kLogLevelFieldMask = 0x3U;

/**
 * @brief  生成所有类别同一级别的打包表
 * @param  level: 级别
 * @retval 打包表
 */
constexpr uint32_t log_levels_all(LogLevel level) noexcept
{
    uint32_t packed = 0U;
    for (uint32_t i = 0U; i < static_cast<uint32_t>(TraceCat::kCount); ++i) {
        packed |= (static_cast<uint32_t>(level) & kLogLevelFieldMask) << (i * kLogLevelBits);
    }
    return packed;
}

/** 默认：所有类别 kInfo（启动与运行中的状态变化默认可见，ADR-0009 §2.12）。 */
inline constexpr uint32_t kLogLevelsDefault = log_levels_all(LogLevel::kInfo);
static_assert(static_cast<uint32_t>(TraceCat::kCount) * kLogLevelBits <= 32U, "log level table must fit in u32");

/** 直写通路与环通路共用的一行最大长度（含时间戳、类别、级别前缀与 CRLF）。 */
inline constexpr std::size_t kLogLineMax = kLogTextMax + 32U;

/** 日志丢失统计。 */
struct LogStats {
    uint32_t lost = 0; // 没有任何 sink 时丢弃的条数（累计）
};

/**
 * @brief  安装直写 sink：文本环未安装时日志交给它同步输出
 * @param  fn: 输出函数，收到完整记录头与文本（不含换行）；须可被多个任务并发调用
 * @param  ctx: 原样传给 fn
 * @param  clock: 毫秒时钟，用于填记录头时间戳
 * @retval kOk 安装成功
 * @retval kInvalidArg fn 或 clock 为 nullptr
 * @retval kInvalidState 已安装过
 * @note   启动阶段单线程调用一次（启动最开始，早于任何 FW_LOG*）。
 */
[[nodiscard]] Result log_install_direct(LogSinkFn fn, void *ctx, TraceClockFn clock) noexcept;

/**
 * @brief  设置某类别的运行期日志级别（与追踪掩码无关）
 * @param  cat: 类别；越界忽略
 * @param  level: 新级别；高于 kLogMaxLevel 时按 kLogMaxLevel 处理
 * @note   原子，任意任务可调用。kError 总是记录（最低只能设到 kError）。
 */
void log_set_cat_level(TraceCat cat, LogLevel level) noexcept;

/**
 * @brief  读取某类别的运行期日志级别
 * @param  cat: 类别
 * @retval 当前级别；越界为 kError
 */
[[nodiscard]] LogLevel log_cat_level(TraceCat cat) noexcept;

/**
 * @brief  整表设置（NVS 恢复用）
 * @param  packed: 打包表（见 kLogLevelBits）；每个字段按 kLogMaxLevel 截断，未用高位忽略
 */
void log_set_levels(uint32_t packed) noexcept;

/**
 * @brief  读取整张打包表（NVS 保存用）
 * @retval 打包表
 */
[[nodiscard]] uint32_t log_levels() noexcept;

/**
 * @brief  判断某级别、某类别的日志此刻是否会被记录
 * @param  level: 日志级别
 * @param  cat: 类别
 * @retval true 会记录（调用方可据此跳过昂贵的参数准备）
 * @retval false 会被丢弃
 */
[[nodiscard]] bool log_enabled(LogLevel level, TraceCat cat) noexcept;

/**
 * @brief  记录一行日志（宏 FW_LOG* 的实现；一般不直接调用）
 * @param  level: 日志级别
 * @param  cat: 类别
 * @param  fmt: printf 格式；结尾换行会被去掉，超过 kLogTextMax 截断并以 "..." 结尾
 * @note   非阻塞（环满丢弃并计数）；禁止 ISR。
 */
void log_line(LogLevel level, TraceCat cat, const char *fmt, ...) noexcept __attribute__((format(printf, 3, 4)));

/**
 * @brief  log_line 的 va_list 版本
 * @param  level: 日志级别
 * @param  cat: 类别
 * @param  fmt: printf 格式
 * @param  args: 参数表（调用方负责 va_start/va_end）
 */
void vlog_line(LogLevel level, TraceCat cat, const char *fmt, va_list args) noexcept;

/**
 * @brief  级别的单字母缩写
 * @param  level: 日志级别
 * @retval 'E'/'W'/'I'/'D'；非法值为 '?'
 */
[[nodiscard]] char log_level_char(LogLevel level) noexcept;

/**
 * @brief  解析级别名（e/w/i/d 或 error/warn/info/debug，不区分大小写）
 * @param  name: 级别名
 * @param[out] out: 解析结果；失败时不修改
 * @retval true 解析成功
 * @retval false 不认识的名字
 */
[[nodiscard]] bool log_parse_level(std::string_view name, LogLevel &out) noexcept;

/**
 * @brief  类别的大写短名（控制台行首使用，如 "GNSS"）
 * @param  cat: 类别
 * @retval 名称；越界为 "?"
 */
[[nodiscard]] std::string_view log_cat_name(TraceCat cat) noexcept;

/**
 * @brief  把一条文本记录格式化为控制台行："[   8.712] GNSS W: text\r\n"（kInfo 不带级别前缀）
 * @param  out: 输出缓冲；建议 kLogLineMax 字节
 * @param  hdr: 记录头（取时间、类别、级别）
 * @param  text: 文本（不含结尾 0 与换行）
 * @retval 写入的字节数（不含结尾 0；缓冲不足时截断，仍以 CRLF 结尾）
 * @note   环通路（DebugModule）与直写通路共用，保证两条通路行格式一致。
 */
std::size_t log_format_line(std::span<char> out, const LogHeader &hdr, std::span<const uint8_t> text) noexcept;

/**
 * @brief  读取丢失统计
 * @retval 自启动以来的累计值
 */
[[nodiscard]] LogStats log_stats() noexcept;

} // namespace fw

// 宏而非函数：高于 kLogMaxLevel 的调用连同参数求值一起编译掉。
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define FW_LOG_AT(level, cat, ...)                                                                                     \
    do {                                                                                                               \
        if constexpr ((level) <= ::fw::kLogMaxLevel) {                                                                 \
            ::fw::log_line((level), (cat), __VA_ARGS__);                                                               \
        }                                                                                                              \
    } while (0)
#define FW_LOGE(cat, ...) FW_LOG_AT(::fw::LogLevel::kError, cat, __VA_ARGS__)
#define FW_LOGW(cat, ...) FW_LOG_AT(::fw::LogLevel::kWarn, cat, __VA_ARGS__)
#define FW_LOGI(cat, ...) FW_LOG_AT(::fw::LogLevel::kInfo, cat, __VA_ARGS__)
#define FW_LOGD(cat, ...) FW_LOG_AT(::fw::LogLevel::kDebug, cat, __VA_ARGS__)
// NOLINTEND(cppcoreguidelines-macro-usage)
