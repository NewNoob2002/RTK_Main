/* 验收测试：device_proto C 接口（报警消息 0x40/0x41）。布局来自 phone_protocol.md §5。冻结输入。 */
#include <string.h>

#include "DevProtoAlarm.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_encode_status_layout(void)
{
    const dev_proto_alarm_status_t st = {
        .seq = 0x1234U,
        .state = DEV_PROTO_ALARM_STATE_ALERT,
        .level = 3U,
        .fix_quality = 0xFFU,
        .battery_pct = 87U,
        .flags = (DEV_PROTO_ALARM_FLAG_PHONE_ONLINE | DEV_PROTO_ALARM_FLAG_TEST_OVERRIDE),
        .uptime_ms = 0xA1B2C3D4U,
    };
    const uint8_t expect[DEV_PROTO_ALARM_STATUS_LEN] = {0x34, 0x12, 0x01, 0x03, 0xFF, 0x57, 0x03, 0x00,
                                                        0xD4, 0xC3, 0xB2, 0xA1, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[DEV_PROTO_ALARM_STATUS_LEN + 4U];

    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(DEV_PROTO_ALARM_STATUS_LEN, dev_proto_alarm_encode_status(&st, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, DEV_PROTO_ALARM_STATUS_LEN);
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[DEV_PROTO_ALARM_STATUS_LEN]); /* 不越界写 */
}

static void test_encode_status_bad_args(void)
{
    dev_proto_alarm_status_t st;
    uint8_t out[DEV_PROTO_ALARM_STATUS_LEN];

    memset(&st, 0, sizeof(st));
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_status(&st, out, DEV_PROTO_ALARM_STATUS_LEN - 1U));
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[0]); /* 不足时不写入 */
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_status(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_status(&st, NULL, sizeof(out)));
}

static void test_status_roundtrip(void)
{
    const dev_proto_alarm_status_t st = {
        .seq = 0xBEEFU,
        .state = DEV_PROTO_ALARM_STATE_NO_FIX,
        .level = 1U,
        .fix_quality = 4U,
        .battery_pct = 42U,
        .flags = DEV_PROTO_ALARM_FLAG_PHONE_ONLINE,
        .uptime_ms = 123456U,
    };
    dev_proto_alarm_status_t back;
    uint8_t buf[DEV_PROTO_ALARM_STATUS_LEN];

    TEST_ASSERT_EQUAL_UINT(DEV_PROTO_ALARM_STATUS_LEN, dev_proto_alarm_encode_status(&st, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(dev_proto_alarm_decode_status(buf, sizeof(buf), &back));
    TEST_ASSERT_EQUAL_HEX16(0xBEEFU, back.seq);
    TEST_ASSERT_EQUAL_HEX8(DEV_PROTO_ALARM_STATE_NO_FIX, back.state);
    TEST_ASSERT_EQUAL_UINT8(1U, back.level);
    TEST_ASSERT_EQUAL_UINT8(4U, back.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(42U, back.battery_pct);
    TEST_ASSERT_EQUAL_HEX8(DEV_PROTO_ALARM_FLAG_PHONE_ONLINE, back.flags);
    TEST_ASSERT_EQUAL_UINT32(123456U, back.uptime_ms);
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_status(buf, sizeof(buf) - 1U, &back));
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_status(NULL, sizeof(buf), &back));
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_status(buf, sizeof(buf), NULL));
}

static void test_decode_heartbeat(void)
{
    const uint8_t data[10] = {0x05, 0x01, 0x50, 0x01, 0, 0, 0, 0, 9, 9};
    dev_proto_alarm_heartbeat_t hb;

    TEST_ASSERT_TRUE(dev_proto_alarm_decode_heartbeat(data, sizeof(data), &hb));
    TEST_ASSERT_EQUAL_HEX16(0x0105U, hb.seq);
    TEST_ASSERT_EQUAL_UINT8(80U, hb.battery_pct);
    TEST_ASSERT_EQUAL_UINT8(1U, hb.local_state);
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_heartbeat(data, 7U, &hb));
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_heartbeat(NULL, 8U, &hb));
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_heartbeat(data, 8U, NULL));
}

static void test_encode_heartbeat(void)
{
    const dev_proto_alarm_heartbeat_t hb = {.seq = 0x0005U, .battery_pct = 80U, .local_state = 0U};
    const uint8_t expect[DEV_PROTO_ALARM_HEARTBEAT_LEN] = {0x05, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[DEV_PROTO_ALARM_HEARTBEAT_LEN];

    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(DEV_PROTO_ALARM_HEARTBEAT_LEN, dev_proto_alarm_encode_heartbeat(&hb, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, sizeof(out));
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_heartbeat(&hb, out, sizeof(out) - 1U));
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[0]);
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_heartbeat(NULL, out, sizeof(out)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_status_layout);
    RUN_TEST(test_encode_status_bad_args);
    RUN_TEST(test_status_roundtrip);
    RUN_TEST(test_decode_heartbeat);
    RUN_TEST(test_encode_heartbeat);
    return UNITY_END();
}
