// bt_reconnect 重连状态机 host 单元测试（ADR-0007 §5，覆盖 Reconnect.hpp 全部语义）。

#include <array>
#include <cstdint>

#include "reconnect/Reconnect.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr reconnect::Addr kAddrA{0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
constexpr reconnect::Addr kAddrB{0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU};
constexpr uint32_t kSeedA = 0x12345678U;
constexpr uint32_t kSeedB = 0xCAFEBABEU;
constexpr uint8_t kScnFirst = 7U;
constexpr uint8_t kScnSecond = 11U;
constexpr uint32_t kSampleCount = 1000U;
constexpr uint32_t kExactSampleCount = 100U;
constexpr uint32_t kWrapStartMs = 0xFFFFF000U;
constexpr uint32_t kWrapBackoffStartMs = 0xFFFFFF00U;
constexpr uint32_t kWrapOnlineStartMs = 0xFFFF7000U;
constexpr uint32_t kStep10Ms = 10U;
constexpr uint32_t kStep20Ms = 20U;
constexpr uint32_t kStep30Ms = 30U;
constexpr uint32_t kStep40Ms = 40U;
constexpr uint32_t kStep50Ms = 50U;
constexpr uint32_t kStep100Ms = 100U;
constexpr uint32_t kStep200Ms = 200U;
constexpr uint32_t kStep600Ms = 600U;
constexpr uint32_t kStep999Ms = 999U;
constexpr uint32_t kStep1000Ms = 1000U;
constexpr uint32_t kStep2000Ms = 2000U;
constexpr uint32_t kStep4000Ms = 4000U;
constexpr uint32_t kStep5000Ms = 5000U;
constexpr uint32_t kStep8000Ms = 8000U;
constexpr uint32_t kStep9999Ms = 9999U;
constexpr uint32_t kStep10000Ms = 10000U;
constexpr uint32_t kStep30000Ms = 30000U;
constexpr uint32_t kStep31000Ms = 31000U;
constexpr uint32_t kStep100000Ms = 100000U;
constexpr uint32_t kStep500Ms = 500U;
constexpr uint32_t kStep1999Ms = 1999U;
constexpr uint32_t kStep8001Ms = 8001U;
constexpr uint8_t kSdpThreeFailures = 3U;
constexpr uint32_t kFastRounds = 29U;
constexpr uint32_t kReopenRounds = 5U;
constexpr uint32_t kFlapRounds = 10U;
constexpr uint32_t kJitterMinMs = 800U;
constexpr uint32_t kJitterMaxMs = 1200U;

reconnect::Config make_exact_cfg() noexcept
{
    reconnect::Config cfg{};
    cfg.jitterPct = 0U;
    return cfg;
}

void assert_stats_eq(const reconnect::Stats &expected, const reconnect::Stats &actual) noexcept
{
    TEST_ASSERT_EQUAL_UINT32(expected.attempts, actual.attempts);
    TEST_ASSERT_EQUAL_UINT32(expected.sdpQueries, actual.sdpQueries);
    TEST_ASSERT_EQUAL_UINT32(expected.failures, actual.failures);
    TEST_ASSERT_EQUAL_UINT32(expected.connects, actual.connects);
    TEST_ASSERT_EQUAL_UINT32(expected.disconnects, actual.disconnects);
    TEST_ASSERT_EQUAL_UINT32(expected.lastDelayMs, actual.lastDelayMs);
}

void test_default_state_and_no_target()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kIdle),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_FALSE(rc.hasTarget());
    TEST_ASSERT_EQUAL_UINT32(0U, rc.nextAttemptIn(0U));

    reconnect::Action act{};
    TEST_ASSERT_FALSE(rc.start(kStep100Ms, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kIdle),
                            static_cast<uint8_t>(rc.state()));
}

