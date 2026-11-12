/* 验收测试：link_mgr。期望值来自 components/link/include/link_mgr.h 契约。
 * 冻结输入。 */
#include <string.h>

#include "link_mgr.h"
#include "unity.h"

#define H_PHONE   (0x81U)
#define H_ALARM   (0x82U)
#define H_OTHER   (0x83U)
#define MSG_QUERY (0x0001U)

static link_addr_t const g_phone_addr = {{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
static link_addr_t const g_alarm_addr = {{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};
static link_addr_t const g_other_addr = {{0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}};

static link_mgr_t     g_mgr;
static link_actions_t g_act;

void
setUp (void)
{
    memset(&g_act, 0x5A, sizeof(g_act)); /* 验证函数会先清零 */
}

void
tearDown (void)
{
}

static void
init_with_alarm (link_policy_t policy)
{
    link_mgr_init(&g_mgr, policy, &g_alarm_addr, 1U);
}

static void
test_whitelisted_is_alarm_unknown_is_phone (void)
{
    init_with_alarm(LINK_POLICY_CONCURRENT);
    TEST_ASSERT_EQUAL(LINK_ROLE_PHONE,
                      link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 100U,
                                       &g_act));
    TEST_ASSERT_EQUAL_UINT8(0U, g_act.count);
    TEST_ASSERT_EQUAL(LINK_ROLE_ALARM,
                      link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, 200U,
                                       &g_act));
    TEST_ASSERT_EQUAL_UINT8(0U, g_act.count);
    TEST_ASSERT_EQUAL(LINK_ROLE_PHONE, link_mgr_role(&g_mgr, H_PHONE));
    TEST_ASSERT_EQUAL(LINK_ROLE_ALARM, link_mgr_role(&g_mgr, H_ALARM));
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE, link_mgr_role(&g_mgr, H_OTHER));
}

static void
test_second_phone_rejected_without_pairing (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE,
                      link_mgr_on_open(&g_mgr, H_OTHER, &g_other_addr, 10U,
                                       &g_act));
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_OTHER, g_act.handles[0]);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE, link_mgr_role(&g_mgr, H_OTHER));
}

static void
test_duplicate_handle_and_full_table_rejected (void)
{
    init_with_alarm(LINK_POLICY_CONCURRENT);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE,
                      link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 1U,
                                       &g_act));
    (void) link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, 2U, &g_act);
    /* 表满（2 个会话），即使是白名单地址也拒绝 */
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE,
                      link_mgr_on_open(&g_mgr, H_OTHER, &g_alarm_addr, 3U,
                                       &g_act));
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_OTHER, g_act.handles[0]);
}

static void
test_preempt_phone_kicks_alarm (void)
{
    init_with_alarm(LINK_POLICY_PHONE_PREEMPT);
    TEST_ASSERT_EQUAL(LINK_ROLE_ALARM,
                      link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, 0U,
                                       &g_act));
    TEST_ASSERT_EQUAL(LINK_ROLE_PHONE,
                      link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 5U,
                                       &g_act));
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_ALARM, g_act.handles[0]);
}

static void
test_preempt_alarm_rejected_while_phone (void)
{
    init_with_alarm(LINK_POLICY_PHONE_PREEMPT);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE,
                      link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, 5U,
                                       &g_act));
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_ALARM, g_act.handles[0]);
}

static void
test_close_frees_slot (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    link_mgr_on_close(&g_mgr, H_PHONE);
    link_mgr_on_close(&g_mgr, 0x999U); /* 未知句柄忽略 */
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE, link_mgr_role(&g_mgr, H_PHONE));
    TEST_ASSERT_EQUAL(LINK_ROLE_PHONE,
                      link_mgr_on_open(&g_mgr, H_OTHER, &g_other_addr, 1U,
                                       &g_act));
}

static void
test_alarm_rx_timeout_with_wrap (void)
{
    uint32_t const t0 = 0xFFFFF000U; /* 跨越 32 位回绕 */

    init_with_alarm(LINK_POLICY_CONCURRENT);
    (void) link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, t0, &g_act);
    TEST_ASSERT_EQUAL(LINK_ROLE_ALARM,
                      link_mgr_on_frame(&g_mgr, H_ALARM, 0x03U, 0x0041U,
                                        t0 + 1000U, &g_act));
    link_mgr_tick(&g_mgr, t0 + 1000U + LINK_ALARM_RX_TIMEOUT_MS, &g_act);
    TEST_ASSERT_EQUAL_UINT8(0U, g_act.count); /* 等于阈值不断开 */
    link_mgr_tick(&g_mgr, t0 + 1001U + LINK_ALARM_RX_TIMEOUT_MS, &g_act);
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_ALARM, g_act.handles[0]);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE, link_mgr_role(&g_mgr, H_ALARM));
}

static void
test_phone_has_no_rx_timeout (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    link_mgr_tick(&g_mgr, 600000U, &g_act);
    TEST_ASSERT_EQUAL_UINT8(0U, g_act.count);
    TEST_ASSERT_EQUAL(LINK_ROLE_PHONE, link_mgr_role(&g_mgr, H_PHONE));
}

