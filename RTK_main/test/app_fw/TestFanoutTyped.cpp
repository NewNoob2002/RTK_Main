// typed FanoutRing host 回归：订阅、边界、短租约、回绕与确定性并发发布。
// 仅 host 测试线程调用；pthread 同步仅作为注入 LockOps，不进入纯组件。

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <pthread.h>
#include <span>

#include "fw/FanoutRing.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace fw {

// 只在无并发访问时设置边界值，避免用数十亿次操作验证计数器耗尽。
struct FanoutRingTestAccess {
    static void empty_at(FanoutRing &ring, uint32_t pos) noexcept
    {
        ring.head_ = pos;
        ring.oldest_ = pos;
        for (auto &reader : ring.readers_) {
            reader.tail = pos;
        }
    }
    static void serial(FanoutRing &ring, uint8_t id, uint32_t value) noexcept
    {
        ring.readers_[id].serial = value;
    }
    static void generation(FanoutRing &ring, uint8_t id, uint32_t value) noexcept
    {
        ring.readers_[id].generation = value;
    }
    static constexpr size_t reader_size() noexcept
    {
        return sizeof(FanoutRing::Reader);
    }
};

} // namespace fw

namespace {

using fw::Result;
constexpr fw::FanoutMask kType0 = 1U;
constexpr fw::FanoutMask kType31 = fw::FanoutMask{1U} << 31U;
constexpr size_t kCap = 32U;
constexpr size_t kAppCap = 4096U;
constexpr size_t kFullPayload = 13U;
constexpr size_t kReservationPayload = 5U;
constexpr size_t kMixedReader = 3U;
constexpr size_t kOverwriteRounds = 6U;
constexpr size_t kPublicationLock = 3U;
constexpr unsigned kBarrierParticipants = 3U;
constexpr uint8_t kHighestType = 31U;
constexpr size_t kLengthSentinel = 99U;
constexpr size_t kByteMask = 0xFFU;
constexpr uint8_t kBadId = fw::kFanoutMaxReaders;
constexpr uint32_t kMaxCounter = std::numeric_limits<uint32_t>::max();
constexpr size_t kRounds = 200U;
constexpr std::array<uint8_t, 1> kByte{0xA5U};

// Unity 断言仅在主线程运行；子线程的系统调用失败立即终止，避免测试被卡住。
void sys_ok(int code) noexcept
{
    if (code != 0) {
        std::abort();
    }
}

void expect(Result expected, Result actual)
{
    TEST_ASSERT_TRUE(expected == actual);
}

void pattern(std::span<uint8_t> bytes)
{
    for (size_t i = 0U; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(i & kByteMask); // 仅保留低 8 位。
    }
}

void view_equal(const fw::FanoutView &view, std::span<const uint8_t> expected)
{
    TEST_ASSERT_EQUAL_size_t(expected.size(), view.first.size() + view.second.size());
    TEST_ASSERT_TRUE(std::equal(view.first.begin(), view.first.end(), expected.begin()));
    TEST_ASSERT_TRUE(std::equal(view.second.begin(), view.second.end(),
                                expected.subspan(view.first.size()).begin()));
}

void test_validation_and_modes()
{
    fw::FanoutRing ring;
    std::array<uint8_t, kCap> storage{};
    fw::FanoutView view{};
    uint8_t id = 0U;
    expect(Result::kInvalidState, ring.attach(kType0, id));
    expect(Result::kInvalidState, ring.set_mask(id, kType0));
    expect(Result::kInvalidState, ring.write(0U, kByte));
    expect(Result::kInvalidState, ring.peek(id, view));
    expect(Result::kInvalidState, ring.commit({}));
    expect(Result::kInvalidState, ring.cancel({}));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.typed_writer_stats().base.records);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.typed_reader_stats(id).filtered);
    expect(Result::kInvalidArg, ring.init_typed({}, 1U, fw::kNoLock));
    expect(Result::kInvalidArg, ring.init_typed(storage, 0U, fw::kNoLock));
    expect(Result::kInvalidArg, ring.init_typed(storage, 65536U, fw::kNoLock));
    expect(Result::kInvalidArg, ring.init_typed(storage, kCap, fw::kNoLock));
    expect(Result::kInvalidArg,
           ring.init_typed(std::span(storage).first(kCap - 1U), 1U, fw::kNoLock));
    const fw::LockOps bad_lock{[](void *) noexcept {}, nullptr, nullptr};
    expect(Result::kInvalidArg, ring.init_typed(storage, 1U, bad_lock));
    expect(Result::kOk, ring.init_typed(storage, kCap - fw::kFanoutTypedHeaderBytes, fw::kNoLock));
    expect(Result::kInvalidState, ring.init_typed(storage, 1U, fw::kNoLock));
    expect(Result::kInvalidState, ring.init(storage, 1U, fw::kNoLock));
    expect(Result::kNotSupported, ring.attach(id));
    expect(Result::kNotSupported, ring.write(kByte));
    size_t length = kLengthSentinel;
    expect(Result::kNotSupported, ring.read(id, std::span<uint8_t>{}, length));
    TEST_ASSERT_EQUAL_size_t(0U, length);
    expect(Result::kInvalidArg, ring.peek(kBadId, view));
    expect(Result::kInvalidArg, ring.set_mask(kBadId, 0U));
    expect(Result::kInvalidArg, ring.commit({1U, 1U, kBadId}));
    expect(Result::kInvalidArg, ring.detach(id));
    expect(Result::kInvalidArg, ring.write(32U, kByte));
    expect(Result::kInvalidArg, ring.write(0U, {}));
    TEST_ASSERT_EQUAL_UINT32(2U, ring.typed_writer_stats().base.rejected);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.typed_writer_stats().oversize_drop);

    fw::FanoutRing legacy;
    expect(Result::kOk, legacy.init(storage, 1U, fw::kNoLock));
    expect(Result::kNotSupported, legacy.attach(kType0, id));
    expect(Result::kNotSupported, legacy.set_mask(id, kType0));
    expect(Result::kNotSupported, legacy.write(0U, kByte));
    expect(Result::kNotSupported, legacy.peek(id, view));
    expect(Result::kNotSupported, legacy.commit({}));
    expect(Result::kNotSupported, legacy.cancel({}));
    TEST_ASSERT_EQUAL_UINT32(0U, legacy.typed_writer_stats().base.records);
    TEST_ASSERT_EQUAL_UINT32(0U, legacy.typed_reader_stats(id).base.records);
}

