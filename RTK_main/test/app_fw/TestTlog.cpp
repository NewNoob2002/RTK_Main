// fw::tlog / fw::LogRing host 单元测试（ADR-0009 §2.11）：install 校验与唯一性、类别掩码与十六进制掩码
// 两级开关、文本记录字段与尾部换行剥离、超长截断加 "..."、hex 记录 len/total、环满丢新、跨越存储末尾回绕、
// peekTime 次序、pop 复制截断、takeDropped 增量、注入锁成对使用、sink 适配器。

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <span>

#include "fw/Lock.hpp"
#include "fw/Result.hpp"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "fw/Trace.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr size_t kRingBytes =
    fw::kLogHeaderBytes + fw::kLogTextMax; // 172：恰好放 3 条 40 字节 payload 记录
// 全局环要能容纳一条最长 hex 记录（12 + 256 = 268），否则 tlogHex 的记录会被丢弃。
constexpr size_t kGlobalRingBytes = fw::kLogHeaderBytes + fw::kLogHexMax;
constexpr size_t kPayloadLen = 40U;
constexpr size_t kRecordBytes = fw::kLogHeaderBytes + kPayloadLen;
constexpr uint32_t kClockStep = 5U;
constexpr size_t kLongTextLen = 300U;
constexpr size_t kHexBytes = 300U;
constexpr size_t kRecordsFit = 3U;
constexpr uint32_t kBytesMarker = 72U;
constexpr uint32_t kTimestampMarker = 42U;

std::array<uint8_t, kGlobalRingBytes> s_storage{};
fw::LogRing s_ring{};
uint32_t s_clock_value = 0U;
uint32_t s_trace_calls = 0U;

uint32_t testClock() noexcept
{
    s_clock_value += kClockStep;
    return s_clock_value;
}

void traceSink(void * /*ctx*/, const fw::TraceEvent & /*ev*/) noexcept
{
    ++s_trace_calls;
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

// 每个用例开始前把全局环里的残留记录读空，保持用例之间互不影响。
void drainRing() noexcept
{
    fw::LogHeader hdr{};
    std::array<uint8_t, fw::kLogHexMax> out{};
    size_t copied = 0U;
    while (s_ring.pop(hdr, out, copied)) {
    }
}

std::array<uint8_t, kPayloadLen> makePayload(uint8_t seed) noexcept
{
    std::array<uint8_t, kPayloadLen> payload{};
    for (size_t i = 0U; i < payload.size(); ++i) {
        // seed 只用于让相邻用例的数据可区分，转换回 uint8_t 是显式的。
        payload[i] = static_cast<uint8_t>(seed + i);
    }
    return payload;
}

fw::LogHeader makeHeader(uint32_t tMs, fw::LogKind kind = fw::LogKind::kText) noexcept
{
    fw::LogHeader hdr{};
    hdr.tMs = tMs;
    hdr.cat = fw::TraceCat::kLink;
    hdr.kind = kind;
    // kPayloadLen（40）≤ uint16 上限，窄化安全。
    hdr.len = static_cast<uint16_t>(kPayloadLen);
    hdr.total = static_cast<uint32_t>(kPayloadLen);
    return hdr;
}

void testLogInstallValidationAndOnce()
{
    // 未安装 trace 时没有可用的时钟源，文本记录的时间戳无从取得。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(fw::logInstall(&fw::LogRing::sink, &s_ring)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(fw::logInstall(nullptr, &s_ring)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kOk),
        static_cast<uint8_t>(fw::traceInstall(&traceSink, nullptr, &testClock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(fw::logInstall(&fw::LogRing::sink, &s_ring)));
    // 重复安装：sink 与 ctx 只能安装一次，避免运行期两条通道写不同环。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(fw::logInstall(&fw::LogRing::sink, &s_ring)));
    // trace 通道本身没有被这些调用触发。
    TEST_ASSERT_EQUAL_UINT32(0U, s_trace_calls);
}

void testTlogRespectsCategoryMask()
{
    // 类别日志级别低于 I 时 tlog 连格式化都不做，环里不应出现记录（ADR-0009 §2.12：与追踪掩码无关）。
    fw::logSetCatLevel(fw::TraceCat::kGnss, fw::LogLevel::kWarn);
    // tlog 是冻结的 printf 风格接口（ADR-0009 §3），无法改成模板 vararg。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    fw::tlog(fw::TraceCat::kGnss, "should be dropped");
    TEST_ASSERT_EQUAL_UINT32(0U, s_ring.stats().pushed);

    fw::logSetCatLevel(fw::TraceCat::kGnss, fw::LogLevel::kDebug);
    // 类别关闭时不应触碰时钟：时间戳只在真正要写记录时才取。
    TEST_ASSERT_EQUAL_UINT32(0U, s_clock_value);
}

void testTlogTextRecordFields()
{
    drainRing();
    fw::traceSetMask(fw::traceBit(fw::TraceCat::kGnss));
    const uint32_t before = s_clock_value;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    fw::tlog(fw::TraceCat::kGnss, "NMEA %s, %u bytes\n", "GNGGA", kBytesMarker);

    fw::LogHeader hdr{};
    std::array<uint8_t, fw::kLogTextMax> out{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(s_ring.pop(hdr, out, copied));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::TraceCat::kGnss),
                            static_cast<uint8_t>(hdr.cat));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::LogKind::kText),
                            static_cast<uint8_t>(hdr.kind));
    TEST_ASSERT_EQUAL_UINT32(kClockStep, hdr.tMs - before);
    TEST_ASSERT_EQUAL_UINT32(hdr.len, hdr.total);
    // 期望文本与上面那次调用一一对应（kBytesMarker == 72）。
    constexpr std::string_view kExpected = "NMEA GNGGA, 72 bytes";
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kExpected.size()),
                             static_cast<uint32_t>(copied));
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(kExpected.data(), out.data(), kExpected.size()));
    TEST_ASSERT_EQUAL_UINT32(0U, s_ring.takeDropped());
}

