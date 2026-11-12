// 运行时调试追踪实现（ADR-0009 §2.1/§2.2）：sink 与时钟安装、类别掩码、定长事件环。
// 线程：trace_install 只在启动阶段单线程调用一次；trace()/trace_set_mask() 任意任务可调用，禁止在 ISR 中调用；
// TraceRing 全部方法由注入的 LockOps 保护，push 可被多个任务并发调用。不分配堆、不抛异常。

#include "fw/trace.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "fw/lock.hpp"

namespace fw {

namespace {

constexpr uint32_t kCatCount = static_cast<uint32_t>(TraceCat::kCount);
constexpr size_t kMinEventsInRing = 2U;

// sink 用 release 发布、trace() 用 acquire 读取：保证 install 写入的 ctx 与 clock 先于 sink 可见。
std::atomic<TraceSinkFn> s_sink{nullptr};
void *s_sink_ctx = nullptr;
TraceClockFn s_clock = nullptr;
constinit std::atomic<uint32_t> s_mask{kTraceAllMask};

} // namespace

Result trace_install(TraceSinkFn fn, void *ctx, TraceClockFn clock) noexcept
{
    if ((fn == nullptr) || (clock == nullptr)) {
        return Result::kInvalidArg;
    }
    // 先写 ctx/clock 再用 release 发布 sink；sink 为 nullptr 时 trace() 直接返回，写脏也无人读取。
    s_sink_ctx = ctx;
    s_clock = clock;
    TraceSinkFn expected = nullptr;
    if (!s_sink.compare_exchange_strong(expected, fn, std::memory_order_release, std::memory_order_relaxed)) {
        return Result::kInvalidState;
    }
    return Result::kOk;
}

void trace_set_mask(uint32_t mask) noexcept
{
    // 越界位一律清零：trace() 只按位检查，清掉可避免调用方误以为某类别已生效。
    s_mask.store(mask & kTraceAllMask, std::memory_order_relaxed);
}

uint32_t trace_mask() noexcept
{
    return s_mask.load(std::memory_order_relaxed);
}

#if FW_TRACE_ENABLED
bool trace_on(TraceCat cat) noexcept
{
    const uint32_t bit = static_cast<uint32_t>(cat);
    if (bit >= kCatCount) {
        return false;
    }
    return (s_mask.load(std::memory_order_relaxed) & trace_bit(cat)) != 0U;
}

void trace(TraceCat cat, uint8_t code, uint16_t session, uint32_t a, uint32_t b) noexcept
{
    if (!trace_on(cat)) {
        return;
    }
    const TraceSinkFn fn = s_sink.load(std::memory_order_acquire);
    if (fn == nullptr) {
        return;
    }
    TraceEvent ev{};
    ev.t_ms = s_clock();
    ev.cat = cat;
    ev.code = code;
    ev.session = session;
    ev.a = a;
    ev.b = b;
    fn(s_sink_ctx, ev);
}
#endif // FW_TRACE_ENABLED

Result TraceRing::init(std::span<TraceEvent> storage, const LockOps &lock) noexcept
{
    if (ready_) {
        return Result::kInvalidState;
    }
    if ((storage.data() == nullptr) || (storage.size() < kMinEventsInRing)) {
        return Result::kInvalidArg;
    }
    // lock/unlock 必须成对出现：只给一个会导致死锁或临界区失效，属于调用错误。
    if ((lock.lock == nullptr) != (lock.unlock == nullptr)) {
        return Result::kInvalidArg;
    }
    // max_depth 是 uint32_t；容量超过该上限时拒绝，避免后面反复说明窄化安全。
    if (storage.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return Result::kInvalidArg;
    }
    buf_ = storage;
    lock_ = lock;
    head_ = 0U;
    count_ = 0U;
    dropped_reported_ = 0U;
    stats_ = TraceRingStats{};
    ready_ = true;
    return Result::kOk;
}

bool TraceRing::push(const TraceEvent &ev) noexcept
{
    if (!ready_) {
        return false;
    }
    const LockGuard guard(lock_);
    const size_t cap = buf_.size();
    if (count_ >= cap) {
        // 环满丢新事件：保留已有事件的时间顺序，丢新是最便宜且不破坏历史的策略。
        ++stats_.dropped;
        return false;
    }
    buf_[(head_ + count_) % cap] = ev;
    ++count_;
    ++stats_.pushed;
    if (count_ > static_cast<size_t>(stats_.max_depth)) {
        // init 已校验 capacity ≤ UINT32_MAX，因此 count_ 转 uint32_t 不丢位。
        stats_.max_depth = static_cast<uint32_t>(count_);
    }
    return true;
}

bool TraceRing::pop(TraceEvent &out) noexcept
{
    if (!ready_) {
        return false;
    }
    const LockGuard guard(lock_);
    if (count_ == 0U) {
        return false;
    }
    out = buf_[head_];
    head_ = (head_ + 1U) % buf_.size();
    --count_;
    return true;
}

uint32_t TraceRing::take_dropped() noexcept
{
    if (!ready_) {
        return 0U;
    }
    const LockGuard guard(lock_);
    const uint32_t delta = stats_.dropped - dropped_reported_;
    dropped_reported_ = stats_.dropped;
    return delta;
}

TraceRingStats TraceRing::stats() const noexcept
{
    if (!ready_) {
        return TraceRingStats{};
    }
    const LockGuard guard(lock_);
    return stats_;
}

size_t TraceRing::depth() const noexcept
{
    if (!ready_) {
        return 0U;
    }
    const LockGuard guard(lock_);
    return count_;
}

void TraceRing::sink(void *ctx, const TraceEvent &ev) noexcept
{
    auto *ring = static_cast<TraceRing *>(ctx);
    if (ring == nullptr) {
        return;
    }
    ring->push(ev);
}

} // namespace fw