void test_masks_and_independent_readers()
{
    fw::FanoutRing ring;
    std::array<uint8_t, kCap> storage{};
    expect(Result::kOk, ring.init_typed(storage, 1U, fw::kNoLock));
    expect(Result::kOk, ring.write(0U, kByte)); // 无读者不发布。
    std::array<uint8_t, fw::kFanoutMaxReaders> ids{};
    expect(Result::kOk, ring.attach(0U, ids[0]));
    expect(Result::kOk, ring.write(kHighestType, kByte)); // mask0 不订阅。
    expect(Result::kOk, ring.attach(kType0, ids[1]));
    expect(Result::kOk, ring.attach(kType31, ids[2]));
    expect(Result::kOk, ring.attach(kType0 | kType31, ids[kMixedReader]));
    uint8_t extra = kBadId;
    expect(Result::kNoSpace, ring.attach(kType0, extra));
    fw::FanoutView view{};
    expect(Result::kNotFound, ring.peek(ids[1], view));
    expect(Result::kOk, ring.write(0U, kByte));
    expect(Result::kOk, ring.write(kHighestType, kByte));
    expect(Result::kOk, ring.peek(ids[1], view));
    TEST_ASSERT_EQUAL_UINT8(0U, view.type);
    view_equal(view, kByte);
    expect(Result::kOk, ring.commit(view.lease));
    expect(Result::kNotFound, ring.peek(ids[1], view));
    expect(Result::kOk, ring.peek(ids[2], view));
    TEST_ASSERT_EQUAL_UINT8(31U, view.type);
    expect(Result::kOk, ring.commit(view.lease));
    expect(Result::kNotFound, ring.peek(ids[0], view));
    for (const uint8_t type : {uint8_t{0U}, uint8_t{kHighestType}}) {
        expect(Result::kOk, ring.peek(ids[kMixedReader], view));
        TEST_ASSERT_EQUAL_UINT8(type, view.type);
        expect(Result::kOk, ring.commit(view.lease));
    }
    TEST_ASSERT_EQUAL_UINT32(2U, ring.typed_reader_stats(ids[0]).filtered);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(ids[1]).filtered);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(ids[2]).filtered);
    TEST_ASSERT_EQUAL_UINT32(2U, ring.typed_reader_stats(ids[kMixedReader]).base.records);
    TEST_ASSERT_EQUAL_UINT32(2U, ring.typed_writer_stats().no_subscriber);
    TEST_ASSERT_EQUAL_UINT32(2U, ring.writer_stats().records);
    expect(Result::kOk, ring.write(0U, kByte));
    expect(Result::kOk, ring.set_mask(ids[1], kType31));
    expect(Result::kNotFound, ring.peek(ids[1], view)); // 改 mask 从最新开始。
    expect(Result::kOk, ring.detach(ids[2]));
    expect(Result::kOk, ring.attach(kType31, extra));
    TEST_ASSERT_EQUAL_UINT8(ids[2], extra);
    expect(Result::kNotFound, ring.peek(extra, view));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(extra).records);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.typed_reader_stats(extra).filtered);
}

