/* 验收测试：alarm_msg。期望布局来自 docs/protocol/phone_protocol.md §5。
 * 冻结输入。 */
#include <string.h>

#include "alarm_msg.h"
#include "unity.h"

static alarm_src_t g_src;

void
setUp (void)
{
    alarm_src_init(&g_src);
}

void
tearDown (void)
{
}

static void
test_encode_status_layout (void)
{
    alarm_status_t const status = {
        .seq         = 0x1234U,
        .state       = ALARM_STATE_ALERT,
        .level       = 3U,
        .fix_quality = 0xFFU,
        .battery_pct = 87U,
        .flags       = (ALARM_FLAG_PHONE_ONLINE | ALARM_FLAG_TEST_OVERRIDE),
        .uptime_ms   = 0xA1B2C3D4U,
    };
    uint8_t const expect[ALARM_STATUS_LEN] = {
        0x34, 0x12, 0x01, 0x03, 0xFF, 0x57, 0x03, 0x00,
        0xD4, 0xC3, 0xB2, 0xA1, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[ALARM_STATUS_LEN + 4U];

    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(ALARM_STATUS_LEN,
                           alarm_msg_encode_status(&status, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, ALARM_STATUS_LEN);
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[ALARM_STATUS_LEN]); /* 不越界写 */
}

static void
test_encode_status_bad_args (void)
{
    alarm_status_t status;
    uint8_t        out[ALARM_STATUS_LEN];

    memset(&status, 0, sizeof(status));
    TEST_ASSERT_EQUAL_UINT(0U, alarm_msg_encode_status(&status, out,
                                                       ALARM_STATUS_LEN - 1U));
    TEST_ASSERT_EQUAL_UINT(0U, alarm_msg_encode_status(NULL, out,
                                                       sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0U, alarm_msg_encode_status(&status, NULL,
                                                       sizeof(out)));
}

static void
test_decode_heartbeat (void)
{
    uint8_t const     data[10] = {0x05, 0x01, 0x50, 0x01, 0, 0, 0, 0, 9, 9};
    alarm_heartbeat_t hb;

    TEST_ASSERT_TRUE(alarm_msg_decode_heartbeat(data, sizeof(data), &hb));
    TEST_ASSERT_EQUAL_HEX16(0x0105U, hb.seq);
    TEST_ASSERT_EQUAL_UINT8(80U, hb.battery_pct);
    TEST_ASSERT_EQUAL_UINT8(1U, hb.local_state);
    TEST_ASSERT_FALSE(alarm_msg_decode_heartbeat(data, 7U, &hb));
    TEST_ASSERT_FALSE(alarm_msg_decode_heartbeat(NULL, 8U, &hb));
    TEST_ASSERT_FALSE(alarm_msg_decode_heartbeat(data, 8U, NULL));
}

static void
test_src_defaults_unknown (void)
{
    TEST_ASSERT_FALSE(alarm_src_is_override(&g_src));
    TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_UNKNOWN, alarm_src_state(&g_src));
    TEST_ASSERT_EQUAL_UINT8(0U, alarm_src_level(&g_src));
}

static void
test_src_override_priority (void)
{
    alarm_src_set_fence(&g_src, ALARM_STATE_NORMAL);
    TEST_ASSERT_TRUE(alarm_src_set_override(&g_src, ALARM_STATE_ALERT, 3U));
    TEST_ASSERT_TRUE(alarm_src_is_override(&g_src));
    TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_ALERT, alarm_src_state(&g_src));
    TEST_ASSERT_EQUAL_UINT8(3U, alarm_src_level(&g_src));
    alarm_src_clear_override(&g_src);
    TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_NORMAL, alarm_src_state(&g_src));
    TEST_ASSERT_EQUAL_UINT8(0U, alarm_src_level(&g_src));
}

static void
test_src_fence_alert_level (void)
{
    alarm_src_set_fence(&g_src, ALARM_STATE_ALERT);
    TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_ALERT, alarm_src_state(&g_src));
    TEST_ASSERT_EQUAL_UINT8(2U, alarm_src_level(&g_src));
}

static void
test_src_override_rejects_invalid (void)
{
    TEST_ASSERT_FALSE(alarm_src_set_override(&g_src, 0x03U, 1U));
    TEST_ASSERT_FALSE(alarm_src_set_override(&g_src, ALARM_STATE_UNKNOWN, 1U));
    TEST_ASSERT_FALSE(alarm_src_set_override(&g_src, ALARM_STATE_ALERT,
                                             ALARM_LEVEL_MAX + 1U));
    TEST_ASSERT_FALSE(alarm_src_is_override(&g_src));
    TEST_ASSERT_TRUE(alarm_src_set_override(&g_src, ALARM_STATE_NO_FIX, 0U));
    TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_NO_FIX, alarm_src_state(&g_src));
}

int
main (void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_status_layout);
    RUN_TEST(test_encode_status_bad_args);
    RUN_TEST(test_decode_heartbeat);
    RUN_TEST(test_src_defaults_unknown);
    RUN_TEST(test_src_override_priority);
    RUN_TEST(test_src_fence_alert_level);
    RUN_TEST(test_src_override_rejects_invalid);
    return UNITY_END();
}
