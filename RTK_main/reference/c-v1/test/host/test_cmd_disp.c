/* 验收测试：cmd_disp。期望值来自 docs/protocol/phone_protocol.md §3–§5
 * 与 cmd_disp.h 契约。冻结输入。 */
#include <string.h>

#include "cmd_disp.h"
#include "unity.h"

#define H_PHONE (0x81U)
#define H_ALARM (0x82U)

static link_addr_t const g_phone_addr = {{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
static link_addr_t const g_alarm_addr = {{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};

static cmd_ctx_t   g_ctx;
static alarm_src_t g_alarm;
static link_mgr_t  g_link;
static cmd_reply_t g_reply;
static uint8_t     g_payload[PROTO_MAX_PAYLOAD];

void
setUp (void)
{
    link_mgr_init(&g_link, LINK_POLICY_CONCURRENT, NULL, 0U);
    alarm_src_init(&g_alarm);
    cmd_disp_init(&g_ctx, &g_alarm, &g_link);
    memset(&g_reply, 0x5A, sizeof(g_reply));
    memset(g_payload, 0, sizeof(g_payload));
}

void
tearDown (void)
{
}

static cmd_result_t
send_cmd (link_role_t role, uint16_t msg_id, uint8_t sender, uint8_t type,
          uint16_t len, uint32_t now_ms)
{
    proto_frame_t frame;

    frame.msg_id      = msg_id;
    frame.sender      = sender;
    frame.type        = type;
    frame.protocol    = 0U;
    frame.interval    = 0U;
    frame.payload_len = len;
    frame.p_payload   = g_payload;
    return cmd_disp_handle(&g_ctx, role, &frame, now_ms, &g_reply);
}

/* 解析应答帧并检查头部，返回 payload 指针。 */
static uint8_t const *
check_reply (uint16_t msg_id, uint8_t type, uint16_t len)
{
    static proto_parser_t parser;
    static proto_frame_t  frame;
    int                   frames = 0;

    proto_parser_init(&parser);
    TEST_ASSERT_TRUE(g_reply.len > 0U);
    for (size_t idx = 0U; idx < g_reply.len; idx++)
    {
        if (PROTO_PARSE_FRAME ==
            proto_parser_feed(&parser, g_reply.buf[idx], &frame))
        {
            frames++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, frames);
    TEST_ASSERT_EQUAL_UINT(PROTO_HEADER_LEN + len + PROTO_CRC_LEN,
                           g_reply.len);
    TEST_ASSERT_EQUAL_HEX16(msg_id, frame.msg_id);
    TEST_ASSERT_EQUAL_HEX8(PROTO_SENDER_DEV, frame.sender);
    TEST_ASSERT_EQUAL_HEX8(type, frame.type);
    TEST_ASSERT_EQUAL_UINT16(len, frame.payload_len);
    return frame.p_payload;
}

static void
test_dev_info_0x01 (void)
{
    uint8_t const * p_pl;

    memcpy(g_ctx.info.sn, "SN0001", 6U);
    memcpy(g_ctx.info.fw_ver, "V6.1.0", 6U);
    TEST_ASSERT_EQUAL(CMD_RES_REPLY,
                      send_cmd(LINK_ROLE_PHONE, 0x0001U, PROTO_SENDER_APP,
                               PROTO_TYPE_QUERY, 0U, 0U));
    p_pl = check_reply(0x0001U, PROTO_TYPE_QUERY_RES, CMD_DEV_INFO_LEN);
    TEST_ASSERT_EQUAL_MEMORY("E1-Lite", &p_pl[0], 7U);
    TEST_ASSERT_EQUAL_HEX8(0x00, p_pl[7]);
    TEST_ASSERT_EQUAL_MEMORY("SN0001", &p_pl[8], 6U);
    /* 整个字段宽度：不足部分必须补 0（防止越界读入字面量之后的字节） */
    TEST_ASSERT_EQUAL_MEMORY("V1.1.0\0\0", &p_pl[24], 8U);
    TEST_ASSERT_EQUAL_MEMORY("V6.1.0", &p_pl[32], 6U);
    TEST_ASSERT_EQUAL_MEMORY("UM980\0\0\0", &p_pl[56], 8U);
    TEST_ASSERT_EQUAL_MEMORY("5002D\0\0\0", &p_pl[96], 8U);
}

static void
test_dev_state_0x02_constant (void)
{
    uint8_t expect[CMD_DEV_STATE_LEN];

    memset(expect, 0, sizeof(expect));
    expect[0]  = 0x03;
    expect[1]  = 0x01;
    expect[2]  = 0x81;
    expect[3]  = 0x83;
    expect[4]  = 0x81;
    expect[9]  = 0x81;
    expect[10] = 0x81;
    expect[11] = 0x81;
    expect[13] = 0x01;
    TEST_ASSERT_EQUAL(CMD_RES_REPLY,
                      send_cmd(LINK_ROLE_PHONE, 0x0002U, PROTO_SENDER_APP,
                               PROTO_TYPE_QUERY, 0U, 0U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect,
                                 check_reply(0x0002U, PROTO_TYPE_QUERY_RES,
                                             CMD_DEV_STATE_LEN),
                                 CMD_DEV_STATE_LEN);
}

static void
test_battery_0x03 (void)
{
    uint8_t const * p_pl;

    g_ctx.info.battery_pct    = 64U;
    g_ctx.info.battery_temp_c = -5;
    TEST_ASSERT_EQUAL(CMD_RES_REPLY,
                      send_cmd(LINK_ROLE_PHONE, 0x0003U, PROTO_SENDER_APP,
                               PROTO_TYPE_QUERY, 0U, 0U));
    p_pl = check_reply(0x0003U, PROTO_TYPE_QUERY_RES, CMD_BATTERY_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x01, p_pl[0]);
    TEST_ASSERT_EQUAL_UINT8(64U, p_pl[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFB, p_pl[12]);
}

static void
test_placeholders_0x04_0x05_0x06 (void)
{
    uint8_t const ok[2] = {0x01, 0x01};
    uint8_t const zeros[CMD_SAT_TRACK_LEN] = {0};

    (void) send_cmd(LINK_ROLE_PHONE, 0x0004U, 0U, PROTO_TYPE_QUERY, 0U, 0U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok, check_reply(0x0004U,
                                                 PROTO_TYPE_QUERY_RES, 2U),
                                 2U);
    (void) send_cmd(LINK_ROLE_PHONE, 0x0005U, 0U, PROTO_TYPE_QUERY, 0U, 0U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok, check_reply(0x0005U,
                                                 PROTO_TYPE_QUERY_RES, 2U),
                                 2U);
    (void) send_cmd(LINK_ROLE_PHONE, 0x0006U, 0U, PROTO_TYPE_QUERY, 0U, 0U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zeros,
                                 check_reply(0x0006U, PROTO_TYPE_QUERY_RES,
                                             CMD_SAT_TRACK_LEN),
                                 CMD_SAT_TRACK_LEN);
    (void) send_cmd(LINK_ROLE_PHONE, 0x0006U, 0U, PROTO_TYPE_SET, 0U, 0U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok, check_reply(0x0006U,
                                                 PROTO_TYPE_SET_RES, 2U),
                                 2U);
}

static void
test_work_mode_0x07_roundtrip_and_short (void)
{
    uint8_t const   ok[2]   = {0x01, 0x01};
    uint8_t const   fail[2] = {0x02, 0x02};
    uint8_t const * p_pl;

    for (uint16_t idx = 0U; idx < CMD_WORK_MODE_SET; idx++)
    {
        g_payload[idx] = (uint8_t) (idx + 1U); /* < 256 */
    }
    TEST_ASSERT_EQUAL(CMD_RES_REPLY,
                      send_cmd(LINK_ROLE_PHONE, 0x0007U, 0U, PROTO_TYPE_SET,
                               CMD_WORK_MODE_SET, 0U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok, check_reply(0x0007U,
                                                 PROTO_TYPE_SET_RES, 2U),
                                 2U);
    (void) send_cmd(LINK_ROLE_PHONE, 0x0007U, 0U, PROTO_TYPE_QUERY, 0U, 0U);
    p_pl = check_reply(0x0007U, PROTO_TYPE_QUERY_RES, CMD_WORK_MODE_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(g_payload, p_pl, CMD_WORK_MODE_SET);

    (void) send_cmd(LINK_ROLE_PHONE, 0x0007U, 0U, PROTO_TYPE_SET,
                    CMD_WORK_MODE_SET - 1U, 0U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fail, check_reply(0x0007U,
                                                   PROTO_TYPE_SET_RES, 2U),
                                 2U);
}

static void
test_com_0x0d (void)
{
    uint8_t const * p_pl;

    TEST_ASSERT_EQUAL(CMD_RES_REPLY,
                      send_cmd(LINK_ROLE_PHONE, 0x000DU, 0U, PROTO_TYPE_QUERY,
                               0U, 0U));
    p_pl = check_reply(0x000DU, PROTO_TYPE_QUERY_RES, CMD_COM_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x01, p_pl[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p_pl[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, p_pl[4]);   /* 115200 = 0x0001C200 */
    TEST_ASSERT_EQUAL_HEX8(0xC2, p_pl[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p_pl[6]);
    TEST_ASSERT_EQUAL_HEX8(0x00, p_pl[7]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p_pl[8]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p_pl[27]);
    TEST_ASSERT_EQUAL_HEX8(0xDA, p_pl[28]);  /* 218 */
    TEST_ASSERT_EQUAL_HEX8(0x00, p_pl[29]);

    /* 设置不回复（5.4 行为） */
    TEST_ASSERT_EQUAL(CMD_RES_NO_REPLY,
                      send_cmd(LINK_ROLE_PHONE, 0x000DU, 0U, PROTO_TYPE_SET,
                               CMD_COM_LEN, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, g_reply.len);
}

static void
test_radio_0x14 (void)
{
    uint8_t const   fail[2] = {0x02, 0x02};
    uint8_t const * p_pl;

    (void) send_cmd(LINK_ROLE_PHONE, 0x0014U, 0U, PROTO_TYPE_QUERY, 0U, 0U);
    p_pl = check_reply(0x0014U, PROTO_TYPE_QUERY_RES, CMD_RADIO_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x05, p_pl[17]);

    g_payload[0]  = 0x02;
    g_payload[14] = 0x04;
    (void) send_cmd(LINK_ROLE_PHONE, 0x0014U, 0U, PROTO_TYPE_SET,
                    CMD_RADIO_SET_MIN, 0U);
    (void) check_reply(0x0014U, PROTO_TYPE_SET_RES, 2U);
    (void) send_cmd(LINK_ROLE_PHONE, 0x0014U, 0U, PROTO_TYPE_QUERY, 0U, 0U);
    p_pl = check_reply(0x0014U, PROTO_TYPE_QUERY_RES, CMD_RADIO_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x02, p_pl[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, p_pl[14]);
    TEST_ASSERT_EQUAL_HEX8(0x05, p_pl[17]); /* 未提供则保持 */

    (void) send_cmd(LINK_ROLE_PHONE, 0x0014U, 0U, PROTO_TYPE_SET,
                    CMD_RADIO_SET_MIN - 1U, 0U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fail, check_reply(0x0014U,
                                                   PROTO_TYPE_SET_RES, 2U),
                                 2U);
}

static void
test_log_0x15 (void)
{
    uint8_t const fail[2] = {0x02, 0x02};

    g_payload[38] = 0x77;
    (void) send_cmd(LINK_ROLE_PHONE, 0x0015U, 0U, PROTO_TYPE_SET, CMD_LOG_LEN,
                    0U);
    (void) check_reply(0x0015U, PROTO_TYPE_SET_RES, 2U);
    (void) send_cmd(LINK_ROLE_PHONE, 0x0015U, 0U, PROTO_TYPE_QUERY, 0U, 0U);
    TEST_ASSERT_EQUAL_HEX8(0x77, check_reply(0x0015U, PROTO_TYPE_QUERY_RES,
                                             CMD_LOG_LEN)[38]);
    (void) send_cmd(LINK_ROLE_PHONE, 0x0015U, 0U, PROTO_TYPE_SET,
                    CMD_LOG_LEN - 1U, 0U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fail, check_reply(0x0015U,
                                                   PROTO_TYPE_SET_RES, 2U),
                                 2U);
}

static void
test_reserved_and_unknown_no_reply (void)
{
    uint16_t const ids[] = {0x001EU, 0x0021U, 0x0025U, 0x0028U, 0x0034U,
                            0x0099U};

    for (size_t idx = 0U; idx < (sizeof(ids) / sizeof(ids[0])); idx++)
    {
        TEST_ASSERT_EQUAL(CMD_RES_NO_REPLY,
                          send_cmd(LINK_ROLE_PHONE, ids[idx], 0U,
                                   PROTO_TYPE_QUERY, 0U, 0U));
        TEST_ASSERT_EQUAL_UINT(0U, g_reply.len);
    }
}

static void
test_alarm_role_cannot_use_phone_cmds (void)
{
    TEST_ASSERT_EQUAL(CMD_RES_DENIED,
                      send_cmd(LINK_ROLE_ALARM, 0x0001U, PROTO_SENDER_ALARM,
                               PROTO_TYPE_QUERY, 0U, 0U));
    TEST_ASSERT_EQUAL(CMD_RES_DENIED,
                      send_cmd(LINK_ROLE_ALARM, ALARM_MSG_TEST_ID,
                               PROTO_SENDER_ALARM, PROTO_TYPE_SET, 3U, 0U));
    TEST_ASSERT_EQUAL(CMD_RES_DENIED,
                      send_cmd(LINK_ROLE_PENDING, 0x0001U, 0U,
                               PROTO_TYPE_QUERY, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, g_reply.len);
}

static void
test_heartbeat_0x41_from_alarm (void)
{
    g_payload[0] = 0x07;
    g_payload[2] = 55U;
    g_payload[3] = 2U;
    TEST_ASSERT_EQUAL(CMD_RES_NO_REPLY,
                      send_cmd(LINK_ROLE_ALARM, ALARM_MSG_HEARTBEAT_ID,
                               PROTO_SENDER_ALARM, PROTO_TYPE_PUSH,
                               ALARM_HEARTBEAT_LEN, 0U));
    TEST_ASSERT_TRUE(g_reply.b_heartbeat);
    TEST_ASSERT_EQUAL_HEX16(0x0007U, g_reply.hb.seq);
    TEST_ASSERT_EQUAL_UINT8(55U, g_reply.hb.battery_pct);
    TEST_ASSERT_EQUAL_UINT8(2U, g_reply.hb.local_state);
    TEST_ASSERT_EQUAL_UINT(0U, g_reply.len);
}

static void
test_test_override_0x42_and_status_query (void)
{
    link_actions_t  act;
    uint8_t const * p_pl;

    (void) link_mgr_on_open(&g_link, H_PHONE, &g_phone_addr, 0U, &act);
    g_payload[0] = 1U;
    g_payload[1] = ALARM_STATE_ALERT;
    g_payload[2] = 3U;
    TEST_ASSERT_EQUAL(CMD_RES_REPLY,
                      send_cmd(LINK_ROLE_PHONE, ALARM_MSG_TEST_ID, 0U,
                               PROTO_TYPE_SET, 3U, 0U));
    (void) check_reply(ALARM_MSG_TEST_ID, PROTO_TYPE_SET_RES, 2U);
    TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_ALERT, alarm_src_state(&g_alarm));

    /* 报警器查询 0x40 */
    TEST_ASSERT_EQUAL(CMD_RES_REPLY,
                      send_cmd(LINK_ROLE_ALARM, ALARM_MSG_STATUS_ID,
                               PROTO_SENDER_ALARM, PROTO_TYPE_QUERY, 0U,
                               0x01020304U));
    p_pl = check_reply(ALARM_MSG_STATUS_ID, PROTO_TYPE_QUERY_RES,
                       ALARM_STATUS_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x00, p_pl[0]);
    TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_ALERT, p_pl[2]);
    TEST_ASSERT_EQUAL_HEX8(3U, p_pl[3]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, p_pl[4]);
    TEST_ASSERT_EQUAL_HEX8(ALARM_FLAG_PHONE_ONLINE | ALARM_FLAG_TEST_OVERRIDE,
                           p_pl[6]);
    TEST_ASSERT_EQUAL_HEX8(0x04, p_pl[8]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p_pl[11]);

    /* 非法状态 */
    g_payload[1] = 0x09;
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_TEST_ID, 0U, PROTO_TYPE_SET,
                    3U, 0U);
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(ALARM_MSG_TEST_ID,
                                             PROTO_TYPE_SET_RES, 2U)[0]);
    /* 清除覆盖 */
    g_payload[0] = 0U;
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_TEST_ID, 0U, PROTO_TYPE_SET,
                    3U, 0U);
    TEST_ASSERT_FALSE(alarm_src_is_override(&g_alarm));
}

static void
test_fence_0x43_reserved (void)
{
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_FENCE_ID, 0U, PROTO_TYPE_SET,
                    8U, 0U);
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(ALARM_MSG_FENCE_ID,
                                             PROTO_TYPE_SET_RES, 2U)[0]);
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_FENCE_ID, 0U, PROTO_TYPE_QUERY,
                    0U, 0U);
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(ALARM_MSG_FENCE_ID,
                                             PROTO_TYPE_QUERY_RES, 2U)[0]);
}

static void
test_pairing_0x44 (void)
{
    link_actions_t  act;
    uint8_t const * p_pl;

    g_payload[0] = 1U;
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_PAIR_ID, 0U, PROTO_TYPE_SET,
                    1U, 1000U);
    (void) check_reply(ALARM_MSG_PAIR_ID, PROTO_TYPE_SET_RES, 2U);
    TEST_ASSERT_TRUE(link_mgr_is_pairing(&g_link));

    /* 报警器 HELLO 加入白名单 */
    (void) link_mgr_on_open(&g_link, H_ALARM, &g_alarm_addr, 1100U, &act);
    (void) link_mgr_on_frame(&g_link, H_ALARM, PROTO_SENDER_ALARM,
                             ALARM_MSG_HEARTBEAT_ID, 1200U, &act);
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_PAIR_ID, 0U, PROTO_TYPE_QUERY,
                    0U, 1300U);
    p_pl = check_reply(ALARM_MSG_PAIR_ID, PROTO_TYPE_QUERY_RES, 7U);
    TEST_ASSERT_EQUAL_UINT8(1U, p_pl[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(g_alarm_addr.addr, &p_pl[1], 6U);

    /* 清空 */
    g_payload[0] = 0U;
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_PAIR_ID, 0U, PROTO_TYPE_SET,
                    1U, 1400U);
    TEST_ASSERT_TRUE(g_reply.b_whitelist_changed);
    (void) send_cmd(LINK_ROLE_PHONE, ALARM_MSG_PAIR_ID, 0U, PROTO_TYPE_QUERY,
                    0U, 1500U);
    TEST_ASSERT_EQUAL_UINT8(0U, check_reply(ALARM_MSG_PAIR_ID,
                                            PROTO_TYPE_QUERY_RES, 1U)[0]);
}

static void
test_status_push (void)
{
    uint8_t const * p_pl;
    static proto_parser_t parser;
    proto_frame_t         frame;
    int                   frames = 0;

    (void) alarm_src_set_override(&g_alarm, ALARM_STATE_NORMAL, 0U);
    g_ctx.info.battery_pct = 90U;
    TEST_ASSERT_TRUE(cmd_build_status_push(&g_ctx, 0xBEEFU, 5000U, &g_reply));
    proto_parser_init(&parser);
    for (size_t idx = 0U; idx < g_reply.len; idx++)
    {
        if (PROTO_PARSE_FRAME ==
            proto_parser_feed(&parser, g_reply.buf[idx], &frame))
        {
            frames++;
            p_pl = frame.p_payload;
            TEST_ASSERT_EQUAL_HEX16(ALARM_MSG_STATUS_ID, frame.msg_id);
            TEST_ASSERT_EQUAL_HEX8(PROTO_TYPE_PUSH, frame.type);
            TEST_ASSERT_EQUAL_HEX8(PROTO_SENDER_DEV, frame.sender);
            TEST_ASSERT_EQUAL_HEX8(0xEF, p_pl[0]);
            TEST_ASSERT_EQUAL_HEX8(0xBE, p_pl[1]);
            TEST_ASSERT_EQUAL_HEX8(ALARM_STATE_NORMAL, p_pl[2]);
            TEST_ASSERT_EQUAL_UINT8(90U, p_pl[5]);
            TEST_ASSERT_EQUAL_HEX8(ALARM_FLAG_TEST_OVERRIDE, p_pl[6]);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, frames);
}

static void
test_null_args (void)
{
    proto_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    TEST_ASSERT_EQUAL(CMD_RES_NO_REPLY,
                      cmd_disp_handle(NULL, LINK_ROLE_PHONE, &frame, 0U,
                                      &g_reply));
    TEST_ASSERT_EQUAL(CMD_RES_NO_REPLY,
                      cmd_disp_handle(&g_ctx, LINK_ROLE_PHONE, NULL, 0U,
                                      &g_reply));
    TEST_ASSERT_EQUAL(CMD_RES_NO_REPLY,
                      cmd_disp_handle(&g_ctx, LINK_ROLE_PHONE, &frame, 0U,
                                      NULL));
}

int
main (void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dev_info_0x01);
    RUN_TEST(test_dev_state_0x02_constant);
    RUN_TEST(test_battery_0x03);
    RUN_TEST(test_placeholders_0x04_0x05_0x06);
    RUN_TEST(test_work_mode_0x07_roundtrip_and_short);
    RUN_TEST(test_com_0x0d);
    RUN_TEST(test_radio_0x14);
    RUN_TEST(test_log_0x15);
    RUN_TEST(test_reserved_and_unknown_no_reply);
    RUN_TEST(test_alarm_role_cannot_use_phone_cmds);
    RUN_TEST(test_heartbeat_0x41_from_alarm);
    RUN_TEST(test_test_override_0x42_and_status_query);
    RUN_TEST(test_fence_0x43_reserved);
    RUN_TEST(test_pairing_0x44);
    RUN_TEST(test_status_push);
    RUN_TEST(test_null_args);
    return UNITY_END();
}
