// app::FanoutRing host 单元测试（ADR-0007 §3.6-1）：单/多读者、覆盖丢弃计数、
// attach 晚于写入、detach 复用、记录长度边界、kNoSpace 不消费、u32 位置回绕（含非 2 的幂容量）。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "app/fanout_ring.hpp"
#include "app/lock.hpp"
#include "app/result.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr size_t kSmallMaxRecord = 16U;
constexpr size_t kSmallRingCap = 40U;
constexpr size_t kWrapMaxRecord = 65535U;
// 故意使用非 2 的幂的最小合法容量 2 * (65535 + 2) = 131074，以验证非 2 的幂在 u32 回绕时的对齐正确性。
constexpr size_t kWrapRingCapNonPow2 = 2U * (kWrapMaxRecord + app::kFanoutHeaderBytes);
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
    uint32_t lock_calls = 0U;
    uint32_t unlock_calls = 0U;
    int32_t depth = 0;
    bool underflow = false;
};

void ring_spy_lock(void *ctx) noexcept
{
    auto *spy = static_cast<RingLockSpy *>(ctx);
    ++spy->lock_calls;
    ++spy->depth;
}

void ring_spy_unlock(void *ctx) noexcept
{
    auto *spy = static_cast<RingLockSpy *>(ctx);
    ++spy->unlock_calls;
    --spy->depth;
    if (spy->depth < 0) {
        spy->underflow = true;
    }
}

void fill_pattern(std::span<uint8_t> dst, uint32_t seq) noexcept
{
    for (size_t i = 0U; i < dst.size(); ++i) {
        dst[i] = static_cast<uint8_t>((seq + static_cast<uint32_t>(i)) & kByteMask); // 取低 8 位生成可验证图案
    }
}

void verify_pattern(std::span<const uint8_t> data, uint32_t seq) noexcept
{
    for (size_t i = 0U; i < data.size(); ++i) {
        const uint8_t expected = static_cast<uint8_t>((seq + static_cast<uint32_t>(i)) & kByteMask);
        TEST_ASSERT_EQUAL_UINT8(expected, data[i]);
    }
}

void test_init_validation_and_uninit_calls()
{
    app::FanoutRing ring;
    std::array<uint8_t, kSmallRingCap> storage{};
    std::array<uint8_t, kSmallMaxRecord> out{};
    const std::array<uint8_t, 3> sample{1U, 2U, 3U};
    uint8_t rid = 0U;
    size_t len = kSentinelLen;

    // 未 init 前所有操作返回 kInvalidState
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidState), static_cast<uint8_t>(ring.attach(rid)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidState), static_cast<uint8_t>(ring.detach(0U)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidState), static_cast<uint8_t>(ring.write(sample)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidState),
                            static_cast<uint8_t>(ring.read(0U, out, len)));
    TEST_ASSERT_EQUAL_size_t(0U, len);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.writer_stats().records);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(0U).records);

    // 非法 init 参数
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(std::span<uint8_t>{}, 1U, app::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(storage, 0U, app::kNoLock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(storage, kWrapMaxRecord + 1U, app::kNoLock)));
    // storage.size() < 2 * (max_record + 2)
    std::array<uint8_t, kTooSmallCap> too_small{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(too_small, 1U, app::kNoLock)));
    // 半空 LockOps 非法
    const app::LockOps bad_lock{&ring_spy_lock, nullptr, nullptr};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.init(storage, 1U, bad_lock)));

    // 最小边界 max_record=1, size=6 合法
    std::array<uint8_t, kMinValidCap> min_buf{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                            static_cast<uint8_t>(ring.init(min_buf, 1U, app::kNoLock)));

    // 重复 init 返回 kInvalidState
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidState),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, app::kNoLock)));
}

