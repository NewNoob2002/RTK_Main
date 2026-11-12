// 验收测试：LinkManager（冻结输入）。期望值来自 link_manager.hpp 契约。
#include <array>
#include <cstdint>

#include "core/link_manager.hpp"
#include "unity.h"

using core::Actions;
using core::BtAddr;
using core::LinkManager;
using core::Policy;
using core::Role;
using core::SessionHandle;

static constexpr SessionHandle kPhone = 0x81;
static constexpr SessionHandle kAlarm = 0x82;
static constexpr SessionHandle kOther = 0x83;
static constexpr uint16_t kMsgQuery = 0x0001;

static constexpr BtAddr kPhoneAddr{{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
static constexpr BtAddr kAlarmAddr{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};
static constexpr BtAddr kOtherAddr{{0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}};

static LinkManager s_mgr;
static Actions s_act;

extern "C" void setUp(void)
{
    s_mgr = LinkManager{};
    s_act.handles.fill(0x5A5A5A5A); // 验证调用会先清空
    s_act.count = 3;
}
extern "C" void tearDown(void) {}

static void init_with_alarm(Policy policy)
{
    const std::array<BtAddr, 1> wl = {kAlarmAddr};
    s_mgr.init(policy, wl);
}

static void test_whitelisted_is_alarm_unknown_is_phone()
{
    init_with_alarm(Policy::kConcurrent);
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.on_open(kPhone, kPhoneAddr, 100, s_act));
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.on_open(kAlarm, kAlarmAddr, 200, s_act));
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.role(kPhone));
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.role(kAlarm));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kOther));
    TEST_ASSERT_TRUE(s_mgr.has_role(Role::kPhone));
    TEST_ASSERT_FALSE(s_mgr.has_role(Role::kPending));
}

static void test_second_phone_rejected_without_pairing()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.on_open(kOther, kOtherAddr, 10, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.view()[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kOther));
}

static void test_duplicate_handle_and_full_table_rejected()
{
    init_with_alarm(Policy::kConcurrent);
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.on_open(kPhone, kPhoneAddr, 1, s_act));
    static_cast<void>(s_mgr.on_open(kAlarm, kAlarmAddr, 2, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.on_open(kOther, kAlarmAddr, 3, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void test_preempt_phone_kicks_alarm()
{
    init_with_alarm(Policy::kPhonePreempt);
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.on_open(kAlarm, kAlarmAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.on_open(kPhone, kPhoneAddr, 5, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, s_act.handles[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kAlarm)); // 立即移除
}

static void test_preempt_alarm_rejected_while_phone()
{
    init_with_alarm(Policy::kPhonePreempt);
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.on_open(kAlarm, kAlarmAddr, 5, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, s_act.handles[0]);
    TEST_ASSERT_TRUE(Policy::kPhonePreempt == s_mgr.policy());
}

static void test_close_frees_slot()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.on_close(kPhone);
    s_mgr.on_close(0x999); // 未知句柄忽略
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kPhone));
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.on_open(kOther, kOtherAddr, 1, s_act));
}

static void test_alarm_rx_timeout_with_wrap()
{
    const uint32_t t0 = 0xFFFFF000U; // 跨越 32 位回绕
    init_with_alarm(Policy::kConcurrent);
    static_cast<void>(s_mgr.on_open(kAlarm, kAlarmAddr, t0, s_act));
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.on_frame(kAlarm, 0x03, 0x0041, t0 + 1000U, s_act));
    s_mgr.tick(t0 + 1000U + core::kAlarmRxTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count); // 等于阈值不断开
    s_mgr.tick(t0 + 1001U + core::kAlarmRxTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, s_act.handles[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kAlarm));
}

static void test_phone_has_no_rx_timeout()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.tick(600000, s_act);
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.role(kPhone));
}

static void test_pairing_promotes_phone_role_to_alarm()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kAlarm, kAlarmAddr, 0, s_act));
    s_mgr.pair_open(10);
    TEST_ASSERT_TRUE(s_mgr.is_pairing());
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.on_frame(kAlarm, core::kHelloSender, core::kHelloMsgId, 20, s_act));
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_EQUAL_UINT(1, s_mgr.whitelist().size());
    TEST_ASSERT_TRUE(s_mgr.whitelist()[0] == kAlarmAddr);
    static_cast<void>(s_mgr.on_frame(kAlarm, core::kHelloSender, core::kHelloMsgId, 30, s_act));
    TEST_ASSERT_EQUAL_UINT(1, s_mgr.whitelist().size()); // 不重复加入
}