void test_overwrite_filters_and_progress()
{
    fw::FanoutRing ring;
    std::array<uint8_t, 16U> storage{};
    expect(Result::kOk, ring.init_typed(storage, 1U, fw::kNoLock));
    uint8_t slow = 0U;
    uint8_t fast = 0U;
    uint8_t other = 0U;
    expect(Result::kOk, ring.attach(kType0, slow));
    expect(Result::kOk, ring.attach(kType0 | kType31, fast));
    expect(Result::kOk, ring.attach(kType31, other));
    fw::FanoutView view{};
    for (size_t i = 0U; i < kOverwriteRounds; ++i) {
        expect(Result::kOk, ring.write((i % 2U == 0U) ? 0U : kHighestType, kByte));
        expect(Result::kOk, ring.peek(fast, view));
        expect(Result::kOk, ring.commit(view.lease));
    }
    TEST_ASSERT_EQUAL_UINT32(2U, ring.writer_stats().overwritten);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(slow).base.dropped);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(slow).filtered);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(other).base.dropped);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(other).filtered);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(fast).dropped);
    TEST_ASSERT_EQUAL_size_t(storage.size(), ring.typed_writer_stats().high_water_bytes);
}

void test_leases_and_pin_rejection()
{
    fw::FanoutRing ring;
    std::array<uint8_t, 16U> storage{};
    expect(Result::kOk, ring.init_typed(storage, kFullPayload, fw::kNoLock));
    uint8_t a = 0U;
    uint8_t b = 0U;
    uint8_t unrelated = 0U;
    expect(Result::kOk, ring.attach(kType0, a));
    expect(Result::kOk, ring.attach(kType0, b));
    expect(Result::kOk, ring.attach(kType31, unrelated));
    expect(Result::kOk, ring.write(0U, kByte));
    fw::FanoutView view{};
    expect(Result::kOk, ring.peek(a, view));
    const fw::FanoutView pinned = view;
    expect(Result::kBusy, ring.peek(a, view));
    TEST_ASSERT_TRUE(view.first.empty());
    expect(Result::kBusy, ring.detach(a));
    expect(Result::kBusy, ring.set_mask(a, kType31));
    std::array<uint8_t, kFullPayload> full{};
    pattern(full);
    expect(Result::kBusy, ring.write(0U, full));
    view_equal(pinned, kByte);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_writer_stats().pinned_drop);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(a).pinned_drop);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_reader_stats(b).pinned_drop);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.typed_reader_stats(unrelated).pinned_drop);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.writer_stats().overwritten);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.writer_stats().rejected);
    auto bad = pinned.lease;
    ++bad.generation;
    expect(Result::kInvalidArg, ring.commit(bad));
    ++bad.serial;
    expect(Result::kInvalidArg, ring.cancel(bad));
    expect(Result::kOk, ring.cancel(pinned.lease));
    expect(Result::kInvalidArg, ring.commit(pinned.lease));
    expect(Result::kOk, ring.peek(a, view));
    TEST_ASSERT_TRUE(view.lease.serial != pinned.lease.serial);
    view_equal(view, kByte); // cancel 没消费。
    expect(Result::kInvalidArg, ring.cancel(pinned.lease));
    expect(Result::kOk, ring.commit(view.lease));
    expect(Result::kInvalidArg, ring.commit(view.lease));
    TEST_ASSERT_EQUAL_UINT32(1U, ring.reader_stats(a).records);
    expect(Result::kOk, ring.detach(a));
    expect(Result::kOk, ring.attach(kType0, a));
    expect(Result::kOk, ring.write(0U, full));
    expect(Result::kOk, ring.peek(a, view));
    expect(Result::kInvalidArg, ring.commit(pinned.lease));
    view_equal(view, full);
    expect(Result::kOk, ring.commit(view.lease));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(a).dropped);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.reader_stats(b).dropped);
}