static void
test_pairing_promotes_phone_role_to_alarm (void)
{
    link_addr_t list[LINK_MAX_ALARMS];

    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    /* 无手机在线时，未配对的报警器先被当作 PHONE */
    (void) link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, 0U, &g_act);
    link_mgr_pair_open(&g_mgr, 10U);
    TEST_ASSERT_TRUE(link_mgr_is_pairing(&g_mgr));
    TEST_ASSERT_EQUAL(LINK_ROLE_ALARM,
                      link_mgr_on_frame(&g_mgr, H_ALARM, LINK_HELLO_SENDER,
                                        LINK_HELLO_MSG_ID, 20U, &g_act));
    TEST_ASSERT_EQUAL_UINT8(0U, g_act.count);
    TEST_ASSERT_EQUAL_UINT8(1U, link_mgr_whitelist_get(&g_mgr, list,
                                                       LINK_MAX_ALARMS));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(g_alarm_addr.addr, list[0].addr,
                                 LINK_ADDR_LEN);
    /* 重复 HELLO 不重复加入 */
    (void) link_mgr_on_frame(&g_mgr, H_ALARM, LINK_HELLO_SENDER,
                             LINK_HELLO_MSG_ID, 30U, &g_act);
    TEST_ASSERT_EQUAL_UINT8(1U, link_mgr_whitelist_get(&g_mgr, list,
                                                       LINK_MAX_ALARMS));
}

static void
test_pending_when_phone_online_and_pairing (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    link_mgr_pair_open(&g_mgr, 0U);
    TEST_ASSERT_EQUAL(LINK_ROLE_PENDING,
                      link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, 100U,
                                       &g_act));
    TEST_ASSERT_EQUAL(LINK_ROLE_ALARM,
                      link_mgr_on_frame(&g_mgr, H_ALARM, LINK_HELLO_SENDER,
                                        LINK_HELLO_MSG_ID, 200U, &g_act));
}

static void
test_pending_non_hello_is_disconnected (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    link_mgr_pair_open(&g_mgr, 0U);
    (void) link_mgr_on_open(&g_mgr, H_OTHER, &g_other_addr, 100U, &g_act);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE,
                      link_mgr_on_frame(&g_mgr, H_OTHER, 0x00U, MSG_QUERY,
                                        200U, &g_act));
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_OTHER, g_act.handles[0]);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE, link_mgr_role(&g_mgr, H_OTHER));
}

static void
test_pending_timeout (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    link_mgr_pair_open(&g_mgr, 0U);
    (void) link_mgr_on_open(&g_mgr, H_OTHER, &g_other_addr, 1000U, &g_act);
    link_mgr_tick(&g_mgr, 1000U + LINK_PENDING_TIMEOUT_MS, &g_act);
    TEST_ASSERT_EQUAL_UINT8(0U, g_act.count);
    link_mgr_tick(&g_mgr, 1001U + LINK_PENDING_TIMEOUT_MS, &g_act);
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_OTHER, g_act.handles[0]);
}

static void
test_hello_without_pairing_is_unauthorized (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    (void) link_mgr_on_open(&g_mgr, H_OTHER, &g_other_addr, 0U, &g_act);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE,
                      link_mgr_on_frame(&g_mgr, H_OTHER, LINK_HELLO_SENDER,
                                        LINK_HELLO_MSG_ID, 10U, &g_act));
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_OTHER, g_act.handles[0]);
}

static void
test_pair_window_expires (void)
{
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, NULL, 0U);
    link_mgr_pair_open(&g_mgr, 500U);
    link_mgr_tick(&g_mgr, 500U + LINK_PAIR_WINDOW_MS - 1U, &g_act);
    TEST_ASSERT_TRUE(link_mgr_is_pairing(&g_mgr));
    link_mgr_tick(&g_mgr, 500U + LINK_PAIR_WINDOW_MS, &g_act);
    TEST_ASSERT_FALSE(link_mgr_is_pairing(&g_mgr));
}

static void
test_whitelist_full_rejects_new_alarm (void)
{
    link_addr_t full[LINK_MAX_ALARMS];

    for (uint8_t idx = 0U; idx < LINK_MAX_ALARMS; idx++)
    {
        memset(full[idx].addr, 0xC0 + idx, LINK_ADDR_LEN);
    }
    link_mgr_init(&g_mgr, LINK_POLICY_CONCURRENT, full, LINK_MAX_ALARMS);
    (void) link_mgr_on_open(&g_mgr, H_OTHER, &g_other_addr, 0U, &g_act);
    link_mgr_pair_open(&g_mgr, 0U);
    TEST_ASSERT_EQUAL(LINK_ROLE_NONE,
                      link_mgr_on_frame(&g_mgr, H_OTHER, LINK_HELLO_SENDER,
                                        LINK_HELLO_MSG_ID, 10U, &g_act));
    TEST_ASSERT_EQUAL_UINT8(1U, g_act.count);
    TEST_ASSERT_EQUAL_UINT32(H_OTHER, g_act.handles[0]);
}

static void
test_whitelist_clear_and_handles (void)
{
    uint32_t    handles[LINK_MAX_SESSIONS];
    link_addr_t list[LINK_MAX_ALARMS];

    init_with_alarm(LINK_POLICY_CONCURRENT);
    (void) link_mgr_on_open(&g_mgr, H_PHONE, &g_phone_addr, 0U, &g_act);
    (void) link_mgr_on_open(&g_mgr, H_ALARM, &g_alarm_addr, 0U, &g_act);
    TEST_ASSERT_EQUAL_UINT8(1U, link_mgr_handles(&g_mgr, LINK_ROLE_ALARM,
                                                 handles, LINK_MAX_SESSIONS));
    TEST_ASSERT_EQUAL_UINT32(H_ALARM, handles[0]);
    link_mgr_whitelist_clear(&g_mgr);
    TEST_ASSERT_EQUAL_UINT8(0U, link_mgr_whitelist_get(&g_mgr, list,
                                                       LINK_MAX_ALARMS));
    /* 已连接会话角色不受影响 */
    TEST_ASSERT_EQUAL(LINK_ROLE_ALARM, link_mgr_role(&g_mgr, H_ALARM));
}

int
main (void)
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
    RUN_TEST(test_whitelist_clear_and_handles);
    return UNITY_END();
}