void testTlogTruncatesWithEllipsis()
{
    drainRing();
    std::array<char, kLongTextLen + 1U> longFmt{};
    std::memset(longFmt.data(), 'x', kLongTextLen);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    fw::tlog(fw::TraceCat::kGnss, "%s", longFmt.data());

    fw::LogHeader hdr{};
    std::array<uint8_t, fw::kLogTextMax> out{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(s_ring.pop(hdr, out, copied));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fw::kLogTextMax),
                             static_cast<uint32_t>(hdr.len));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fw::kLogTextMax), static_cast<uint32_t>(copied));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fw::kLogTextMax), hdr.total);
    TEST_ASSERT_EQUAL_CHAR('.', static_cast<char>(out[copied - 1U]));
    TEST_ASSERT_EQUAL_CHAR('.', static_cast<char>(out[copied - 2U]));
    TEST_ASSERT_EQUAL_CHAR('.', static_cast<char>(out[copied - 3U]));
}

void testTlogHexTwoLevelMask()
{
    drainRing();
    std::array<uint8_t, kHexBytes> bytes{};
    for (size_t i = 0U; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(i);
    }
    // 级别一：类别 D 级、十六进制关 → 不写记录。
    TEST_ASSERT_TRUE(fw::logEnabled(fw::LogLevel::kDebug, fw::TraceCat::kGnss));
    TEST_ASSERT_FALSE(fw::logHexOn(fw::TraceCat::kGnss));
    const uint32_t pushedBefore = s_ring.stats().pushed;
    fw::tlogHex(fw::TraceCat::kGnss, bytes);
    TEST_ASSERT_EQUAL_UINT32(pushedBefore, s_ring.stats().pushed);

    // 级别二：十六进制掩码打开 → 写记录，只保留前 kLogHexMax 字节，total 保留原始长度。
    fw::logSetHexMask(fw::traceBit(fw::TraceCat::kGnss) | 0x80000000U);
    TEST_ASSERT_TRUE(fw::logHexOn(fw::TraceCat::kGnss));
    // 越界位被清掉。
    TEST_ASSERT_EQUAL_UINT32(fw::traceBit(fw::TraceCat::kGnss), fw::logHexMask());
    fw::tlogHex(fw::TraceCat::kGnss, bytes);

    fw::LogHeader hdr{};
    std::array<uint8_t, fw::kLogHexMax> out{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(s_ring.pop(hdr, out, copied));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::LogKind::kHex),
                            static_cast<uint8_t>(hdr.kind));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fw::kLogHexMax), static_cast<uint32_t>(hdr.len));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kHexBytes), hdr.total);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fw::kLogHexMax), static_cast<uint32_t>(copied));
    TEST_ASSERT_EQUAL_UINT8(0U, out[0]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::kLogHexMax - 1U), out[copied - 1U]);

    // 类别降到 I 级后即使十六进制掩码还在，也不再转储。
    fw::logSetCatLevel(fw::TraceCat::kGnss, fw::LogLevel::kInfo);
    fw::tlogHex(fw::TraceCat::kGnss, bytes);
    TEST_ASSERT_EQUAL_UINT32(pushedBefore + 1U, s_ring.stats().pushed);
    // 越界类别不得崩溃。
    TEST_ASSERT_FALSE(fw::logHexOn(fw::TraceCat::kCount));
    fw::logSetHexMask(0U);
}