void test_pin_rejection_does_not_partially_evict()
{
    fw::FanoutRing ring;
    std::array<uint8_t, 16U> storage{};
    expect(Result::kOk, ring.init_typed(storage, kFullPayload, fw::kNoLock));
    uint8_t id = 0U;
    expect(Result::kOk, ring.attach(kType0, id));
    expect(Result::kOk, ring.write(0U, kByte));
    expect(Result::kOk, ring.write(0U, kByte));
    fw::FanoutView view{};
    expect(Result::kOk, ring.peek(id, view));
    expect(Result::kOk, ring.commit(view.lease));
    expect(Result::kOk, ring.peek(id, view)); // pin 在第二条，第一条可以被覆盖。
    const std::array<uint8_t, kFullPayload> full{};
    expect(Result::kBusy, ring.write(0U, full));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.writer_stats().overwritten);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(id).dropped);
    view_equal(view, kByte);
    expect(Result::kOk, ring.commit(view.lease));
}

void test_capacity_and_oversize()
{
    std::array<uint8_t, 8192U> backing{};
    std::array<uint8_t, 8192U> payload{};
    pattern(payload);
    for (const size_t cap : {size_t{2048U}, size_t{4096U}, size_t{8192U}}) {
        fw::FanoutRing ring;
        const size_t max = cap - fw::kFanoutTypedHeaderBytes;
        expect(Result::kOk, ring.init_typed(std::span(backing).first(cap), max, fw::kNoLock));
        uint8_t id = 0U;
        expect(Result::kOk, ring.attach(kType0, id));
        const auto bytes = std::span(payload).first(max);
        expect(Result::kOk, ring.write(0U, bytes));
        fw::FanoutView view{};
        expect(Result::kOk, ring.peek(id, view));
        view_equal(view, bytes);
        expect(Result::kOk, ring.commit(view.lease));
        TEST_ASSERT_EQUAL_size_t(max + fw::kFanoutTypedHeaderBytes,
                                 ring.typed_writer_stats().high_water_bytes);
        if (cap == kAppCap) {
            expect(Result::kInvalidArg, ring.write(0U, std::span(payload).first(kAppCap)));
            TEST_ASSERT_EQUAL_UINT32(1U, ring.typed_writer_stats().oversize_drop);
            TEST_ASSERT_EQUAL_UINT32(1U, ring.writer_stats().rejected);
            TEST_ASSERT_EQUAL_UINT32(1U, ring.writer_stats().records);
            expect(Result::kNotFound, ring.peek(id, view));
        } else if (cap > kAppCap) {
            expect(Result::kOk, ring.write(0U, std::span(payload).first(kAppCap)));
            expect(Result::kOk, ring.peek(id, view));
            view_equal(view, std::span(payload).first(kAppCap));
            expect(Result::kOk, ring.commit(view.lease));
        }
    }
    fw::FanoutRing small;
    expect(Result::kOk, small.init_typed(backing, 1U, fw::kNoLock));
    expect(Result::kInvalidArg, small.write(0U, payload)); // 超 max，但未超过容量也计 oversize。
    TEST_ASSERT_EQUAL_UINT32(1U, small.typed_writer_stats().oversize_drop);
    std::array<uint8_t, 4U> minimum{};
    fw::FanoutRing tiny;
    expect(Result::kOk, tiny.init_typed(minimum, 1U, fw::kNoLock));
    uint8_t id = 0U;
    expect(Result::kOk, tiny.attach(kType0, id));
    expect(Result::kOk, tiny.write(0U, kByte));
    expect(Result::kOk, tiny.write(0U, kByte));
    TEST_ASSERT_EQUAL_UINT32(1U, tiny.reader_stats(id).dropped);
}