void test_attach_detach_and_late_attach()
{
    std::array<uint8_t, kSmallRingCap> storage{};
    RingLockSpy spy{};
    const app::LockOps ops{&ring_spy_lock, &ring_spy_unlock, &spy};
    app::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, ops)));

    // 先写入 2 条记录再 attach：新读者不得回放历史
    const std::array<uint8_t, 4> pre1{0x10U, 0x11U, 0x12U, 0x13U};
    const std::array<uint8_t, 4> pre2{0x20U, 0x21U, 0x22U, 0x23U};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(pre1)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(pre2)));

    std::array<uint8_t, app::kFanoutMaxReaders> ids{};
    for (size_t i = 0U; i < app::kFanoutMaxReaders; ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(ids[i])));
        TEST_ASSERT_TRUE(ids[i] < app::kFanoutMaxReaders);
    }
    uint8_t extra = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kNoSpace), static_cast<uint8_t>(ring.attach(extra)));

    // 晚于 pre1/pre2 attach 的读者当前应读不到历史数据
    std::array<uint8_t, kSmallMaxRecord> out{};
    size_t len = kSentinelLen;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(ids[0], out, len)));
    TEST_ASSERT_EQUAL_size_t(0U, len);

    // 写入新记录 post1：所有已 attach 读者都能读到 post1
    const std::array<uint8_t, 5> post1{0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(post1)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.read(ids[0], out, len)));
    TEST_ASSERT_EQUAL_size_t(post1.size(), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(post1.data(), out.data(), post1.size());

    // detach 与无效 id 检查，以及复用 id 后统计清零且不回放历史
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.detach(kInvalidReaderId)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.detach(ids[0])));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg), static_cast<uint8_t>(ring.detach(ids[0])));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.read(ids[0], out, len)));

    uint8_t reused = kSentinelId;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(reused)));
    TEST_ASSERT_EQUAL_UINT8(ids[0], reused);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(reused).records);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(reused).bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.reader_stats(reused).dropped);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(reused, out, len)));

    // 另一位一直在线的读者 ids[1] 仍能读到 post1
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.read(ids[1], out, len)));
    TEST_ASSERT_EQUAL_size_t(post1.size(), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(post1.data(), out.data(), post1.size());

    TEST_ASSERT_EQUAL_UINT32(spy.lock_calls, spy.unlock_calls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_FALSE(spy.underflow);
}

void test_record_boundaries_and_nospace_non_consuming()
{
    std::array<uint8_t, kSmallRingCap> storage{};
    app::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, app::kNoLock)));

    uint8_t rid = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(rid)));

    // 长度 0 与 > max_record 被拒绝并计入 rejected
    std::array<uint8_t, kSmallMaxRecord + 1U> oversized{};
    oversized.fill(kOversizeFill);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.write(std::span<const uint8_t>{})));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kInvalidArg),
                            static_cast<uint8_t>(ring.write(oversized)));
    TEST_ASSERT_EQUAL_UINT32(2U, ring.writer_stats().rejected);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.writer_stats().records);

    // 写入长度 1 与 max_record 边界记录（同时跨过缓冲区物理末尾）
    std::array<uint8_t, kSmallMaxRecord> max_rec{};
    fill_pattern(max_rec, 1U);
    const std::array<uint8_t, 1> min_rec{0xFEU};
    // 连续写 4 次，促使写指针多次跨越 storage 末尾（40 B）并包含 2 B 长度头跨尾的情况
    for (uint32_t round = 0U; round < kOddRounds; ++round) {
        std::array<uint8_t, kOddRecordLen> odd_rec{};
        fill_pattern(odd_rec, round + kOddSeqBase);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(odd_rec)));
        std::array<uint8_t, kSmallMaxRecord> got{};
        size_t len = 0U;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.read(rid, got, len)));
        TEST_ASSERT_EQUAL_size_t(odd_rec.size(), len);
        verify_pattern(std::span<const uint8_t>(got.data(), len), round + kOddSeqBase);
    }

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(max_rec)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(min_rec)));

    // 输出缓冲不足 -> kNoSpace，len = 需要的长度，尾指针不动
    std::array<uint8_t, kSmallMaxRecord - 1U> small_out{};
    size_t len = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kNoSpace),
                            static_cast<uint8_t>(ring.read(rid, small_out, len)));
    TEST_ASSERT_EQUAL_size_t(kSmallMaxRecord, len);
    TEST_ASSERT_EQUAL_UINT32(3U, ring.reader_stats(rid).records);

    // 换足够大的缓冲立即再次读：完整读出刚才那条 max_rec，然后读出 min_rec
    std::array<uint8_t, kSmallMaxRecord> full_out{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                            static_cast<uint8_t>(ring.read(rid, full_out, len)));
    TEST_ASSERT_EQUAL_size_t(kSmallMaxRecord, len);
    verify_pattern(std::span<const uint8_t>(full_out.data(), len), 1U);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                            static_cast<uint8_t>(ring.read(rid, full_out, len)));
    TEST_ASSERT_EQUAL_size_t(1U, len);
    TEST_ASSERT_EQUAL_UINT8(0xFEU, full_out[0]);
    TEST_ASSERT_EQUAL_UINT32(5U, ring.reader_stats(rid).records);
}

