// fw::FanoutRing host 单元测试（ADR-0007 §3.6-1）：单/多读者、覆盖丢弃计数、
// attach 晚于写入、detach 复用、记录长度边界、kNoSpace 不消费、u32 位置回绕（含非 2 的幂容量）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/FanoutRing.hpp"
#include "fw/Lock.hpp"
#include "fw/Result.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr size_t kSmallMaxRecord = 16U;
constexpr size_t kSmallRingCap = 40U;
constexpr size_t kWrapMaxRecord = 65535U;
// 故意使用非 2 的幂的最小合法容量 2 * (65535 + 2) = 131074，以验证非 2 的幂在 u32 回绕时的对齐正确性。
constexpr size_t kWrapRingCapNonPow2 = 2U * (kWrapMaxRecord + fw::kFanoutHeaderBytes);
constexpr size_t kWrapPow2Cap = 262144U;
constexpr uint8_t kInvalidReaderId = 4U;
constexpr size_t kTooSmallCap = 5U;
constexpr size_t kMinValidCap = 6U;
constexpr size_t kOddRecordLen = 9U;
constexpr size_t kSentinelLen = 99U;
constexpr uint8_t kSentinelId = 99U;
constexpr uint8_t kOversizeFill = 0x5AU;
constexpr uint32_t kOddSeqBase = 10U;
constexpr uint32_t kOddRounds = 3U;
constexpr uint32_t kMidReadStop = 3U;
constexpr uint32_t kFirstSurvivingSeq = 6U;
constexpr uint32_t kWrapTailXor = 0xA5U;
constexpr uint32_t kHighByteShift = 8U;
constexpr uint32_t kByteMask = 0xFFU;

struct RingLockSpy {
    uint32_t lockCalls = 0U;
    uint32_t unlockCalls = 0U;
    int32_t depth = 0;
    bool underflow = false;
};

void ringSpyLock(void *ctx) noexcept
{
    auto *spy = static_cast<RingLockSpy *>(ctx);
    ++spy->lockCalls;
    ++spy->depth;
}

void ringSpyUnlock(void *ctx) noexcept
{
    auto *spy = static_cast<RingLockSpy *>(ctx);
    ++spy->unlockCalls;
    --spy->depth;
    if (spy->depth < 0) {
        spy->underflow = true;
    }
}

void fillPattern(std::span<uint8_t> dst, uint32_t seq) noexcept
{
    for (size_t i = 0U; i < dst.size(); ++i) {
        dst[i] = static_cast<uint8_t>((seq + static_cast<uint32_t>(i)) &
                                      kByteMask); // 取低 8 位生成可验证图案
    }
}

void verifyPattern(std::span<const uint8_t> data, uint32_t seq) noexcept
{
    for (size_t i = 0U; i < data.size(); ++i) {
        const uint8_t expected = static_cast<uint8_t>((seq + static_cast<uint32_t>(i)) & kByteMask);
        TEST_ASSERT_EQUAL_UINT8(expected, data[i]);
    }
}

void testInitValidationAndUninitCalls()
{
    fw::FanoutRing ring;
    std::array<uint8_t, kSmallRingCap> storage{};
    std::array<uint8_t, kSmallMaxRecord> out{};
    const std::array<uint8_t, 3> sample{1U, 2U, 3U};
    uint8_t rid = 0U;
    size_t len = kSentinelLen;

    // 未 init 前所有操作返回 kInvalidState
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(ring.attach(rid)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(ring.detach(0U)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(ring.write(sample)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(ring.read(0U, out, len)));
    TEST_ASSERT_EQUAL_size_t(0U, len);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.writerStats().records);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.readerStats(0U).records);

    // 非法 init 参数
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(std::span<uint8_t>{}, 1U, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(storage, 0U, fw::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kInvalidArg),
        static_cast<uint8_t>(ring.init(storage, kWrapMaxRecord + 1U, fw::kNoLock)));
    // storage.size() < 2 * (maxRecord + 2)
    std::array<uint8_t, kTooSmallCap> tooSmall{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(tooSmall, 1U, fw::kNoLock)));
    // 半空 LockOps 非法
    const fw::LockOps badLock{&ringSpyLock, nullptr, nullptr};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(storage, 1U, badLock)));

    // 最小边界 maxRecord=1, size=6 合法
    std::array<uint8_t, kMinValidCap> minBuf{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(minBuf, 1U, fw::kNoLock)));

    // 重复 init 返回 kInvalidState
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, fw::kNoLock)));
}