void test_physical_and_u32_wrap_without_moving_pin()
{
    for (const uint32_t start : {uint32_t{kCap - 1U}, uint32_t{kCap - 4U}, kMaxCounter - 1U,
                                 kMaxCounter - 4U, kMaxCounter - 16U}) {
        fw::FanoutRing ring;
        std::array<uint8_t, kCap> storage{};
        expect(Result::kOk, ring.init_typed(storage, 8U, fw::kNoLock));
        uint8_t id = 0U;
        expect(Result::kOk, ring.attach(kType0, id));
        fw::FanoutRingTestAccess::empty_at(ring, start);
        std::array<uint8_t, 8U> payload{};
        pattern(payload);
        expect(Result::kOk, ring.write(0U, payload));
        fw::FanoutView view{};
        expect(Result::kOk, ring.peek(id, view));
        view_equal(view, payload);
        const auto *first_address = view.first.data();
        const auto *second_address = view.second.data();
        // 第一个租约仍存续时 head/oldest 可回绕，但不得搬动任何负载。
        expect(Result::kOk, ring.write(0U, payload));
        view_equal(view, payload);
        TEST_ASSERT_EQUAL_PTR(first_address, view.first.data());
        TEST_ASSERT_EQUAL_PTR(second_address, view.second.data());
        expect(Result::kBusy, ring.write(0U, payload));
        expect(Result::kOk, ring.commit(view.lease));
        expect(Result::kOk, ring.write(0U, payload));
        expect(Result::kOk, ring.peek(id, view));
        view_equal(view, payload);
        expect(Result::kOk, ring.commit(view.lease));
        expect(Result::kOk, ring.peek(id, view));
        // pin 留在较新记录，允许淘汰较旧记录；oldest 回绕也不能旋转 pin 负载。
        expect(Result::kOk, ring.write(0U, payload));
        view_equal(view, payload);
        expect(Result::kOk, ring.commit(view.lease));
        expect(Result::kOk, ring.peek(id, view));
        view_equal(view, payload);
        expect(Result::kOk, ring.commit(view.lease));
        expect(Result::kNotFound, ring.peek(id, view));
        TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(id).dropped);
    }
}