void test_multi_reader_overwrite_drops_whole_records()
{
    // cap = 40，每条记录 8 B 负载 + 2 B 头 = 10 B，容量正好放 4 条记录
    constexpr size_t kRecPayload = 8U;
    constexpr uint32_t kTotalWrites = 10U;
    std::array<uint8_t, kSmallRingCap> storage{};
    app::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kSmallMaxRecord, app::kNoLock)));

    uint8_t r_fast = 0U;
    uint8_t r_mid = 0U;
    uint8_t r_slow = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(r_fast)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(r_mid)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(r_slow)));

    std::array<uint8_t, kSmallMaxRecord> out{};
    size_t len = 0U;

    for (uint32_t seq = 0U; seq < kTotalWrites; ++seq) {
        std::array<uint8_t, kRecPayload> rec{};
        fill_pattern(rec, seq);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(rec)));

        // fast 读者每写一条读一条：0 丢弃
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                                static_cast<uint8_t>(ring.read(r_fast, out, len)));
        TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
        verify_pattern(std::span<const uint8_t>(out.data(), len), seq);

        // mid 读者在前半程读了 0,1,2，之后停下直到结束（丢 3,4,5，剩 6,7,8,9）
        if (seq < kMidReadStop) {
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                                    static_cast<uint8_t>(ring.read(r_mid, out, len)));
            verify_pattern(std::span<const uint8_t>(out.data(), len), seq);
        }
    }

    const app::FanoutWriterStats ws = ring.writer_stats();
    TEST_ASSERT_EQUAL_UINT32(kTotalWrites, ws.records);
    TEST_ASSERT_EQUAL_UINT32(kTotalWrites * static_cast<uint32_t>(kRecPayload), ws.bytes);
    TEST_ASSERT_EQUAL_UINT32(kFirstSurvivingSeq, ws.overwritten);

    const app::FanoutReaderStats fs = ring.reader_stats(r_fast);
    TEST_ASSERT_EQUAL_UINT32(kTotalWrites, fs.records);
    TEST_ASSERT_EQUAL_UINT32(0U, fs.dropped);

    // mid 读者先用过小缓冲读：返回 kNoSpace 且不消费、不重复累加 dropped
    std::array<uint8_t, 1> tiny_out{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kNoSpace),
                            static_cast<uint8_t>(ring.read(r_mid, tiny_out, len)));
    TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
    TEST_ASSERT_EQUAL_UINT32(kMidReadStop, ring.reader_stats(r_mid).dropped);

    // mid 读者跳过被覆盖的 3..5（dropped == 3），整条读出 6..9
    for (uint32_t expected_seq = kFirstSurvivingSeq; expected_seq < kTotalWrites; ++expected_seq) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                                static_cast<uint8_t>(ring.read(r_mid, out, len)));
        TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
        verify_pattern(std::span<const uint8_t>(out.data(), len), expected_seq);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(r_mid, out, len)));
    const app::FanoutReaderStats ms = ring.reader_stats(r_mid);
    TEST_ASSERT_EQUAL_UINT32(7U, ms.records);
    TEST_ASSERT_EQUAL_UINT32(kMidReadStop, ms.dropped);

    // slow 读者从未读过：跳过 0..5（dropped == 6），整条读出 6..9
    for (uint32_t expected_seq = kFirstSurvivingSeq; expected_seq < kTotalWrites; ++expected_seq) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                                static_cast<uint8_t>(ring.read(r_slow, out, len)));
        TEST_ASSERT_EQUAL_size_t(kRecPayload, len);
        verify_pattern(std::span<const uint8_t>(out.data(), len), expected_seq);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kNotFound),
                            static_cast<uint8_t>(ring.read(r_slow, out, len)));
    const app::FanoutReaderStats ss = ring.reader_stats(r_slow);
    TEST_ASSERT_EQUAL_UINT32(4U, ss.records);
    TEST_ASSERT_EQUAL_UINT32(kFirstSurvivingSeq, ss.dropped);
    TEST_ASSERT_EQUAL_UINT32(4U * static_cast<uint32_t>(kRecPayload), ss.bytes);
}