void testLogRingInitValidation()
{
    std::array<uint8_t, kRingBytes> storage{};
    fw::LogRing ring{};
    std::array<uint8_t, fw::kLogHeaderBytes> tiny{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(std::span<uint8_t>{}, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(tiny, fw::kNoLock)));
    fw::LockOps half{};
    half.lock = &spyLock;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(storage, half)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(ring.used()));
    // 未 init 的环一律返回 false / 0，不写调用方内存。
    fw::LogRing fresh{};
    const auto payload = makePayload(1U);
    TEST_ASSERT_FALSE(fresh.push(makeHeader(1U), payload));
    uint32_t tMs = 0U;
    TEST_ASSERT_FALSE(fresh.peekTime(tMs));
}

void testLogRingPushValidation()
{
    fw::LogRing ring{};
    static std::array<uint8_t, kRingBytes> storage{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    const auto payload = makePayload(2U);
    // payload 长度与 hdr.len 不一致：拒绝，且不计 dropped（调用方 bug 不是拥塞）。
    fw::LogHeader bad = makeHeader(1U);
    bad.len = static_cast<uint16_t>(kPayloadLen - 1U);
    TEST_ASSERT_FALSE(ring.push(bad, payload));
    TEST_ASSERT_EQUAL_UINT32(0U, ring.stats().dropped);
    // 空 ctx 的 sink 什么也不做，不得崩溃。
    fw::LogRing::sink(nullptr, makeHeader(1U), payload);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.stats().pushed);
}

void testLogRingFullDropsNewAndKeepsOrder()
{
    fw::LogRing ring{};
    static std::array<uint8_t, kRingBytes> storage{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    // 172 字节恰好放 3 条 52 字节记录，第 4 条丢新。
    for (uint32_t i = 0U; i < kRecordsFit; ++i) {
        const auto payload = makePayload(static_cast<uint8_t>(i));
        TEST_ASSERT_TRUE(ring.push(makeHeader(i + 1U), payload));
    }
    const auto payload = makePayload(9U);
    TEST_ASSERT_FALSE(ring.push(makeHeader(99U), payload));
    TEST_ASSERT_EQUAL_UINT32(1U, ring.stats().dropped);
    TEST_ASSERT_EQUAL_UINT32(1U, ring.takeDropped());
    TEST_ASSERT_EQUAL_UINT32(0U, ring.takeDropped());
    // 取出顺序 = 压入顺序，丢的是新记录。
    fw::LogHeader hdr{};
    std::array<uint8_t, kPayloadLen> out{};
    size_t copied = 0U;
    for (uint32_t i = 0U; i < kRecordsFit; ++i) {
        TEST_ASSERT_TRUE(ring.pop(hdr, out, copied));
        TEST_ASSERT_EQUAL_UINT32(i + 1U, hdr.tMs);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kPayloadLen), static_cast<uint32_t>(copied));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(i), out[0]);
    }
    TEST_ASSERT_FALSE(ring.pop(hdr, out, copied));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kRecordsFit * kRecordBytes),
                             ring.stats().maxUsed);
}

void testLogRingWrapsAcrossEnd()
{
    fw::LogRing ring{};
    static std::array<uint8_t, kRingBytes> storage{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    // 压满 3 条后弹 1 条：head=156、tail=52，下一条会从 156 写 16 字节、再从 0 写 36 字节。
    const std::array<uint8_t, kRecordsFit> seeds{1U, 2U, 7U};
    for (uint32_t i = 0U; i < kRecordsFit; ++i) {
        const auto payload = makePayload(static_cast<uint8_t>(i));
        TEST_ASSERT_TRUE(ring.push(makeHeader(i + 1U), payload));
    }
    fw::LogHeader hdr{};
    std::array<uint8_t, kPayloadLen> out{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(ring.pop(hdr, out, copied));
    TEST_ASSERT_EQUAL_UINT32(1U, hdr.tMs);

    const auto wrapped = makePayload(7U);
    TEST_ASSERT_TRUE(ring.push(makeHeader(4U), wrapped));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kRecordsFit * kRecordBytes),
                             static_cast<uint32_t>(ring.used()));
    for (uint32_t i = 0U; i < kRecordsFit; ++i) {
        TEST_ASSERT_TRUE(ring.pop(hdr, out, copied));
        TEST_ASSERT_EQUAL_UINT32(i + 2U, hdr.tMs);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kPayloadLen), static_cast<uint32_t>(copied));
        TEST_ASSERT_EQUAL_UINT8(seeds[i], out[0]);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(seeds[i] + kPayloadLen - 1U),
                                out[copied - 1U]);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(ring.used()));
}