void testAttachDetachAndLateAttach()
{
    std::array<uint8_t, kSmallRingCap> storage{};
    RingLockSpy spy{};
    const fw::LockOps ops{&ringSpyLock, &ringSpyUnlock, &spy};
    fw::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, ops)));

    // 先写入 2 条记录再 attach：新读者不得回放历史
    const std::array<uint8_t, 4> pre1{0x10U, 0x11U, 0x12U, 0x13U};
    const std::array<uint8_t, 4> pre2{0x20U, 0x21U, 0x22U, 0x23U};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.write(pre1)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.write(pre2)));

    std::array<uint8_t, fw::kFanoutMaxReaders> ids{};
    for (size_t i = 0U; i < fw::kFanoutMaxReaders; ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.attach(ids[i])));
        TEST_ASSERT_TRUE(ids[i] < fw::kFanoutMaxReaders);
    }
    uint8_t extra = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kNoSpace),
                            static_cast<uint8_t>(ring.attach(extra)));

    // 晚于 pre1/pre2 attach 的读者当前应读不到历史数据
    std::array<uint8_t, kSmallMaxRecord> out{};
    size_t len = kSentinelLen;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(ids[0], out, len)));
    TEST_ASSERT_EQUAL_size_t(0U, len);

    // 写入新记录 post1：所有已 attach 读者都能读到 post1
    const std::array<uint8_t, 5> post1{0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.write(post1)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.read(ids[0], out, len)));
    TEST_ASSERT_EQUAL_size_t(post1.size(), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(post1.data(), out.data(), post1.size());

    // detach 与无效 id 检查，以及复用 id 后统计清零且不回放历史
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.detach(kInvalidReaderId)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.detach(ids[0])));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.detach(ids[0])));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.read(ids[0], out, len)));

    uint8_t reused = kSentinelId;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.attach(reused)));
    TEST_ASSERT_EQUAL_UINT8(ids[0], reused);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.readerStats(reused).records);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.readerStats(reused).bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.readerStats(reused).dropped);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(reused, out, len)));

    // 另一位一直在线的读者 ids[1] 仍能读到 post1
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.read(ids[1], out, len)));
    TEST_ASSERT_EQUAL_size_t(post1.size(), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(post1.data(), out.data(), post1.size());

    TEST_ASSERT_EQUAL_UINT32(spy.lockCalls, spy.unlockCalls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_FALSE(spy.underflow);
}

void testRecordBoundariesAndNospaceNonConsuming()
{
    std::array<uint8_t, kSmallRingCap> storage{};
    fw::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, fw::kNoLock)));

    uint8_t rid = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.attach(rid)));

    // 长度 0 与 > maxRecord 被拒绝并计入 rejected
    std::array<uint8_t, kSmallMaxRecord + 1U> oversized{};
    oversized.fill(kOversizeFill);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.write(std::span<const uint8_t>{})));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.write(oversized)));
    TEST_ASSERT_EQUAL_UINT32(2U, ring.writerStats().rejected);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.writerStats().records);

    // 写入长度 1 与 maxRecord 边界记录（同时跨过缓冲区物理末尾）
    std::array<uint8_t, kSmallMaxRecord> maxRec{};
    fillPattern(maxRec, 1U);
    const std::array<uint8_t, 1> minRec{0xFEU};
    // 连续写 4 次，促使写指针多次跨越 storage 末尾（40 B）并包含 2 B 长度头跨尾的情况
    for (uint32_t round = 0U; round < kOddRounds; ++round) {
        std::array<uint8_t, kOddRecordLen> oddRec{};
        fillPattern(oddRec, round + kOddSeqBase);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.write(oddRec)));
        std::array<uint8_t, kSmallMaxRecord> got{};
        size_t len = 0U;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.read(rid, got, len)));
        TEST_ASSERT_EQUAL_size_t(oddRec.size(), len);
        verifyPattern(std::span<const uint8_t>(got.data(), len), round + kOddSeqBase);
    }

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.write(maxRec)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.write(minRec)));

    // 输出缓冲不足 -> kNoSpace，len = 需要的长度，尾指针不动
    std::array<uint8_t, kSmallMaxRecord - 1U> smallOut{};
    size_t len = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kNoSpace),
                            static_cast<uint8_t>(ring.read(rid, smallOut, len)));
    TEST_ASSERT_EQUAL_size_t(kSmallMaxRecord, len);
    TEST_ASSERT_EQUAL_UINT32(3U, ring.readerStats(rid).records);

    // 换足够大的缓冲立即再次读：完整读出刚才那条 maxRec，然后读出 minRec
    std::array<uint8_t, kSmallMaxRecord> fullOut{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.read(rid, fullOut, len)));
    TEST_ASSERT_EQUAL_size_t(kSmallMaxRecord, len);
    verifyPattern(std::span<const uint8_t>(fullOut.data(), len), 1U);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.read(rid, fullOut, len)));
    TEST_ASSERT_EQUAL_size_t(1U, len);
    TEST_ASSERT_EQUAL_UINT8(0xFEU, fullOut[0]);
    TEST_ASSERT_EQUAL_UINT32(5U, ring.readerStats(rid).records);
}