void test_serial_and_generation_exhaustion()
{
    fw::FanoutRing ring;
    std::array<uint8_t, kCap> storage{};
    expect(Result::kOk, ring.init_typed(storage, 1U, fw::kNoLock));
    uint8_t id = 0U;
    expect(Result::kOk, ring.attach(kType0, id));
    fw::FanoutRingTestAccess::serial(ring, id, kMaxCounter - 1U);
    fw::FanoutRingTestAccess::generation(ring, id, kMaxCounter - 1U);
    expect(Result::kOk, ring.write(0U, kByte));
    fw::FanoutView view{};
    expect(Result::kOk, ring.peek(id, view));
    const auto stale = view.lease;
    TEST_ASSERT_EQUAL_UINT32(kMaxCounter, stale.serial);
    expect(Result::kOk, ring.cancel(stale));
    expect(Result::kInvalidState, ring.peek(id, view));
    expect(Result::kOk, ring.detach(id));
    expect(Result::kOk, ring.attach(kType0, id));
    expect(Result::kOk, ring.write(0U, kByte));
    expect(Result::kOk, ring.peek(id, view));
    TEST_ASSERT_EQUAL_UINT32(kMaxCounter, view.lease.generation);
    expect(Result::kInvalidArg, ring.commit(stale));
    expect(Result::kOk, ring.commit(view.lease));
    expect(Result::kOk, ring.detach(id));
    expect(Result::kOk, ring.attach(kType0, id));
    TEST_ASSERT_EQUAL_UINT8(1U, id); // 第0槽已经永久退役。
    expect(Result::kOk, ring.detach(id));
    for (uint8_t slot = 0U; slot < fw::kFanoutMaxReaders; ++slot) {
        fw::FanoutRingTestAccess::generation(ring, slot, kMaxCounter);
    }
    expect(Result::kInvalidState, ring.attach(kType0, id));
}

// writer 在选定的 lock/unlock 边界停住；主线程此时仍可正常使用 ring_mutex。
struct ControlledLock {
    pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
    pthread_t writer{};
    size_t lock_calls = 0U;
    size_t pause_at = 0U;
    bool pause_before_lock = false;
    bool paused = false;
    bool resume = false;
    bool armed = false;

    ControlledLock() = default;
    ControlledLock(const ControlledLock &) = delete;
    ControlledLock &operator=(const ControlledLock &) = delete;
    ControlledLock(ControlledLock &&) = delete;
    ControlledLock &operator=(ControlledLock &&) = delete;

    ~ControlledLock() noexcept
    {
        sys_ok(pthread_cond_destroy(&gate_cond));
        sys_ok(pthread_mutex_destroy(&gate_mutex));
        sys_ok(pthread_mutex_destroy(&ring_mutex));
    }

    void pause() noexcept
    {
        sys_ok(pthread_mutex_lock(&gate_mutex));
        paused = true;
        sys_ok(pthread_cond_broadcast(&gate_cond));
        while (!resume) {
            sys_ok(pthread_cond_wait(&gate_cond, &gate_mutex));
        }
        sys_ok(pthread_mutex_unlock(&gate_mutex));
    }

    static void lock(void *ctx) noexcept
    {
        auto &self = *static_cast<ControlledLock *>(ctx); // ctx 仅绑定到本类型。
        if (self.armed && (pthread_equal(pthread_self(), self.writer) != 0)) {
            ++self.lock_calls;
            if (self.pause_before_lock && (self.lock_calls == self.pause_at)) {
                self.pause();
            }
        }
        sys_ok(pthread_mutex_lock(&self.ring_mutex));
    }

    static void unlock(void *ctx) noexcept
    {
        auto &self = *static_cast<ControlledLock *>(ctx); // ctx 仅绑定到本类型。
        sys_ok(pthread_mutex_unlock(&self.ring_mutex));
        if (self.armed && (pthread_equal(pthread_self(), self.writer) != 0) &&
            !self.pause_before_lock && (self.lock_calls == self.pause_at)) {
            self.pause();
        }
    }

    void await_pause() noexcept
    {
        sys_ok(pthread_mutex_lock(&gate_mutex));
        while (!paused) {
            sys_ok(pthread_cond_wait(&gate_cond, &gate_mutex));
        }
        sys_ok(pthread_mutex_unlock(&gate_mutex));
    }

    void release() noexcept
    {
        sys_ok(pthread_mutex_lock(&gate_mutex));
        resume = true;
        sys_ok(pthread_cond_broadcast(&gate_cond));
        sys_ok(pthread_mutex_unlock(&gate_mutex));
    }
};

