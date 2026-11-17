// 组件分级日志实现（ADR-0009 §2.12）：级别过滤、格式化、在文本环与直写 sink 之间选择通路。
// 线程：log_install_direct 只在启动阶段单线程调用一次；类别级别表为原子 u32（CAS 更新）；其余函数任意任务可调用（禁止
// ISR）。 堆档位：无（格式化缓冲约 0.2 KB 在调用方栈上）。

#include "fw/Log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "TraceInternal.hpp"
#include "fw/LogC.h"

namespace fw {

namespace {

// 截断标记 "..." 的长度：文本超长时覆盖末尾三字节。
constexpr std::size_t kEllipsisLen = 3U;
constexpr char kDot = '.';
constexpr uint32_t kMsPerSecond = 1000U;

// 直写 sink：fn 最后以 release 发布，读者 acquire 后 ctx/clock 一定可见。
std::atomic<LogSinkFn> s_direct_fn{nullptr};
void *s_direct_ctx = nullptr;
TraceClockFn s_direct_clock = nullptr;

constinit std::atomic<uint32_t> s_levels{kLogLevelsDefault};
constinit std::atomic<uint32_t> s_lost{0U};
// CPP-008：原子量必须无锁，否则退回 libatomic 的锁实现（各级别日志从任意任务调用）。
static_assert(std::atomic<LogSinkFn>::is_always_lock_free, "日志直写 sink 原子须无锁");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "日志级别/丢弃计数原子须无锁");

constexpr std::array<std::string_view, static_cast<std::size_t>(TraceCat::kCount)> kCatNames = {
    {"LINK", "SPP", "RTCM", "PROTO", "ALARM", "STORE", "EXEC", "GNSS", "SYS"}};

/**
 * @brief  把级别限制在编译期上限内
 * @param  level: 请求的级别
 * @retval 不高于 kLogMaxLevel 的级别
 */
constexpr LogLevel clamp_level(LogLevel level) noexcept
{
    return (level > kLogMaxLevel) ? kLogMaxLevel : level;
}

/**
 * @brief  格式化正文：去掉结尾换行，超长时以 "..." 结尾
 * @param  buf: 输出缓冲（kLogTextMax + 1 字节，含结尾 0）
 * @param  fmt: printf 格式
 * @param  args: 参数表
 * @retval 正文字节数（≤ kLogTextMax）；格式化失败为 0
 */
std::size_t format_text(std::array<char, kLogTextMax + 1U> &buf, const char *fmt, va_list args) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const int n = std::vsnprintf(buf.data(), buf.size(), fmt, args);
    if (n < 0) {
        // 格式化失败也留一条空记录，便于看出调用点有问题，而不是静默丢日志。
        buf[0] = '\0';
        return 0U;
    }
    std::size_t len = static_cast<std::size_t>(n);
    if (len > kLogTextMax) {
        len = kLogTextMax;
        for (std::size_t i = 0U; i < kEllipsisLen; ++i) {
            buf[len - kEllipsisLen + i] = kDot;
        }
        return len;
    }
    // 记录不带换行（输出端统一补 CRLF），调用点习惯写 "\n" 时在这里削掉。
    while ((len > 0U) && ((buf[len - 1U] == '\n') || (buf[len - 1U] == '\r'))) {
        --len;
    }
    return len;
}

} // namespace

Result log_install_direct(LogSinkFn fn, void *ctx, TraceClockFn clock) noexcept
{
    if ((fn == nullptr) || (clock == nullptr)) {
        return Result::kInvalidArg;
    }
    if (s_direct_fn.load(std::memory_order_acquire) != nullptr) {
        return Result::kInvalidState;
    }
    s_direct_ctx = ctx;
    s_direct_clock = clock;
    s_direct_fn.store(fn, std::memory_order_release);
    return Result::kOk;
}

