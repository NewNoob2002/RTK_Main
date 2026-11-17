// fw::trace / fw::TraceRing host 单元测试（ADR-0009 §2.1-§2.2）：安装校验与幂等、
// 类别掩码开关与越界位、事件字段与时钟、环满丢新保持顺序、wrap-around、丢弃增量、
// depth/maxDepth 统计、注入锁的成对使用、sink 适配器。

#include <array>
#include <cstddef>
#include <cstdint>

#include "fw/Lock.hpp"
#include "fw/Result.hpp"
#include "fw/Trace.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr size_t kRingCap = 4U;
constexpr size_t kTinyCap = 1U;
constexpr uint32_t kSeedCode = 1U;
constexpr uint32_t kWrapRounds = 3U;
constexpr uint32_t kWrapCodeBase = 10U;
constexpr uint16_t kSessionMarker = 0x1234U;
constexpr uint8_t kCodeMarker = 42U;
constexpr uint32_t kParamA = 7U;
constexpr uint32_t kParamB = 9U;
constexpr uint32_t kClockStep = 100U;
constexpr uint32_t kAllBitsMask = 0xFFFFFFFFU;

// 最近一次 sink 收到的事件与调用次数；sink 由 traceInstall 安装一次，ctx 不使用。
uint32_t s_sink_calls = 0U;
fw::TraceEvent s_last_event{};
uint32_t s_clock_calls = 0U;
uint32_t s_clock_value = 0U;

uint32_t testClock() noexcept
{
    ++s_clock_calls;
    s_clock_value += kClockStep;
    return s_clock_value;
}

void testSink(void * /*ctx*/, const fw::TraceEvent &ev) noexcept
{
    ++s_sink_calls;
    s_last_event = ev;
}

struct RingLockSpy {
    uint32_t lockCalls = 0U;
    uint32_t unlockCalls = 0U;
    int32_t depth = 0;
    bool underflow = false;
};

void spyLock(void *ctx) noexcept
{
    auto *spy = static_cast<RingLockSpy *>(ctx);
    ++spy->lockCalls;
    ++spy->depth;
}

void spyUnlock(void *ctx) noexcept
{
    auto *spy = static_cast<RingLockSpy *>(ctx);
    ++spy->unlockCalls;
    --spy->depth;
    if (spy->depth < 0) {
        spy->underflow = true;
    }
}

fw::TraceEvent makeEvent(uint8_t code, uint16_t session = 0U) noexcept
{
    fw::TraceEvent ev{};
    ev.tMs = static_cast<uint32_t>(code);
    ev.cat = fw::TraceCat::kLink;
    ev.code = code;
    ev.session = session;
    ev.a = code;
    ev.b = static_cast<uint32_t>(code) * 2U;
    return ev;
}

void testTraceNoopBeforeInstall()
{
    s_sink_calls = 0U;
    s_clock_calls = 0U;
    fw::traceSetMask(fw::kTraceAllMask);
    fw::trace(fw::TraceCat::kLink, 1U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_sink_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_clock_calls);
}