struct Writer {
    fw::FanoutRing *ring = nullptr;
    ControlledLock *lock = nullptr;
    std::span<const uint8_t> payload;
    Result result = Result::kInvalidState;

    static void *run(void *ctx) noexcept
    {
        auto &self = *static_cast<Writer *>(ctx); // pthread 参数绑定到存活至 join 的 Writer。
        self.lock->writer = pthread_self();
        self.lock->armed = true;
        self.result = self.ring->write(0U, self.payload);
        return nullptr;
    }
};

void test_reservation_prevents_late_pin()
{
    ControlledLock lock;
    const fw::LockOps ops{ControlledLock::lock, ControlledLock::unlock, &lock};
    fw::FanoutRing ring;
    std::array<uint8_t, 16U> storage{};
    expect(Result::kOk, ring.init_typed(storage, kReservationPayload, ops));
    uint8_t id = 0U;
    expect(Result::kOk, ring.attach(kType0, id));
    for (size_t i = 0U; i < 4U; ++i) {
        expect(Result::kOk, ring.write(0U, kByte));
    }
    const std::array<uint8_t, 5U> payload{1U, 2U, 3U, 4U, 5U};
    lock.pause_at = 1U; // 保留区已发布，尚未淘汰旧记录。
    Writer writer{&ring, &lock, payload};
    pthread_t thread{};
    sys_ok(pthread_create(&thread, nullptr, Writer::run, &writer));
    lock.await_pause();
    fw::FanoutView view{};
    const Result peek_result = ring.peek(id, view);
    const uint32_t dropped = ring.reader_stats(id).dropped;
    lock.release();
    sys_ok(pthread_join(thread, nullptr));
    expect(Result::kOk, peek_result);
    expect(Result::kOk, writer.result);
    TEST_ASSERT_EQUAL_UINT32(2U, dropped); // peek 协助淘汰待覆盖前缀后才获取租约。
    view_equal(view, kByte);
    expect(Result::kOk, ring.commit(view.lease));
    expect(Result::kOk, ring.peek(id, view));
    view_equal(view, kByte);
    expect(Result::kOk, ring.commit(view.lease));
    expect(Result::kOk, ring.peek(id, view));
    view_equal(view, payload);
    expect(Result::kOk, ring.commit(view.lease));
}

void test_copy_before_publication_is_invisible()
{
    ControlledLock lock;
    const fw::LockOps ops{ControlledLock::lock, ControlledLock::unlock, &lock};
    fw::FanoutRing ring;
    std::array<uint8_t, kAppCap * 2U> storage{};
    expect(Result::kOk, ring.init_typed(storage, kAppCap, ops));
    uint8_t id = 0U;
    expect(Result::kOk, ring.attach(kType0, id));
    std::array<uint8_t, kAppCap> payload{};
    pattern(payload);
    lock.pause_at = kPublicationLock;
    lock.pause_before_lock = true; // 锁外复制已完成，最终 head 发布锁尚未获取。
    Writer writer{&ring, &lock, payload};
    pthread_t thread{};
    sys_ok(pthread_create(&thread, nullptr, Writer::run, &writer));
    lock.await_pause();
    fw::FanoutView view{};
    const Result peek_result = ring.peek(id, view);
    const uint32_t records = ring.writer_stats().records;
    const bool copied = std::equal(payload.begin(), payload.end(),
                                   std::span(storage).subspan(fw::kFanoutTypedHeaderBytes).begin());
    lock.release();
    sys_ok(pthread_join(thread, nullptr));
    expect(Result::kNotFound, peek_result);
    TEST_ASSERT_EQUAL_UINT32(0U, records);
    TEST_ASSERT_TRUE(copied);
    expect(Result::kOk, writer.result);
    expect(Result::kOk, ring.peek(id, view));
    view_equal(view, payload);
    expect(Result::kOk, ring.commit(view.lease));
}