void testLogRingPeekTimeAndTruncatedPop()
{
    fw::LogRing ring{};
    static std::array<uint8_t, kRingBytes> storage{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));
    uint32_t tMs = 0U;
    TEST_ASSERT_FALSE(ring.peekTime(tMs));

    const auto payload = makePayload(4U);
    TEST_ASSERT_TRUE(ring.push(makeHeader(500U), payload));
    TEST_ASSERT_TRUE(ring.push(makeHeader(600U), payload));
    TEST_ASSERT_TRUE(ring.peekTime(tMs));
    TEST_ASSERT_EQUAL_UINT32(500U, tMs);

    // out 缓冲不足时截断复制，但记录整体移除，避免读者卡在一条超长记录上。
    fw::LogHeader hdr{};
    std::array<uint8_t, 4U> small{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(ring.pop(hdr, small, copied));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kPayloadLen), static_cast<uint32_t>(hdr.len));
    TEST_ASSERT_EQUAL_UINT32(4U, static_cast<uint32_t>(copied));
    TEST_ASSERT_TRUE(ring.peekTime(tMs));
    TEST_ASSERT_EQUAL_UINT32(600U, tMs);
}

void testLogRingUsesInjectedLock()
{
    RingLockSpy spy{};
    fw::LockOps ops{};
    ops.ctx = &spy;
    ops.lock = &spyLock;
    ops.unlock = &spyUnlock;
    static std::array<uint8_t, kRingBytes> storage{};
    fw::LogRing ring{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, ops)));
    const auto payload = makePayload(5U);
    TEST_ASSERT_TRUE(ring.push(makeHeader(1U), payload));
    uint32_t tMs = 0U;
    TEST_ASSERT_TRUE(ring.peekTime(tMs));
    fw::LogHeader hdr{};
    std::array<uint8_t, kPayloadLen> out{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(ring.pop(hdr, out, copied));
    (void)ring.takeDropped();
    (void)ring.stats();
    (void)ring.used();
    TEST_ASSERT_EQUAL_UINT32(spy.lockCalls, spy.unlockCalls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_FALSE(spy.underflow);
}

void testLogRingSinkReceivesInstalledContext()
{
    drainRing();
    std::array<uint8_t, 8U> payload{};
    for (size_t i = 0U; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i + 1U);
    }
    fw::LogHeader hdr{};
    hdr.tMs = kTimestampMarker;
    hdr.cat = fw::TraceCat::kStore;
    hdr.kind = fw::LogKind::kHex;
    // payload.size()（8）≤ uint16 上限，窄化安全。
    hdr.len = static_cast<uint16_t>(payload.size());
    hdr.total = static_cast<uint32_t>(payload.size());
    fw::LogRing::sink(&s_ring, hdr, payload);

    fw::LogHeader got{};
    std::array<uint8_t, 8U> out{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(s_ring.pop(got, out, copied));
    TEST_ASSERT_EQUAL_UINT32(kTimestampMarker, got.tMs);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::LogKind::kHex),
                            static_cast<uint8_t>(got.kind));
    TEST_ASSERT_EQUAL_UINT32(8U, static_cast<uint32_t>(copied));
    TEST_ASSERT_EQUAL_UINT8(8U, out[7]);
}

} // namespace

int main(void)
{
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(s_ring.init(s_storage, fw::kNoLock)));
    UNITY_BEGIN();
    RUN_TEST(testLogInstallValidationAndOnce);
    RUN_TEST(testTlogRespectsCategoryMask);
    RUN_TEST(testTlogTextRecordFields);
    RUN_TEST(testTlogTruncatesWithEllipsis);
    RUN_TEST(testTlogHexTwoLevelMask);
    RUN_TEST(testLogRingInitValidation);
    RUN_TEST(testLogRingPushValidation);
    RUN_TEST(testLogRingFullDropsNewAndKeepsOrder);
    RUN_TEST(testLogRingWrapsAcrossEnd);
    RUN_TEST(testLogRingPeekTimeAndTruncatedPop);
    RUN_TEST(testLogRingUsesInjectedLock);
    RUN_TEST(testLogRingSinkReceivesInstalledContext);
    return UNITY_END();
}