void testMultiReaderOverwriteDropsWholeRecords()
{
    // cap = 40，每条记录 8 B 负载 + 2 B 头 = 10 B，容量正好放 4 条记录
    constexpr size_t kRecPayload = 8U;
    constexpr uint32_t kTotalWrites = 10U;
    std::array<uint8_t, kSmallRingCap> storage{};
    fw::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, fw::kNoLock)));

    uint8_t rFast = 0U;
    uint8_t rMid = 0U;
    uint8_t rSlow = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.attach(rFast)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.attach(rMid)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.attach(rSlow)));

    std::array<uint8_t, kSmallMaxRecord> out{};
    size_t len = 0U;

    for (uint32_t seq = 0U; seq < kTotalWrites; ++seq) {
        std::array<uint8_t, kRecPayload> rec{};
        fillPattern(rec, seq);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.write(rec)));

        // fast 读者每写一条读一条：0 丢弃
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.read(rFast, out, len)));
        TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
        verifyPattern(std::span<const uint8_t>(out.data(), len), seq);

        // mid 读者在前半程读了 0,1,2，之后停下直到结束（丢 3,4,5，剩 6,7,8,9）
        if (seq < kMidReadStop) {
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                    static_cast<uint8_t>(ring.read(rMid, out, len)));
            verifyPattern(std::span<const uint8_t>(out.data(), len), seq);
        }
    }

    const fw::FanoutWriterStats ws = ring.writerStats();
    TEST_ASSERT_EQUAL_UINT32(kTotalWrites, ws.records);
    TEST_ASSERT_EQUAL_UINT32(kTotalWrites * static_cast<uint32_t>(kRecPayload), ws.bytes);
    TEST_ASSERT_EQUAL_UINT32(kFirstSurvivingSeq, ws.overwritten);

    const fw::FanoutReaderStats fs = ring.readerStats(rFast);
    TEST_ASSERT_EQUAL_UINT32(kTotalWrites, fs.records);
    TEST_ASSERT_EQUAL_UINT32(0U, fs.dropped);

    // mid 读者先用过小缓冲读：返回 kNoSpace 且不消费、不重复累加 dropped
    std::array<uint8_t, 1> tinyOut{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kNoSpace),
                            static_cast<uint8_t>(ring.read(rMid, tinyOut, len)));
    TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
    TEST_ASSERT_EQUAL_UINT32(kMidReadStop, ring.readerStats(rMid).dropped);

    // mid 读者跳过被覆盖的 3..5（dropped == 3），整条读出 6..9
    for (uint32_t expectedSeq = kFirstSurvivingSeq; expectedSeq < kTotalWrites; ++expectedSeq) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.read(rMid, out, len)));
        TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
        verifyPattern(std::span<const uint8_t>(out.data(), len), expectedSeq);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(rMid, out, len)));
    const fw::FanoutReaderStats ms = ring.readerStats(rMid);
    TEST_ASSERT_EQUAL_UINT32(7U, ms.records);
    TEST_ASSERT_EQUAL_UINT32(kMidReadStop, ms.dropped);

    // slow 读者从未读过：跳过 0..5（dropped == 6），整条读出 6..9
    for (uint32_t expectedSeq = kFirstSurvivingSeq; expectedSeq < kTotalWrites; ++expectedSeq) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.read(rSlow, out, len)));
        TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
        verifyPattern(std::span<const uint8_t>(out.data(), len), expectedSeq);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(rSlow, out, len)));
    const fw::FanoutReaderStats ss = ring.readerStats(rSlow);
    TEST_ASSERT_EQUAL_UINT32(4U, ss.records);
    TEST_ASSERT_EQUAL_UINT32(kFirstSurvivingSeq, ss.dropped);
    TEST_ASSERT_EQUAL_UINT32(4U * static_cast<uint32_t>(kRecPayload), ss.bytes);
}