void barrier(pthread_barrier_t &sync) noexcept
{
    const int rc = pthread_barrier_wait(&sync);
    if ((rc != 0) && (rc != PTHREAD_BARRIER_SERIAL_THREAD)) {
        std::abort();
    }
}

struct ConcurrentReader {
    fw::FanoutRing *ring = nullptr;
    pthread_barrier_t *sync = nullptr;
    uint8_t id = 0U;
    bool ok = true;

    static void *run(void *ctx) noexcept
    {
        auto &self = *static_cast<ConcurrentReader *>(ctx); // pthread 参数存活至 join。
        for (size_t i = 0U; i < kRounds; ++i) {
            barrier(*self.sync);
            fw::FanoutView view{};
            if (self.ring->peek(self.id, view) == Result::kOk) {
                self.ok = self.ok && (view.first.size() == 1U) && (view.first[0] == kByte[0]);
                self.ok = (self.ring->commit(view.lease) == Result::kOk) && self.ok;
            } else {
                self.ok = false;
            }
            const auto stats = self.ring->typed_reader_stats(self.id);
            self.ok = self.ok && (stats.base.records == i + 1U) && (stats.base.dropped == 0U) &&
                      (self.ring->typed_writer_stats().base.records == i + 1U);
            barrier(*self.sync);
        }
        return nullptr;
    }
};

void test_concurrent_reader_stats()
{
    ControlledLock lock;
    const fw::LockOps ops{ControlledLock::lock, ControlledLock::unlock, &lock};
    fw::FanoutRing ring;
    std::array<uint8_t, kCap> storage{};
    expect(Result::kOk, ring.init_typed(storage, 1U, ops));
    pthread_barrier_t sync{};
    sys_ok(pthread_barrier_init(&sync, nullptr, kBarrierParticipants));
    uint8_t a = 0U;
    uint8_t b = 0U;
    expect(Result::kOk, ring.attach(kType0, a));
    expect(Result::kOk, ring.attach(kType0, b));
    ConcurrentReader first{&ring, &sync, a};
    ConcurrentReader second{&ring, &sync, b};
    pthread_t ta{};
    pthread_t tb{};
    sys_ok(pthread_create(&ta, nullptr, ConcurrentReader::run, &first));
    sys_ok(pthread_create(&tb, nullptr, ConcurrentReader::run, &second));
    bool writes_ok = true;
    for (size_t i = 0U; i < kRounds; ++i) {
        writes_ok = (ring.write(0U, kByte) == Result::kOk) && writes_ok;
        barrier(sync);
        barrier(sync);
    }
    sys_ok(pthread_join(ta, nullptr));
    sys_ok(pthread_join(tb, nullptr));
    sys_ok(pthread_barrier_destroy(&sync));
    TEST_ASSERT_TRUE(writes_ok);
    TEST_ASSERT_TRUE(first.ok);
    TEST_ASSERT_TRUE(second.ok);
}

} // namespace

int main()
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) host 报告对象布局，格式与 size_t 匹配。
    std::printf("K1 sizeof FanoutRing=%zu Reader=%zu FanoutLease=%zu FanoutView=%zu\n",
                sizeof(fw::FanoutRing), fw::FanoutRingTestAccess::reader_size(),
                sizeof(fw::FanoutLease), sizeof(fw::FanoutView));
    UNITY_BEGIN();
    RUN_TEST(test_validation_and_modes);
    RUN_TEST(test_masks_and_independent_readers);
    RUN_TEST(test_overwrite_filters_and_progress);
    RUN_TEST(test_leases_and_pin_rejection);
    RUN_TEST(test_pin_rejection_does_not_partially_evict);
    RUN_TEST(test_capacity_and_oversize);
    RUN_TEST(test_physical_and_u32_wrap_without_moving_pin);
    RUN_TEST(test_serial_and_generation_exhaustion);
    RUN_TEST(test_reservation_prevents_late_pin);
    RUN_TEST(test_copy_before_publication_is_invisible);
    RUN_TEST(test_concurrent_reader_stats);
    return UNITY_END();
}