void test_configure_invalid_params()
{
    reconnect::Reconnector rc;
    reconnect::Config valid{};
    valid.fastWindowMs = kStep5000Ms;
    valid.fastRetryMs = kStep500Ms;
    valid.backoffMinMs = kStep1000Ms;
    valid.backoffMaxMs = kStep8000Ms;
    valid.jitterPct = reconnect::kMaxJitterPct;
    valid.stableOnlineMs = kStep4000Ms;
    valid.sdpAfterFailures = kSdpThreeFailures;
    valid.attemptTimeoutMs = kStep2000Ms;
    TEST_ASSERT_TRUE(rc.configure(valid, kSeedA));

    rc.setTarget(kAddrA);
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(kStep100Ms, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kDiscovering),
                            static_cast<uint8_t>(rc.state()));

    // jitter > kMaxJitterPct
    {
        reconnect::Config bad = valid;
        bad.jitterPct = reconnect::kMaxJitterPct + 1U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    // sdpAfterFailures == 0
    {
        reconnect::Config bad = valid;
        bad.sdpAfterFailures = 0U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    // backoff_min > backoff_max
    {
        reconnect::Config bad = valid;
        bad.backoffMinMs = kStep8001Ms;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    // 任一时长为 0
    {
        reconnect::Config bad = valid;
        bad.fastWindowMs = 0U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    {
        reconnect::Config bad = valid;
        bad.fastRetryMs = 0U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    {
        reconnect::Config bad = valid;
        bad.backoffMinMs = 0U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    {
        reconnect::Config bad = valid;
        bad.backoffMaxMs = 0U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    {
        reconnect::Config bad = valid;
        bad.stableOnlineMs = 0U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }
    {
        reconnect::Config bad = valid;
        bad.attemptTimeoutMs = 0U;
        TEST_ASSERT_FALSE(rc.configure(bad, kSeedB));
    }

    // 非法 configure 不得改变状态与已有合法配置
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kDiscovering),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.poll(kStep100Ms + kStep1999Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.poll(kStep100Ms + kStep2000Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                            static_cast<uint8_t>(rc.state()));

    // backoff_min == backoff_max 且 seed == 0 合法
    reconnect::Config eq_cfg = valid;
    eq_cfg.backoffMinMs = kStep2000Ms;
    eq_cfg.backoffMaxMs = kStep2000Ms;
    TEST_ASSERT_TRUE(rc.configure(eq_cfg, 0U));
}

void test_fast_window_fixed_retry()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    uint32_t now = 0U;
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(now, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));

    // 在 30000 ms 快速窗口内多次失败：始终 1000 ms 固定间隔
    for (uint32_t round = 0U; round < kFastRounds; ++round) {
        now += kStep100Ms;
        act = rc.onSdpResult(false, 0U, now);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                                static_cast<uint8_t>(act.type));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                                static_cast<uint8_t>(rc.state()));
        TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);
        TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.nextAttemptIn(now));
        TEST_ASSERT_EQUAL_UINT32(1U, rc.nextAttemptIn(now + kStep999Ms));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                                static_cast<uint8_t>(rc.poll(now + kStep999Ms).type));

        now += kStep1000Ms;
        TEST_ASSERT_EQUAL_UINT32(0U, rc.nextAttemptIn(now));
        act = rc.poll(now);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                                static_cast<uint8_t>(act.type));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kDiscovering),
                                static_cast<uint8_t>(rc.state()));
    }
    TEST_ASSERT_EQUAL_UINT32(kFastRounds, rc.stats().failures);
    TEST_ASSERT_EQUAL_UINT32(kFastRounds + 1U, rc.stats().sdpQueries);
}

void test_backoff_outside_window_and_cap()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    uint32_t now = 0U;
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(now, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));

    // 跨越 30000 ms 快速窗口边界
    now = kStep30000Ms;
    const std::array<uint32_t, 8> expected_delays{1000U,  2000U,  4000U,  8000U,
                                                  16000U, 30000U, 30000U, 30000U};
    for (const uint32_t expected : expected_delays) {
        act = rc.onSdpResult(false, 0U, now);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                                static_cast<uint8_t>(act.type));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                                static_cast<uint8_t>(rc.state()));
        TEST_ASSERT_EQUAL_UINT32(expected, rc.stats().lastDelayMs);

        now += expected;
        act = rc.poll(now);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                                static_cast<uint8_t>(act.type));
    }
}