void log_set_cat_level(TraceCat cat, LogLevel level) noexcept
{
    const auto index = static_cast<uint32_t>(cat);
    if (index >= static_cast<uint32_t>(TraceCat::kCount)) {
        return;
    }
    const uint32_t shift = index * kLogLevelBits;
    const uint32_t field = (static_cast<uint32_t>(clamp_level(level)) & kLogLevelFieldMask) << shift;
    uint32_t cur = s_levels.load(std::memory_order_relaxed);
    // CAS 循环：多个任务同时改不同类别时互不覆盖。
    while (!s_levels.compare_exchange_weak(cur, (cur & ~(kLogLevelFieldMask << shift)) | field,
                                           std::memory_order_relaxed)) {
    }
}

LogLevel log_cat_level(TraceCat cat) noexcept
{
    const auto index = static_cast<uint32_t>(cat);
    if (index >= static_cast<uint32_t>(TraceCat::kCount)) {
        return LogLevel::kError;
    }
    const uint32_t field = (s_levels.load(std::memory_order_relaxed) >> (index * kLogLevelBits)) & kLogLevelFieldMask;
    return static_cast<LogLevel>(field);
}

void log_set_levels(uint32_t packed) noexcept
{
    uint32_t clamped = 0U;
    for (uint32_t i = 0U; i < static_cast<uint32_t>(TraceCat::kCount); ++i) {
        const auto level = static_cast<LogLevel>((packed >> (i * kLogLevelBits)) & kLogLevelFieldMask);
        clamped |= static_cast<uint32_t>(clamp_level(level)) << (i * kLogLevelBits);
    }
    s_levels.store(clamped, std::memory_order_relaxed);
}

uint32_t log_levels() noexcept
{
    return s_levels.load(std::memory_order_relaxed);
}

bool log_enabled(LogLevel level, TraceCat cat) noexcept
{
    if ((level > kLogMaxLevel) || (static_cast<uint32_t>(cat) >= static_cast<uint32_t>(TraceCat::kCount))) {
        return false;
    }
    // 与追踪掩码无关：只看该类别的日志级别（kError 是最低可设级别，因此错误总会记录）。
    return level <= log_cat_level(cat);
}

void log_line(LogLevel level, TraceCat cat, const char *fmt, ...) noexcept
{
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    va_list ap;
    va_start(ap, fmt);
    vlog_line(level, cat, fmt, ap);
    va_end(ap);
    // NOLINTEND(cppcoreguidelines-pro-type-vararg, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
}

void vlog_line(LogLevel level, TraceCat cat, const char *fmt, va_list args) noexcept
{
    if ((fmt == nullptr) || !log_enabled(level, cat)) {
        return;
    }
    // 优先写文本环（与 trace 事件同一时钟、由 trace_out 按时间合并）；环未装时直写控制台。
    void *ctx = nullptr;
    LogSinkFn fn = detail::installed_log_sink(ctx);
    TraceClockFn clock = detail::installed_clock();
    if (fn == nullptr) {
        fn = s_direct_fn.load(std::memory_order_acquire);
        ctx = s_direct_ctx;
        clock = s_direct_clock;
    }
    if ((fn == nullptr) || (clock == nullptr)) {
        s_lost.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    std::array<char, kLogTextMax + 1U> buf{};
    const std::size_t len = format_text(buf, fmt, args);

    LogHeader hdr{};
    hdr.t_ms = clock();
    hdr.cat = cat;
    hdr.kind = LogKind::kText;
    hdr.level = level;
    // len ≤ kLogTextMax（160），窄化安全。
    hdr.len = static_cast<uint16_t>(len);
    hdr.total = hdr.len;
    // 文本按字节交给 sink；char 与 uint8_t 同宽，按对象表示读取是允许的。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    fn(ctx, hdr, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(buf.data()), len));
}

char log_level_char(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::kError:
        return 'E';
    case LogLevel::kWarn:
        return 'W';
    case LogLevel::kInfo:
        return 'I';
    case LogLevel::kDebug:
        return 'D';
    default:
        return '?';
    }
}

