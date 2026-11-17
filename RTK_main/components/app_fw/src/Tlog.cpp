// 调试文本日志通道实现（ADR-0009 §2.11）：tlog()/tlog_hex() 两级开关与变长记录环 LogRing。
// 线程：log_install 只在启动阶段单线程调用一次；tlog()/tlog_hex() 任意任务可调用（禁止 ISR、禁止实时路径）；
// LogRing 全部方法由注入的 LockOps 保护，push 可被多个任务并发调用。不分配堆、不抛异常。
// 堆档位：无（格式化缓冲在调用方栈上，环存储由调用方提供）。

#include "fw/Tlog.hpp"

#include "fw/Log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <limits>
#include <span>

#include "fw/Atomic.hpp"
#include "fw/Lock.hpp"
#include "TraceInternal.hpp"

namespace fw {

namespace {

// 环存储下限：至少放得下一条最长的文本记录，否则 init 直接报错，避免运行期全部丢弃。
constexpr size_t kMinLogBytes = kLogHeaderBytes + kLogTextMax;

// sink 用 release 发布、tlog() 用 acquire 读取：保证 install 写入的 ctx 先于 sink 可见。
std::atomic<LogSinkFn> s_log_sink{nullptr};
void *s_log_ctx = nullptr;
// 默认全关：即使类别打开，也要显式 debug on <cat> hex 才转储原始字节。
constinit std::atomic<uint32_t> s_hex_mask{0U};
// CPP-008：原子量必须无锁，否则退回 libatomic 的锁实现（tlog 可能从任意任务调用，锁内不可阻塞）。
static_assert(fw::kAtomicUsable<LogSinkFn>, "日志 sink 原子须无锁");
static_assert(fw::kAtomicUsable<uint32_t>, "日志开关原子须无锁");

// 环内一律按字节视图读写：记录头不保证 4 字节对齐，Xtensa 上非对齐访问会异常。
// std::byte 视图避免 reinterpret_cast，子区间用 subspan 表达，不用裸指针算术。
using ByteSpan = std::span<std::byte>;
using ConstByteSpan = std::span<const std::byte>;

void copyInto(ByteSpan dst, size_t offset, ConstByteSpan src) noexcept
{
    const ByteSpan tail = dst.subspan(offset);
    const size_t first = std::min(src.size(), tail.size());
    if (first > 0U) {
        std::memcpy(tail.data(), src.data(), first);
    }
    const size_t rest = src.size() - first;
    if (rest > 0U) {
        std::memcpy(dst.data(), src.subspan(first).data(), rest);
    }
}

void copyOut(ConstByteSpan src, size_t offset, ByteSpan dst) noexcept
{
    const ConstByteSpan tail = src.subspan(offset);
    const size_t first = std::min(dst.size(), tail.size());
    if (first > 0U) {
        std::memcpy(dst.data(), tail.data(), first);
    }
    const size_t rest = dst.size() - first;
    if (rest > 0U) {
        std::memcpy(dst.subspan(first).data(), src.data(), rest);
    }
}

ConstByteSpan headerBytes(const LogHeader &hdr) noexcept
{
    return std::as_bytes(std::span<const LogHeader>(&hdr, 1U));
}

ByteSpan headerBytes(LogHeader &hdr) noexcept
{
    return std::as_writable_bytes(std::span<LogHeader>(&hdr, 1U));
}

} // namespace

Result logInstall(LogSinkFn fn, void *ctx) noexcept
{
    if (fn == nullptr) {
        return Result::kInvalidArg;
    }
    // 文本记录的时间戳必须与 trace 事件同源，否则读者无法按时间合并两条通道。
    if (detail::installedClock() == nullptr) {
        return Result::kInvalidState;
    }
    s_log_ctx = ctx;
    LogSinkFn expected = nullptr;
    if (!s_log_sink.compare_exchange_strong(expected, fn, std::memory_order_release,
                                            std::memory_order_relaxed)) {
        return Result::kInvalidState;
    }
    return Result::kOk;
}

void logSetHexMask(uint32_t mask) noexcept
{
    // 越界位一律清零：与 trace_set_mask 保持一致，避免调用方以为某类别已生效。
    s_hex_mask.store(mask & kTraceAllMask, std::memory_order_relaxed);
}

uint32_t logHexMask() noexcept
{
    return s_hex_mask.load(std::memory_order_relaxed);
}

#if FW_TRACE_ENABLED

namespace {

// 仅 tlog_hex() 使用；放在开关内，关闭追踪时不留下未用常量。
constexpr uint16_t kMaxPayloadLen = std::numeric_limits<uint16_t>::max();

} // namespace

/**
 * @brief  该类别的十六进制转储是否打开
 * @param  cat: 类别
 * @retval true hex 掩码置位且该类别日志级别为 D
 */
bool logHexOn(TraceCat cat) noexcept
{
    const uint32_t bit = static_cast<uint32_t>(cat);
    if (bit >= static_cast<uint32_t>(TraceCat::kCount)) {
        return false;
    }
    // 两级开关（ADR-0009 §2.12）：转储与同一调用点的 D 级句子配对，需该类别日志级别为 D，十六进制掩码再收一层。
    return ((s_hex_mask.load(std::memory_order_relaxed) & traceBit(cat)) != 0U) &&
           logEnabled(LogLevel::kDebug, cat);
}

void tlog(TraceCat cat, const char *fmt, ...) noexcept
{
    // 冻结接口保留给已有调用点：语义等同 FW_LOGI（ADR-0009 §2.12），格式化与通路选择统一在 Log.cpp。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    va_list ap;
    va_start(ap, fmt);
    vlogLine(LogLevel::kInfo, cat, fmt, ap);
    va_end(ap);
    // NOLINTEND(cppcoreguidelines-pro-type-vararg, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
}

void tlogHex(TraceCat cat, std::span<const uint8_t> bytes) noexcept
{
    if (!logHexOn(cat)) {
        return;
    }
    const LogSinkFn fn = s_log_sink.load(std::memory_order_acquire);
    if ((fn == nullptr) || bytes.empty()) {
        return;
    }
    const size_t len = std::min(bytes.size(), kLogHexMax);
    LogHeader hdr{};
    hdr.tMs = detail::installedClock()();
    hdr.cat = cat;
    hdr.kind = LogKind::kHex;
    // len ≤ kLogHexMax（256）；total 是原始长度，帧长上限 1029，同样远小于 uint16 上限。
    hdr.len = static_cast<uint16_t>(len);
    hdr.total = static_cast<uint16_t>(std::min(bytes.size(), static_cast<size_t>(kMaxPayloadLen)));
    fn(s_log_ctx, hdr, bytes.first(len));
}
#endif // FW_TRACE_ENABLED

Result LogRing::init(std::span<uint8_t> storage, const LockOps &lock) noexcept
{
    if (ready_) {
        return Result::kInvalidState;
    }
    if ((storage.data() == nullptr) || (storage.size() < kMinLogBytes)) {
        return Result::kInvalidArg;
    }
    // lock/unlock 必须成对出现：只给一个会导致死锁或临界区失效，属于调用错误。
    if ((lock.lock == nullptr) != (lock.unlock == nullptr)) {
        return Result::kInvalidArg;
    }
    // stats_.max_used 是 uint32_t；容量超过该上限时拒绝，避免后面反复说明窄化安全。
    if (storage.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return Result::kInvalidArg;
    }
    buf_ = storage;
    lock_ = lock;
    head_ = 0U;
    tail_ = 0U;
    used_ = 0U;
    dropped_reported_ = 0U;
    stats_ = LogRingStats{};
    ready_ = true;
    return Result::kOk;
}

bool LogRing::push(const LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    if (!ready_) {
        return false;
    }
    // 记录必须整条落盘：payload 长度与头声明不一致说明调用方有 bug，直接拒绝而不是写半条。
    if (payload.size() != hdr.len) {
        return false;
    }
    const LockGuard guard(lock_);
    const size_t need = kLogHeaderBytes + static_cast<size_t>(hdr.len);
    // 一条记录都放不下时（容量不足）同样按“空间不足”计数：环必须能容纳最长记录，init 已保证这一点。
    if ((need > buf_.size()) || (used_ + need > buf_.size())) {
        ++stats_.dropped;
        return false;
    }
    copyInto(std::as_writable_bytes(buf_), head_, headerBytes(hdr));
    const size_t payloadAt = (head_ + kLogHeaderBytes) % buf_.size();
    copyInto(std::as_writable_bytes(buf_), payloadAt, std::as_bytes(payload));
    head_ = (head_ + need) % buf_.size();
    used_ += need;
    ++stats_.pushed;
    if (used_ > static_cast<size_t>(stats_.maxUsed)) {
        // init 已校验容量 ≤ UINT32_MAX，因此 used_ 转 uint32_t 不丢位。
        stats_.maxUsed = static_cast<uint32_t>(used_);
    }
    return true;
}

bool LogRing::peekTime(uint32_t &tMs) const noexcept
{
    if (!ready_) {
        return false;
    }
    const LockGuard guard(lock_);
    if (used_ == 0U) {
        return false;
    }
    LogHeader hdr{};
    copyOut(std::as_bytes(std::span<const uint8_t>(buf_.data(), buf_.size())), tail_,
            headerBytes(hdr));
    tMs = hdr.tMs;
    return true;
}

bool LogRing::pop(LogHeader &hdr, std::span<uint8_t> out, size_t &copied) noexcept
{
    if (!ready_) {
        return false;
    }
    const LockGuard guard(lock_);
    if (used_ == 0U) {
        return false;
    }
    copyOut(std::as_bytes(std::span<const uint8_t>(buf_.data(), buf_.size())), tail_,
            headerBytes(hdr));
    const size_t total = kLogHeaderBytes + static_cast<size_t>(hdr.len);
    copied = std::min(static_cast<size_t>(hdr.len), out.size());
    if ((copied > 0U) && (out.data() != nullptr)) {
        copyOut(std::as_bytes(std::span<const uint8_t>(buf_.data(), buf_.size())),
                (tail_ + kLogHeaderBytes) % buf_.size(), std::as_writable_bytes(out).first(copied));
    }
    tail_ = (tail_ + total) % buf_.size();
    used_ -= total;
    return true;
}

uint32_t LogRing::takeDropped() noexcept
{
    if (!ready_) {
        return 0U;
    }
    const LockGuard guard(lock_);
    const uint32_t delta = stats_.dropped - dropped_reported_;
    dropped_reported_ = stats_.dropped;
    return delta;
}

LogRingStats LogRing::stats() const noexcept
{
    if (!ready_) {
        return LogRingStats{};
    }
    const LockGuard guard(lock_);
    return stats_;
}

size_t LogRing::used() const noexcept
{
    if (!ready_) {
        return 0U;
    }
    const LockGuard guard(lock_);
    return used_;
}

void LogRing::sink(void *ctx, const LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    auto *ring = static_cast<LogRing *>(ctx);
    if (ring == nullptr) {
        return;
    }
    ring->push(hdr, payload);
}

namespace detail {

LogSinkFn installedLogSink(void *&ctx) noexcept
{
    const LogSinkFn fn = s_log_sink.load(std::memory_order_acquire);
    if (fn != nullptr) {
        ctx = s_log_ctx;
    }
    return fn;
}

} // namespace detail

} // namespace fw