void test_jitter_range_and_determinism()
{
    reconnect::Config cfg{};
    cfg.fastWindowMs = kStep100Ms;
    cfg.fastRetryMs = kStep1000Ms;
    cfg.backoffMinMs = kStep1000Ms;
    cfg.backoffMaxMs = kStep1000Ms;
    cfg.jitterPct = reconnect::kDefJitterPct;

    reconnect::Reconnector rc1;
    reconnect::Reconnector rc2;
    TEST_ASSERT_TRUE(rc1.configure(cfg, kSeedA));
    TEST_ASSERT_TRUE(rc2.configure(cfg, kSeedA));
    rc1.setTarget(kAddrA);
    rc2.setTarget(kAddrA);

    uint32_t now1 = 0U;
    uint32_t now2 = 0U;
    reconnect::Action act1{};
    reconnect::Action act2{};
    TEST_ASSERT_TRUE(rc1.start(now1, act1));
    TEST_ASSERT_TRUE(rc2.start(now2, act2));

    // 先跳过 100 ms 快速窗口
    now1 = kStep200Ms;
    now2 = kStep200Ms;
    uint32_t min_seen = UINT32_MAX;
    uint32_t max_seen = 0U;

    for (uint32_t i = 0U; i < kSampleCount; ++i) {
        static_cast<void>(rc1.onSdpResult(false, 0U, now1));
        static_cast<void>(rc2.onSdpResult(false, 0U, now2));

        const uint32_t d1 = rc1.stats().lastDelayMs;
        const uint32_t d2 = rc2.stats().lastDelayMs;
        TEST_ASSERT_EQUAL_UINT32(d2, d1);
        TEST_ASSERT_TRUE((d1 >= kJitterMinMs) && (d1 <= kJitterMaxMs));
        if (d1 < min_seen) {
            min_seen = d1;
        }
        if (d1 > max_seen) {
            max_seen = d1;
        }

        now1 += d1;
        now2 += d2;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                                static_cast<uint8_t>(rc1.poll(now1).type));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                                static_cast<uint8_t>(rc2.poll(now2).type));
    }
    TEST_ASSERT_TRUE(min_seen < max_seen);

    // jitter=0 时精确
    cfg.jitterPct = 0U;
    reconnect::Reconnector exact;
    TEST_ASSERT_TRUE(exact.configure(cfg, kSeedA));
    exact.setTarget(kAddrA);
    uint32_t now_e = 0U;
    TEST_ASSERT_TRUE(exact.start(now_e, act1));
    now_e = kStep200Ms;
    for (uint32_t i = 0U; i < kExactSampleCount; ++i) {
        static_cast<void>(exact.onSdpResult(false, 0U, now_e));
        TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, exact.stats().lastDelayMs);
        now_e += kStep1000Ms;
        static_cast<void>(exact.poll(now_e));
    }
}