void testTraceInstallValidationAndOnce()
{
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(fw::traceInstall(nullptr, nullptr, testClock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(fw::traceInstall(testSink, nullptr, nullptr)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(fw::traceInstall(testSink, nullptr, testClock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(fw::traceInstall(testSink, nullptr, testClock)));
}

void testTraceMaskSwitch()
{
    fw::traceSetMask(0U);
    TEST_ASSERT_EQUAL_UINT32(0U, fw::traceMask());
    TEST_ASSERT_FALSE(fw::traceOn(fw::TraceCat::kLink));
    s_sink_calls = 0U;
    s_clock_calls = 0U;
    fw::trace(fw::TraceCat::kLink, 1U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_sink_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_clock_calls);

    // 越界位被清零：掩码等于可用类别全集。
    fw::traceSetMask(kAllBitsMask);
    TEST_ASSERT_EQUAL_UINT32(fw::kTraceAllMask, fw::traceMask());

    fw::traceSetMask(fw::traceBit(fw::TraceCat::kStore));
    TEST_ASSERT_TRUE(fw::traceOn(fw::TraceCat::kStore));
    TEST_ASSERT_FALSE(fw::traceOn(fw::TraceCat::kLink));
    // 越界类别（>= kCount）不能打开，也不产生事件。
    TEST_ASSERT_FALSE(fw::traceOn(fw::TraceCat::kCount));
    s_clock_calls = 0U;
    fw::trace(fw::TraceCat::kCount, 1U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_clock_calls);
}

void testTraceEventFieldsAndClock()
{
    fw::traceSetMask(fw::kTraceAllMask);
    s_sink_calls = 0U;
    s_clock_calls = 0U;
    const uint32_t before = s_clock_value;
    fw::trace(fw::TraceCat::kRtcm, kCodeMarker, kSessionMarker, kParamA, kParamB);
    TEST_ASSERT_EQUAL_UINT32(1U, s_sink_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_clock_calls);
    TEST_ASSERT_EQUAL_UINT32(before + kClockStep, s_last_event.tMs);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::TraceCat::kRtcm),
                            static_cast<uint8_t>(s_last_event.cat));
    TEST_ASSERT_EQUAL_UINT8(kCodeMarker, s_last_event.code);
    TEST_ASSERT_EQUAL_UINT16(kSessionMarker, s_last_event.session);
    TEST_ASSERT_EQUAL_UINT32(kParamA, s_last_event.a);
    TEST_ASSERT_EQUAL_UINT32(kParamB, s_last_event.b);
}

void testTraceRingInitValidation()
{
    static std::array<fw::TraceEvent, kRingCap> storage{};
    static std::array<fw::TraceEvent, kTinyCap> tiny{};
    fw::TraceRing ring{};
    const fw::TraceEvent ev = makeEvent(1U);

    // 未 init：所有访问都应安全返回。
    TEST_ASSERT_FALSE(ring.push(ev));
    fw::TraceEvent out{};
    TEST_ASSERT_FALSE(ring.pop(out));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.takeDropped());
    TEST_ASSERT_EQUAL_UINT32(0U, ring.stats().pushed);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.depth());

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kInvalidArg),
        static_cast<uint8_t>(ring.init(std::span<fw::TraceEvent>{}, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(tiny, fw::kNoLock)));
    const fw::LockOps halfLock{spyLock, nullptr, nullptr};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(storage, halfLock)));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.depth());
}

void testTraceRingFullDropsNewAndKeepsOrder()
{
    static std::array<fw::TraceEvent, kRingCap> storage{};
    fw::TraceRing ring{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));

    for (uint8_t i = 0U; i < static_cast<uint8_t>(kRingCap); ++i) {
        TEST_ASSERT_TRUE(ring.push(makeEvent(static_cast<uint8_t>(kSeedCode + i))));
    }
    // 环满丢新：新事件被拒绝，已有事件不受影响。
    TEST_ASSERT_FALSE(ring.push(makeEvent(99U)));
    TEST_ASSERT_FALSE(ring.push(makeEvent(100U)));
    TEST_ASSERT_EQUAL_UINT32(kRingCap, ring.depth());
    const fw::TraceRingStats st = ring.stats();
    TEST_ASSERT_EQUAL_UINT32(kRingCap, st.pushed);
    TEST_ASSERT_EQUAL_UINT32(2U, st.dropped);
    TEST_ASSERT_EQUAL_UINT32(kRingCap, st.maxDepth);

    // 丢弃增量语义：读一次清零，再读为 0，新增丢弃再累加。
    TEST_ASSERT_EQUAL_UINT32(2U, ring.takeDropped());
    TEST_ASSERT_EQUAL_UINT32(0U, ring.takeDropped());
    TEST_ASSERT_FALSE(ring.push(makeEvent(101U)));
    TEST_ASSERT_EQUAL_UINT32(1U, ring.takeDropped());

    fw::TraceEvent out{};
    for (uint8_t i = 0U; i < static_cast<uint8_t>(kRingCap); ++i) {
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kSeedCode + i), out.code);
    }
    TEST_ASSERT_FALSE(ring.pop(out));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.depth());
}