bool log_parse_level(std::string_view name, LogLevel &out) noexcept
{
    struct Entry {
        std::string_view short_name;
        std::string_view long_name;
        LogLevel level;
    };
    constexpr std::array<Entry, 4U> kTable = {{
        {"e", "error", LogLevel::kError},
        {"w", "warn", LogLevel::kWarn},
        {"i", "info", LogLevel::kInfo},
        {"d", "debug", LogLevel::kDebug},
    }};
    std::array<char, 8U> lower{};
    if (name.empty() || (name.size() >= lower.size())) {
        return false;
    }
    for (std::size_t i = 0U; i < name.size(); ++i) {
        const char c = name[i];
        lower[i] = ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view key(lower.data(), name.size());
    for (const Entry &e : kTable) {
        if ((key == e.short_name) || (key == e.long_name)) {
            out = e.level;
            return true;
        }
    }
    return false;
}

std::string_view log_cat_name(TraceCat cat) noexcept
{
    const auto index = static_cast<std::size_t>(cat);
    return (index < kCatNames.size()) ? kCatNames[index] : std::string_view("?");
}

std::size_t log_format_line(std::span<char> out, const LogHeader &hdr, std::span<const uint8_t> text) noexcept
{
    constexpr std::string_view kCrlf = "\r\n";
    if (out.size() <= kCrlf.size()) {
        return 0U;
    }
    const std::string_view cat = log_cat_name(hdr.cat);
    const unsigned long sec = hdr.t_ms / kMsPerSecond;
    const unsigned long ms = hdr.t_ms % kMsPerSecond;
    // kInfo 是最常见的级别，不加前缀，保持与 T4 已有行格式一致；其他级别加 "E:"/"W:"/"D:" 便于 grep。
    const bool tagged = hdr.level != LogLevel::kInfo;
    // 预留 CRLF：前缀与正文最多写到 out.size() - 2。
    const std::size_t room = out.size() - kCrlf.size();
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
    const int n = tagged ? std::snprintf(out.data(), room + 1U, "[%4lu.%03lu] [%.*s] %c: ", sec, ms,
                                         static_cast<int>(cat.size()), cat.data(), log_level_char(hdr.level))
                         : std::snprintf(out.data(), room + 1U, "[%4lu.%03lu] [%.*s] ", sec, ms,
                                         static_cast<int>(cat.size()), cat.data());
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    std::size_t pos = (n < 0) ? 0U : std::min(static_cast<std::size_t>(n), room);
    const std::size_t copy = std::min(text.size(), room - pos);
    if (copy > 0U) {
        std::memcpy(out.subspan(pos).data(), text.data(), copy);
        pos += copy;
    }
    std::memcpy(out.subspan(pos).data(), kCrlf.data(), kCrlf.size());
    pos += kCrlf.size();
    if (pos < out.size()) {
        out[pos] = '\0';
    }
    return pos;
}

static_assert(FW_LOG_CAT_SYS == static_cast<int>(TraceCat::kSys), "LogC.h category values must match TraceCat");
static_assert(FW_LOG_CAT_GNSS == static_cast<int>(TraceCat::kGnss), "LogC.h category values must match TraceCat");
static_assert(FW_LOG_CAT_LINK == static_cast<int>(TraceCat::kLink), "LogC.h category values must match TraceCat");
static_assert(FW_LOG_LEVEL_DEBUG == static_cast<int>(LogLevel::kDebug), "LogC.h level values must match LogLevel");
static_assert(FW_LOG_LEVEL_ERROR == static_cast<int>(LogLevel::kError), "LogC.h level values must match LogLevel");

LogStats log_stats() noexcept
{
    LogStats st{};
    st.lost = s_lost.load(std::memory_order_relaxed);
    return st;
}

} // namespace fw

extern "C" void fw_log_c(int level, int cat, const char *fmt, ...)
{
    if ((level < 0) || (level > static_cast<int>(fw::LogLevel::kDebug)) || (cat < 0) ||
        (cat >= static_cast<int>(fw::TraceCat::kCount))) {
        return;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    va_list ap;
    va_start(ap, fmt);
    fw::vlog_line(static_cast<fw::LogLevel>(level), static_cast<fw::TraceCat>(cat), fmt, ap);
    va_end(ap);
    // NOLINTEND(cppcoreguidelines-pro-type-vararg, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
}