static void test_pending_when_phone_online_and_pairing()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.pair_open(0);
    TEST_ASSERT_TRUE(Role::kPending == s_mgr.on_open(kAlarm, kAlarmAddr, 100, s_act));
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.on_frame(kAlarm, core::kHelloSender, core::kHelloMsgId, 200, s_act));
}

static void test_pending_non_hello_is_disconnected()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.pair_open(0);
    static_cast<void>(s_mgr.on_open(kOther, kOtherAddr, 100, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.on_frame(kOther, 0x00, kMsgQuery, 200, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kOther));
}

static void test_pending_timeout()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.pair_open(0);
    static_cast<void>(s_mgr.on_open(kOther, kOtherAddr, 1000, s_act));
    s_mgr.tick(1000U + core::kPendingTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    s_mgr.tick(1001U + core::kPendingTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void test_hello_without_pairing_is_unauthorized()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.on_open(kOther, kOtherAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.on_frame(kOther, core::kHelloSender, core::kHelloMsgId, 10, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void test_pair_window_expires()
{
    s_mgr.init(Policy::kConcurrent, {});
    s_mgr.pair_open(500);
    s_mgr.tick(500U + core::kPairWindowMs - 1U, s_act);
    TEST_ASSERT_TRUE(s_mgr.is_pairing());
    s_mgr.tick(500U + core::kPairWindowMs, s_act);
    TEST_ASSERT_FALSE(s_mgr.is_pairing());
}

static void test_whitelist_full_rejects_new_alarm()
{
    std::array<BtAddr, core::kMaxAlarms> full{};
    for (size_t i = 0; i < full.size(); ++i) {
        full[i].bytes.fill(static_cast<uint8_t>(0xC0U + i)); // i < 4
    }
    s_mgr.init(Policy::kConcurrent, full);
    static_cast<void>(s_mgr.on_open(kOther, kOtherAddr, 0, s_act));
    s_mgr.pair_open(0);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.on_frame(kOther, core::kHelloSender, core::kHelloMsgId, 10, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void test_init_truncates_and_clear()
{
    std::array<BtAddr, core::kMaxAlarms + 2> many{};
    for (size_t i = 0; i < many.size(); ++i) {
        many[i].bytes.fill(static_cast<uint8_t>(i + 1U));
    }
    s_mgr.init(Policy::kConcurrent, many);
    TEST_ASSERT_EQUAL_UINT(core::kMaxAlarms, s_mgr.whitelist().size());
    s_mgr.whitelist_clear();
    TEST_ASSERT_EQUAL_UINT(0, s_mgr.whitelist().size());
}

static void test_handles_and_clear_keeps_roles()
{
    init_with_alarm(Policy::kConcurrent);
    static_cast<void>(s_mgr.on_open(kPhone, kPhoneAddr, 0, s_act));
    static_cast<void>(s_mgr.on_open(kAlarm, kAlarmAddr, 0, s_act));
    std::array<SessionHandle, core::kMaxSessions> out{};
    TEST_ASSERT_EQUAL_UINT(1, s_mgr.handles(Role::kAlarm, out));
    TEST_ASSERT_EQUAL_UINT32(kAlarm, out[0]);
    TEST_ASSERT_EQUAL_UINT(0, s_mgr.handles(Role::kAlarm, std::span<SessionHandle>(out.data(), 0)));
    s_mgr.whitelist_clear();
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.role(kAlarm));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_whitelisted_is_alarm_unknown_is_phone);
    RUN_TEST(test_second_phone_rejected_without_pairing);
    RUN_TEST(test_duplicate_handle_and_full_table_rejected);
    RUN_TEST(test_preempt_phone_kicks_alarm);
    RUN_TEST(test_preempt_alarm_rejected_while_phone);
    RUN_TEST(test_close_frees_slot);
    RUN_TEST(test_alarm_rx_timeout_with_wrap);
    RUN_TEST(test_phone_has_no_rx_timeout);
    RUN_TEST(test_pairing_promotes_phone_role_to_alarm);
    RUN_TEST(test_pending_when_phone_online_and_pairing);
    RUN_TEST(test_pending_non_hello_is_disconnected);
    RUN_TEST(test_pending_timeout);
    RUN_TEST(test_hello_without_pairing_is_unauthorized);
    RUN_TEST(test_pair_window_expires);
    RUN_TEST(test_whitelist_full_rejects_new_alarm);
    RUN_TEST(test_init_truncates_and_clear);
    RUN_TEST(test_handles_and_clear_keeps_roles);
    return UNITY_END();
}