void test_disconnect_stable_vs_unstable()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    uint32_t now = 0U;
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(now, act));

    // 在窗口外推高退避到 4000（经历 1000, 2000，下一级为 4000）
    now = kStep30000Ms;
    static_cast<void>(rc.onSdpResult(false, 0U, now));
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);
    now += kStep1000Ms;
    static_cast<void>(rc.poll(now));
    static_cast<void>(rc.onSdpResult(false, 0U, now));
    TEST_ASSERT_EQUAL_UINT32(kStep2000Ms, rc.stats().lastDelayMs);
    now += kStep2000Ms;
    static_cast<void>(rc.poll(now));

    // 连上后不足 10 s（9999 ms）掉线：不复位退避，继续到 4000 ms
    act = rc.onSdpResult(true, kScnFirst, now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));
    act = rc.onOpen(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnected),
                            static_cast<uint8_t>(rc.state()));

    now += kStep9999Ms;
    act = rc.onClose(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_EQUAL_UINT32(kStep4000Ms, rc.stats().lastDelayMs);
    TEST_ASSERT_EQUAL_UINT32(1U, rc.stats().disconnects);

    // 再次连上并在线满 10000 ms 后掉线：复位退避并重开快速窗口
    now += kStep4000Ms;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));
    static_cast<void>(rc.onOpen(now));

    now += kStep10000Ms;
    static_cast<void>(rc.onClose(now));
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);
    TEST_ASSERT_EQUAL_UINT32(2U, rc.stats().disconnects);

    // 验证重开后的 30 s 快速窗口确实有效（多次失败仍为 1000 ms）
    for (uint32_t i = 0U; i < kReopenRounds; ++i) {
        now += kStep1000Ms;
        act = rc.poll(now);
        if (act.type == reconnect::ActionType::kStartSdp) {
            act = rc.onSdpResult(true, kScnFirst, now);
        }
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                                static_cast<uint8_t>(act.type));
        static_cast<void>(rc.onConnectFailed(now));
        TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);
    }
}

void test_unstable_disconnect_in_fast_window_no_storm()
{
    // 快速窗口内反复“连上 2 s 即断”，窗口起点不得刷新，30 s 后必然转入指数退避。
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    uint32_t now = 0U;
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(now, act));
    act = rc.onSdpResult(true, kScnFirst, now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));

    for (uint32_t round = 0U; round < kFlapRounds; ++round) {
        static_cast<void>(rc.onOpen(now));
        now += kStep2000Ms;
        static_cast<void>(rc.onClose(now));
        TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);
        now += kStep1000Ms;
        act = rc.poll(now);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                                static_cast<uint8_t>(act.type));
    }
    // 当前 now == 30000，下一轮不稳定掉线落在窗口外
    static_cast<void>(rc.onOpen(now));
    now += kStep2000Ms;
    static_cast<void>(rc.onClose(now));
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);
    now += kStep1000Ms;
    static_cast<void>(rc.poll(now));
    static_cast<void>(rc.onOpen(now));
    now += kStep2000Ms;
    static_cast<void>(rc.onClose(now));
    TEST_ASSERT_EQUAL_UINT32(kStep2000Ms, rc.stats().lastDelayMs);
}

void test_scn_cache_and_resdp_after_failures()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    uint32_t now = 0U;
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(now, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT32(1U, rc.stats().sdpQueries);

    // SDP 成功缓存 SCN=7 并立即返回 kConnect(7)
    act = rc.onSdpResult(true, kScnFirst, now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(kScnFirst, act.scn);
    TEST_ASSERT_EQUAL_UINT32(1U, rc.stats().attempts);

    // 第 1 次 connect 失败：仍保留 SCN 缓存
    static_cast<void>(rc.onConnectFailed(now));
    now += kStep1000Ms;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(kScnFirst, act.scn);
    TEST_ASSERT_EQUAL_UINT32(1U, rc.stats().sdpQueries);

    // 中途成功连接再断开：连续失败计数清零
    static_cast<void>(rc.onOpen(now));
    now += kStep1000Ms;
    static_cast<void>(rc.onClose(now));
    now += kStep1000Ms;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));

    // 连续第 1 次失败：不触发重新 SDP
    static_cast<void>(rc.onConnectFailed(now));
    now += kStep1000Ms;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT32(1U, rc.stats().sdpQueries);

    // 连续第 2 次失败：清除缓存，下次退避到期走 SDP
    static_cast<void>(rc.onConnectFailed(now));
    now += kStep1000Ms;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT32(2U, rc.stats().sdpQueries);

    act = rc.onSdpResult(true, kScnSecond, now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(kScnSecond, act.scn);
}