void run_u32_wrap_case(std::span<uint8_t> storage)
{
    app::FanoutRing ring;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, kWrapMaxRecord, app::kNoLock)));

    uint8_t r_fast = 0U;
    uint8_t r_lag = 0U;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(r_fast)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.attach(r_lag)));

    static std::array<uint8_t, kWrapMaxRecord> s_in_buf{};
    static std::array<uint8_t, kWrapMaxRecord> s_out_buf{};

    // 每条 65535 + 2 = 65537 B；写 65560 次，累计写入约 4.2966 GB，覆盖 head_ 与 oldest_ 的 u32 回绕。
    constexpr uint32_t kTotalWrapWrites = 65560U;
    constexpr uint32_t kVerifyStartSeq = 65520U; // 靠近回绕区起全程逐字节校验

    for (uint32_t seq = 0U; seq < kTotalWrapWrites; ++seq) {
        // 用记录序号打标头尾，靠近回绕点填充完整图案
        s_in_buf[0] = static_cast<uint8_t>(seq & kByteMask);
        s_in_buf[1] = static_cast<uint8_t>((seq >> kHighByteShift) & kByteMask);
        s_in_buf[kWrapMaxRecord - 1U] = static_cast<uint8_t>((seq ^ kWrapTailXor) & kByteMask);
        if (seq >= kVerifyStartSeq) {
            fill_pattern(s_in_buf, seq);
        }

        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk), static_cast<uint8_t>(ring.write(s_in_buf)));

        size_t len = 0U;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(app::Result::kOk),
                                static_cast<uint8_t>(ring.read(r_fast, s_out_buf, len)));
        TEST_ASSERT_EQUAL_size_t(kWrapMaxRecord, len);
        TEST_ASSERT_EQUAL_UINT8(s_in_buf[0], s_out_buf[0]);
        TEST_ASSERT_EQUAL_UINT8(s_in_buf[1], s_out_buf[1]);
        TEST_ASSERT_EQUAL_UINT8(s_in_buf[kWrapMaxRecord - 1U], s_out_buf[kWrapMaxRecord - 1U]);
        if (seq >= kVerifyStartSeq) {
            verify_pattern(s_out_buf, seq);
        }
    }

    // 落后读者从未读取：它的 tail 与 oldest_ 均随淘汰跨越了 u32 回绕，现应读到最后存活记录且无损坏
    size_t lag_len = 0U;
    uint32_t lag_read_count = 0U;
    uint32_t expected_seq = kTotalWrapWrites - 1U;
    // cap 可容纳的最大 kWrapMaxRecord 记录数（131074 放 2 条，262144 放 3 条）
    const uint32_t cap_records = static_cast<uint32_t>(storage.size() / (kWrapMaxRecord + app::kFanoutHeaderBytes));
    const uint32_t first_surviving = kTotalWrapWrites - cap_records;
    expected_seq = first_surviving;
    while (ring.read(r_lag, s_out_buf, lag_len) == app::Result::kOk) {
        TEST_ASSERT_EQUAL_size_t(kWrapMaxRecord, lag_len);
        verify_pattern(s_out_buf, expected_seq);
        ++expected_seq;
        ++lag_read_count;
    }
    TEST_ASSERT_EQUAL_UINT32(cap_records, lag_read_count);
    const app::FanoutReaderStats ls = ring.reader_stats(r_lag);
    TEST_ASSERT_EQUAL_UINT32(cap_records, ls.records);
    TEST_ASSERT_EQUAL_UINT32(kTotalWrapWrites - cap_records, ls.dropped);
}

void test_u32_wraparound_pow2_and_non_pow2()
{
    static std::array<uint8_t, kWrapPow2Cap> s_pow2_storage{};
    static std::array<uint8_t, kWrapRingCapNonPow2> s_non_pow2_storage{};
    run_u32_wrap_case(s_pow2_storage);
    run_u32_wrap_case(s_non_pow2_storage);
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_validation_and_uninit_calls);
    RUN_TEST(test_attach_detach_and_late_attach);
    RUN_TEST(test_record_boundaries_and_nospace_non_consuming);
    RUN_TEST(test_multi_reader_overwrite_drops_whole_records);
    RUN_TEST(test_u32_wraparound_pow2_and_non_pow2);
    return UNITY_END();
}