void runU32WrapCase(std::span<uint8_t> storage)
{
    fw::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kWrapMaxRecord, fw::kNoLock)));

    uint8_t rFast = 0U;
    uint8_t rLag = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.attach(rFast)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.attach(rLag)));

    static std::array<uint8_t, kWrapMaxRecord> sInBuf{};
    static std::array<uint8_t, kWrapMaxRecord> sOutBuf{};

    // 每条 65535 + 2 = 65537 B；写 65560 次，累计写入约 4.2966 GB，覆盖 head_ 与 oldest_ 的 u32 回绕。
    constexpr uint32_t kTotalWrapWrites = 65560U;
    constexpr uint32_t kVerifyStartSeq = 65520U; // 靠近回绕区起全程逐字节校验

    for (uint32_t seq = 0U; seq < kTotalWrapWrites; ++seq) {
        // 用记录序号打标头尾，靠近回绕点填充完整图案
        sInBuf[0] = static_cast<uint8_t>(seq & kByteMask);
        sInBuf[1] = static_cast<uint8_t>((seq >> kHighByteShift) & kByteMask);
        sInBuf[kWrapMaxRecord - 1U] = static_cast<uint8_t>((seq ^ kWrapTailXor) & kByteMask);
        if (seq >= kVerifyStartSeq) {
            fillPattern(sInBuf, seq);
        }

        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.write(sInBuf)));

        size_t len = 0U;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                                static_cast<uint8_t>(ring.read(rFast, sOutBuf, len)));
        TEST_ASSERT_EQUAL_size_t(kWrapMaxRecord, len);
        TEST_ASSERT_EQUAL_UINT8(sInBuf[0], sOutBuf[0]);
        TEST_ASSERT_EQUAL_UINT8(sInBuf[1], sOutBuf[1]);
        TEST_ASSERT_EQUAL_UINT8(sInBuf[kWrapMaxRecord - 1U], sOutBuf[kWrapMaxRecord - 1U]);
        if (seq >= kVerifyStartSeq) {
            verifyPattern(sOutBuf, seq);
        }
    }

    // 落后读者从未读取：它的 tail 与 oldest_ 均随淘汰跨越了 u32 回绕，现应读到最后存活记录且无损坏
    size_t lagLen = 0U;
    uint32_t lagReadCount = 0U;
    uint32_t expectedSeq = kTotalWrapWrites - 1U;
    // cap 可容纳的最大 kWrapMaxRecord 记录数（131074 放 2 条，262144 放 3 条）
    const uint32_t capRecords =
        static_cast<uint32_t>(storage.size() / (kWrapMaxRecord + fw::kFanoutHeaderBytes));
    const uint32_t firstSurviving = kTotalWrapWrites - capRecords;
    expectedSeq = firstSurviving;
    while (ring.read(rLag, sOutBuf, lagLen) == fw::Result::kOk) {
        TEST_ASSERT_EQUAL_size_t(kWrapMaxRecord, lagLen);
        verifyPattern(sOutBuf, expectedSeq);
        ++expectedSeq;
        ++lagReadCount;
    }
    TEST_ASSERT_EQUAL_UINT32(capRecords, lagReadCount);
    const fw::FanoutReaderStats ls = ring.readerStats(rLag);
    TEST_ASSERT_EQUAL_UINT32(capRecords, ls.records);
    TEST_ASSERT_EQUAL_UINT32(kTotalWrapWrites - capRecords, ls.dropped);
}

void testU32WraparoundPow2AndNonPow2()
{
    static std::array<uint8_t, kWrapPow2Cap> sPow2Storage{};
    static std::array<uint8_t, kWrapRingCapNonPow2> sNonPow2Storage{};
    runU32WrapCase(sPow2Storage);
    runU32WrapCase(sNonPow2Storage);
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(testInitValidationAndUninitCalls);
    RUN_TEST(testAttachDetachAndLateAttach);
    RUN_TEST(testRecordBoundariesAndNospaceNonConsuming);
    RUN_TEST(testMultiReaderOverwriteDropsWholeRecords);
    RUN_TEST(testU32WraparoundPow2AndNonPow2);
    return UNITY_END();
}