void test_attempt_timeout()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    uint32_t now = kStep100Ms;
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(now, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kDiscovering),
                            static_cast<uint8_t>(rc.state()));

    // kDiscovering 超时前不触发
    act = rc.poll(now + reconnect::kDefAttemptTimeoutMs - 1U);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kDiscovering),
                            static_cast<uint8_t>(rc.state()));

    // 到 attemptTimeoutMs 按失败处理
    now += reconnect::kDefAttemptTimeoutMs;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_EQUAL_UINT32(1U, rc.stats().failures);

    // 退避到期后完成 SDP，再测 kConnecting 超时（连续 2 次同样清 SCN）
    now += rc.stats().lastDelayMs;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));
    act = rc.onSdpResult(true, kScnFirst, now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnecting),
                            static_cast<uint8_t>(rc.state()));

    // 第 1 次 connect 超时
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnecting),
                            static_cast<uint8_t>(rc.state()));
    static_cast<void>(rc.poll(now + reconnect::kDefAttemptTimeoutMs - 1U));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnecting),
                            static_cast<uint8_t>(rc.state()));
    now += reconnect::kDefAttemptTimeoutMs;
    static_cast<void>(rc.poll(now));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_EQUAL_UINT32(2U, rc.stats().failures);

    now += rc.stats().lastDelayMs;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));

    // 第 2 次 connect 超时 -> 清除缓存
    now += reconnect::kDefAttemptTimeoutMs;
    static_cast<void>(rc.poll(now));
    TEST_ASSERT_EQUAL_UINT32(kSdpThreeFailures, rc.stats().failures);

    now += rc.stats().lastDelayMs;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));
}

void test_stop_and_start()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(0U, act));
    act = rc.onSdpResult(true, kScnFirst, kStep10Ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));

    rc.stop();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kStopped),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_EQUAL_UINT32(0U, rc.nextAttemptIn(kStep10Ms));

    // kStopped 后所有事件只更新统计，不产生动作、不改变状态
    const reconnect::Stats before = rc.stats();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.poll(kStep100000Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onSdpResult(false, 0U, kStep100Ms).type));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(reconnect::ActionType::kNone),
        static_cast<uint8_t>(rc.onSdpResult(true, kScnSecond, kStep100Ms + kStep10Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onOpen(kStep100Ms + kStep20Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onConnectFailed(kStep100Ms + kStep30Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onClose(kStep100Ms + kStep40Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kStopped),
                            static_cast<uint8_t>(rc.state()));

    TEST_ASSERT_EQUAL_UINT32(before.failures + 2U, rc.stats().failures);
    TEST_ASSERT_EQUAL_UINT32(before.connects + 1U, rc.stats().connects);
    TEST_ASSERT_EQUAL_UINT32(before.disconnects + 1U, rc.stats().disconnects);
    TEST_ASSERT_EQUAL_UINT32(before.attempts, rc.stats().attempts);
    TEST_ASSERT_EQUAL_UINT32(before.sdpQueries, rc.stats().sdpQueries);

    // 再次 start：保留此前缓存的 SCN=7，直接 kConnect(7)
    TEST_ASSERT_TRUE(rc.start(kStep100000Ms, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kConnect),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(kScnFirst, act.scn);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnecting),
                            static_cast<uint8_t>(rc.state()));
}

void test_change_target_clears_cache()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);
    TEST_ASSERT_TRUE(rc.hasTarget());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kAddrA.data(), rc.target().data(), reconnect::kAddrLen);

    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(0U, act));
    act = rc.onSdpResult(true, kScnFirst, kStep10Ms);
    static_cast<void>(rc.onOpen(kStep20Ms));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnected),
                            static_cast<uint8_t>(rc.state()));

    // 设相同目标不清除连接与缓存
    rc.setTarget(kAddrA);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnected),
                            static_cast<uint8_t>(rc.state()));

    // 改目标清除 SCN 缓存并回到 kIdle
    rc.setTarget(kAddrB);
    TEST_ASSERT_TRUE(rc.hasTarget());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kAddrB.data(), rc.target().data(), reconnect::kAddrLen);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kIdle),
                            static_cast<uint8_t>(rc.state()));

    // 此时旧连接回调 onClose 被忽略
    act = rc.onClose(kStep30Ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(act.type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kIdle),
                            static_cast<uint8_t>(rc.state()));

    // start 必须重新 SDP
    TEST_ASSERT_TRUE(rc.start(kStep40Ms, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));

    // clearTarget 回到 kIdle 且无目标
    rc.clearTarget();
    TEST_ASSERT_FALSE(rc.hasTarget());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kIdle),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_FALSE(rc.start(kStep50Ms, act));
}