void testTraceRingWrapAround()
{
    static std::array<fw::TraceEvent, kRingCap> storage{};
    fw::TraceRing ring{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));

    fw::TraceEvent out{};
    for (uint32_t round = 0U; round < kWrapRounds; ++round) {
        for (size_t i = 0U; i < kRingCap; ++i) {
            // round < 3、i < 4 → 码值 < 32，转 uint8_t 无损。
            const uint32_t code = kWrapCodeBase * round + static_cast<uint32_t>(i);
            TEST_ASSERT_TRUE(ring.push(makeEvent(static_cast<uint8_t>(code))));
        }
        for (size_t i = 0U; i < kRingCap; ++i) {
            TEST_ASSERT_TRUE(ring.pop(out));
            // 同上，码值 < 32，转 uint8_t 无损。
            const uint32_t code = kWrapCodeBase * round + static_cast<uint32_t>(i);
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(code), out.code);
        }
        TEST_ASSERT_EQUAL_UINT32(0U, ring.depth());
    }
    const fw::TraceRingStats st = ring.stats();
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kWrapRounds * kRingCap), st.pushed);
    TEST_ASSERT_EQUAL_UINT32(0U, st.dropped);
    TEST_ASSERT_EQUAL_UINT32(kRingCap, st.maxDepth);
}

void testTraceRingPartialDrainDepth()
{
    static std::array<fw::TraceEvent, kRingCap> storage{};
    fw::TraceRing ring{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));

    TEST_ASSERT_TRUE(ring.push(makeEvent(1U)));
    TEST_ASSERT_TRUE(ring.push(makeEvent(2U)));
    TEST_ASSERT_TRUE(ring.push(makeEvent(3U)));
    TEST_ASSERT_EQUAL_UINT32(3U, ring.depth());

    fw::TraceEvent out{};
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL_UINT8(1U, out.code);
    TEST_ASSERT_EQUAL_UINT32(2U, ring.depth());
    // maxDepth 是历史峰值，不随出队下降。
    TEST_ASSERT_EQUAL_UINT32(3U, ring.stats().maxDepth);
    TEST_ASSERT_EQUAL_UINT32(3U, ring.stats().pushed);
}

void testTraceRingUsesInjectedLock()
{
    static std::array<fw::TraceEvent, kRingCap> storage{};
    RingLockSpy spy{};
    const fw::LockOps ops{spyLock, spyUnlock, &spy};
    fw::TraceRing ring{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, ops)));

    TEST_ASSERT_TRUE(ring.push(makeEvent(1U)));
    fw::TraceEvent out{};
    TEST_ASSERT_TRUE(ring.pop(out));
    (void)ring.takeDropped();
    (void)ring.depth();
    (void)ring.stats();

    TEST_ASSERT_GREATER_THAN_UINT32(0U, spy.lockCalls);
    TEST_ASSERT_EQUAL_UINT32(spy.lockCalls, spy.unlockCalls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_FALSE(spy.underflow);
}

void testTraceRingSinkAdapter()
{
    static std::array<fw::TraceEvent, kRingCap> storage{};
    fw::TraceRing ring{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));

    const fw::TraceEvent ev = makeEvent(7U);
    fw::TraceRing::sink(&ring, ev);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.depth());
    fw::TraceEvent out{};
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL_UINT8(7U, out.code);
    // 空 ctx 不做任何事，不得崩溃。
    fw::TraceRing::sink(nullptr, ev);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.depth());
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(testTraceNoopBeforeInstall);
    RUN_TEST(testTraceInstallValidationAndOnce);
    RUN_TEST(testTraceMaskSwitch);
    RUN_TEST(testTraceEventFieldsAndClock);
    RUN_TEST(testTraceRingInitValidation);
    RUN_TEST(testTraceRingFullDropsNewAndKeepsOrder);
    RUN_TEST(testTraceRingWrapAround);
    RUN_TEST(testTraceRingPartialDrainDepth);
    RUN_TEST(testTraceRingUsesInjectedLock);
    RUN_TEST(testTraceRingSinkAdapter);
    return UNITY_END();
}