void test_unexpected_events_ignored()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    // kIdle 忽略所有事件
    reconnect::Stats snap = rc.stats();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.poll(kStep100Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onSdpResult(true, kScnFirst, kStep100Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onOpen(kStep100Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onConnectFailed(kStep100Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onClose(kStep100Ms).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kIdle),
                            static_cast<uint8_t>(rc.state()));
    assert_stats_eq(snap, rc.stats());

    // kDiscovering 忽略 open / connect_failed / close
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(kStep200Ms, act));
    snap = rc.stats();
    const uint32_t t_disc = kStep200Ms + kStep10Ms;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onOpen(t_disc).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onConnectFailed(t_disc).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onClose(t_disc).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kDiscovering),
                            static_cast<uint8_t>(rc.state()));
    assert_stats_eq(snap, rc.stats());

    // kConnecting 忽略 sdp_result / close，且重复 start 返回 false
    act = rc.onSdpResult(true, kScnFirst, kStep200Ms + kStep20Ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnecting),
                            static_cast<uint8_t>(rc.state()));
    snap = rc.stats();
    const uint32_t t_conn = kStep200Ms + kStep30Ms;
    TEST_ASSERT_FALSE(rc.start(t_conn, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onSdpResult(false, 0U, t_conn).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onClose(t_conn).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnecting),
                            static_cast<uint8_t>(rc.state()));
    assert_stats_eq(snap, rc.stats());

    // kConnected 忽略 sdp_result / open / connect_failed，且重复 start 返回 false
    const uint32_t t_open = kStep200Ms + kStep40Ms;
    static_cast<void>(rc.onOpen(t_open));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnected),
                            static_cast<uint8_t>(rc.state()));
    snap = rc.stats();
    const uint32_t t_online = kStep200Ms + kStep50Ms;
    TEST_ASSERT_FALSE(rc.start(t_online, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onSdpResult(true, kScnSecond, t_online).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onOpen(t_online).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onConnectFailed(t_online).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.poll(t_online).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kConnected),
                            static_cast<uint8_t>(rc.state()));
    assert_stats_eq(snap, rc.stats());

    // kBackoff 忽略 sdp_result / open / connect_failed / close
    const uint32_t t_close = kStep200Ms + kStep100Ms;
    static_cast<void>(rc.onClose(t_close));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                            static_cast<uint8_t>(rc.state()));
    snap = rc.stats();
    const uint32_t t_backoff = t_close + kStep10Ms;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onSdpResult(true, kScnSecond, t_backoff).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onOpen(t_backoff).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onConnectFailed(t_backoff).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.onClose(t_backoff).type));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                            static_cast<uint8_t>(rc.state()));
    assert_stats_eq(snap, rc.stats());
}

void test_u32_wraparound()
{
    reconnect::Reconnector rc;
    TEST_ASSERT_TRUE(rc.configure(make_exact_cfg(), kSeedA));
    rc.setTarget(kAddrA);

    // now 从 0xFFFF_F000 起跑
    uint32_t now = kWrapStartMs;
    reconnect::Action act{};
    TEST_ASSERT_TRUE(rc.start(now, act));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));

    // attempt 超时跨越 0xFFFF_FFFF -> 0x0000_0F40
    now += reconnect::kDefAttemptTimeoutMs;
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::State::kBackoff),
                            static_cast<uint8_t>(rc.state()));
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);

    // 验证跨回绕的 nextAttemptIn 与到期
    rc.stop();
    now = kWrapBackoffStartMs;
    TEST_ASSERT_TRUE(rc.start(now, act));
    now += kStep100Ms; // 0xFFFF_FF64
    static_cast<void>(rc.onSdpResult(false, 0U, now));
    // 到期时刻为 0xFFFF_FF64 + 1000 = 0x0000_034C
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.nextAttemptIn(now));
    TEST_ASSERT_EQUAL_UINT32(kStep4000Ms / 10U, rc.nextAttemptIn(now + kStep600Ms));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kNone),
                            static_cast<uint8_t>(rc.poll(now + kStep999Ms).type));

    now += kStep1000Ms; // 0x0000_034C
    TEST_ASSERT_EQUAL_UINT32(0U, rc.nextAttemptIn(now));
    act = rc.poll(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(reconnect::ActionType::kStartSdp),
                            static_cast<uint8_t>(act.type));

    // 跨回绕后继续超过 30000 ms 快速窗口，验证指数退避
    now = kWrapBackoffStartMs + kStep30000Ms;
    static_cast<void>(rc.onSdpResult(false, 0U, now));
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc.stats().lastDelayMs);
    now += kStep1000Ms;
    static_cast<void>(rc.poll(now));
    static_cast<void>(rc.onSdpResult(false, 0U, now));
    TEST_ASSERT_EQUAL_UINT32(kStep2000Ms, rc.stats().lastDelayMs);

    // 跨回绕在线 >= 10 s 复位（now 单调递增跨过 0xFFFF_FFFF）
    reconnect::Reconnector rc2;
    TEST_ASSERT_TRUE(rc2.configure(make_exact_cfg(), kSeedA));
    rc2.setTarget(kAddrA);
    uint32_t t2 = kWrapOnlineStartMs;
    TEST_ASSERT_TRUE(rc2.start(t2, act));
    t2 += kStep31000Ms; // 0xFFFF_E918（已出快速窗口）
    static_cast<void>(rc2.onSdpResult(false, 0U, t2));
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc2.stats().lastDelayMs);
    t2 += kStep1000Ms;
    static_cast<void>(rc2.poll(t2));
    static_cast<void>(rc2.onSdpResult(false, 0U, t2));
    TEST_ASSERT_EQUAL_UINT32(kStep2000Ms, rc2.stats().lastDelayMs);
    t2 += kStep2000Ms; // 0xFFFF_F4D0
    static_cast<void>(rc2.poll(t2));
    static_cast<void>(rc2.onSdpResult(true, kScnFirst, t2));
    static_cast<void>(rc2.onOpen(t2));
    t2 += kStep10000Ms; // 0x0000_1BE0，跨越回绕且在线满 10 s
    static_cast<void>(rc2.onClose(t2));
    TEST_ASSERT_EQUAL_UINT32(kStep1000Ms, rc2.stats().lastDelayMs);
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_state_and_no_target);
    RUN_TEST(test_configure_invalid_params);
    RUN_TEST(test_fast_window_fixed_retry);
    RUN_TEST(test_backoff_outside_window_and_cap);
    RUN_TEST(test_jitter_range_and_determinism);
    RUN_TEST(test_disconnect_stable_vs_unstable);
    RUN_TEST(test_unstable_disconnect_in_fast_window_no_storm);
    RUN_TEST(test_scn_cache_and_resdp_after_failures);
    RUN_TEST(test_attempt_timeout);
    RUN_TEST(test_stop_and_start);
    RUN_TEST(test_change_target_clears_cache);
    RUN_TEST(test_unexpected_events_ignored);
    RUN_TEST(test_u32_wraparound);
    return UNITY_END();
}
