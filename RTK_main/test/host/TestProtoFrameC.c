/* 验收测试：device_proto C 接口（帧）。由 QA-A 通过的 reference/c-v1 测试机械移植；
 * 期望值来自 phone_protocol.md 与 Python zlib.crc32。冻结输入。 */
#include <string.h>

#include "DevProtoFrame.h"
#include "unity.h"

/* App 查询 0x0001，无 payload */
static uint8_t const g_frame_a[] = {0xAA, 0x44, 0x18, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x69, 0x17, 0x6E, 0x7E};

/* 设备设置应答 0x0004，payload 01 01 */
static uint8_t const g_frame_b[] = {0xAA, 0x44, 0x18, 0x14, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
                                    0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x01, 0x01, 0xC0, 0x20, 0xDA, 0x74};

/* 报警器心跳 0x0041，sender 3，type PUSH，payload 05 00 50 00 00 00 00 00 */
static uint8_t const g_frame_c[] = {0xAA, 0x44, 0x18, 0x14, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x08, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x00, 0x05, 0x00,
                                    0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x34, 0x05, 0xAA};

static dev_proto_parser_t g_parser;

void setUp(void)
{
    dev_proto_parser_init(&g_parser);
}

void tearDown(void) {}

/* 喂入整段数据，返回得到的帧数，最后一帧写入 p_last。 */
static int feed_all(uint8_t const *p_data, size_t len, dev_proto_frame_t *p_last, int *p_crc_err, int *p_len_err)
{
    int frames = 0;

    for (size_t idx = 0U; idx < len; idx++) {
        dev_proto_frame_t frame;
        dev_proto_parse_result_t res;

        res = dev_proto_parser_feed(&g_parser, p_data[idx], &frame);
        if (DEV_PROTO_PARSE_FRAME == res) {
            frames++;
            *p_last = frame;
        } else if (DEV_PROTO_PARSE_CRC_ERR == res) {
            (*p_crc_err)++;
        } else if (DEV_PROTO_PARSE_LEN_ERR == res) {
            (*p_len_err)++;
        } else {
            /* 需要更多字节 */
        }
    }
    return frames;
}

static void test_crc32_check_value(void)
{
    uint8_t const msg[] = "123456789";

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, dev_proto_crc32(0U, msg, 9U));
    /* 分段累计 */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, dev_proto_crc32(dev_proto_crc32(0U, msg, 4U), &msg[4], 5U));
    TEST_ASSERT_EQUAL_HEX32(0x12345678U, dev_proto_crc32(0x12345678U, NULL, 0U));
}

static void test_build_query_frame(void)
{
    uint8_t out[64];
    size_t out_len = 0U;

    TEST_ASSERT_EQUAL(DEV_PROTO_OK, dev_proto_frame_build(out, sizeof(out), 0x0001U, DEV_PROTO_SENDER_APP,
                                                          DEV_PROTO_TYPE_QUERY, NULL, 0U, &out_len));
    TEST_ASSERT_EQUAL_UINT(sizeof(g_frame_a), out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(g_frame_a, out, sizeof(g_frame_a));
}

static void test_build_set_response(void)
{
    uint8_t const payload[] = {0x01, 0x01};
    uint8_t out[64];
    size_t out_len = 0U;

    TEST_ASSERT_EQUAL(DEV_PROTO_OK, dev_proto_frame_build(out, sizeof(out), 0x0004U, DEV_PROTO_SENDER_DEV,
                                                          DEV_PROTO_TYPE_SET_RES, payload, 2U, &out_len));
    TEST_ASSERT_EQUAL_UINT(sizeof(g_frame_b), out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(g_frame_b, out, sizeof(g_frame_b));
}

static void test_build_rejects_bad_args(void)
{
    static uint8_t big[DEV_PROTO_MAX_PAYLOAD + 1U];
    uint8_t out[DEV_PROTO_MAX_FRAME + 8U];
    size_t out_len = 0xABCDU;

    /* 空间正好差 1 字节 */
    TEST_ASSERT_EQUAL(DEV_PROTO_ERR_SPACE,
                      dev_proto_frame_build(out, sizeof(g_frame_b) - 1U, 0x0004U, DEV_PROTO_SENDER_DEV,
                                            DEV_PROTO_TYPE_SET_RES, big, 2U, &out_len));
    TEST_ASSERT_EQUAL(DEV_PROTO_ERR_ARG,
                      dev_proto_frame_build(out, sizeof(out), 0x0004U, DEV_PROTO_SENDER_DEV, DEV_PROTO_TYPE_SET_RES,
                                            big, DEV_PROTO_MAX_PAYLOAD + 1U, &out_len));
    TEST_ASSERT_EQUAL(DEV_PROTO_ERR_ARG, dev_proto_frame_build(NULL, sizeof(out), 0x0004U, DEV_PROTO_SENDER_DEV,
                                                               DEV_PROTO_TYPE_SET_RES, big, 2U, &out_len));
    TEST_ASSERT_EQUAL(DEV_PROTO_ERR_ARG, dev_proto_frame_build(out, sizeof(out), 0x0004U, DEV_PROTO_SENDER_DEV,
                                                               DEV_PROTO_TYPE_SET_RES, NULL, 2U, &out_len));
    TEST_ASSERT_EQUAL_UINT(0xABCDU, out_len);
    /* 最大 payload 正好可以组帧 */
    TEST_ASSERT_EQUAL(DEV_PROTO_OK,
                      dev_proto_frame_build(out, DEV_PROTO_MAX_FRAME, 0x0004U, DEV_PROTO_SENDER_DEV,
                                            DEV_PROTO_TYPE_SET_RES, big, DEV_PROTO_MAX_PAYLOAD, &out_len));
    TEST_ASSERT_EQUAL_UINT(DEV_PROTO_MAX_FRAME, out_len);
}

static void test_response_type_mapping(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x01U, dev_proto_response_type(0x00U));
    TEST_ASSERT_EQUAL_HEX8(0x03U, dev_proto_response_type(0x02U));
    TEST_ASSERT_EQUAL_HEX8(0x05U, dev_proto_response_type(0x04U));
    TEST_ASSERT_EQUAL_HEX8(0x05U, dev_proto_response_type(0x01U));
}

static void test_parse_single_frame(void)
{
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    memset(&last, 0, sizeof(last));
    TEST_ASSERT_EQUAL_INT(1, feed_all(g_frame_c, sizeof(g_frame_c), &last, &crc_err, &len_err));
    TEST_ASSERT_EQUAL_HEX16(0x0041U, last.msg_id);
    TEST_ASSERT_EQUAL_HEX8(0x03U, last.sender);
    TEST_ASSERT_EQUAL_HEX8(0x04U, last.type);
    TEST_ASSERT_EQUAL_UINT16(8U, last.payload_len);
    TEST_ASSERT_NOT_NULL(last.payload);
    TEST_ASSERT_EQUAL_HEX8(0x05U, last.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0x50U, last.payload[2]);
    TEST_ASSERT_EQUAL_INT(0, crc_err);
    TEST_ASSERT_EQUAL_INT(0, len_err);
}

static void test_parse_garbage_and_back_to_back(void)
{
    uint8_t stream[128];
    size_t len = 0U;
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    /* 噪声 + 假同步 AA AA 44（第二个 AA 必须作为新起点）+ 两帧连续 */
    stream[len++] = 0x00;
    stream[len++] = 0xAA;
    stream[len++] = 0x13;
    stream[len++] = 0xAA;
    memcpy(&stream[len], g_frame_a, sizeof(g_frame_a));
    len += sizeof(g_frame_a);
    memcpy(&stream[len], g_frame_b, sizeof(g_frame_b));
    len += sizeof(g_frame_b);

    TEST_ASSERT_EQUAL_INT(2, feed_all(stream, len, &last, &crc_err, &len_err));
    TEST_ASSERT_EQUAL_HEX16(0x0004U, last.msg_id);
    TEST_ASSERT_EQUAL_INT(0, crc_err);
}

static void test_parse_aa_aa_resync(void)
{
    uint8_t stream[64];
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    stream[0] = 0xAA;
    memcpy(&stream[1], g_frame_a, sizeof(g_frame_a));
    TEST_ASSERT_EQUAL_INT(1, feed_all(stream, sizeof(g_frame_a) + 1U, &last, &crc_err, &len_err));
}

static void test_parse_split_across_feeds(void)
{
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    /* 分三段到达，解析器状态必须保留（修正 5.4 每轮重建解析器的问题） */
    TEST_ASSERT_EQUAL_INT(0, feed_all(g_frame_c, 3U, &last, &crc_err, &len_err));
    TEST_ASSERT_EQUAL_INT(0, feed_all(&g_frame_c[3], 20U, &last, &crc_err, &len_err));
    TEST_ASSERT_EQUAL_INT(1, feed_all(&g_frame_c[23], sizeof(g_frame_c) - 23U, &last, &crc_err, &len_err));
}

static void test_parse_crc_error_then_recover(void)
{
    uint8_t bad[sizeof(g_frame_b)];
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    memcpy(bad, g_frame_b, sizeof(bad));
    bad[20] ^= 0xFFU; /* 改 payload */
    TEST_ASSERT_EQUAL_INT(0, feed_all(bad, sizeof(bad), &last, &crc_err, &len_err));
    TEST_ASSERT_EQUAL_INT(1, crc_err);
    TEST_ASSERT_EQUAL_UINT32(1U, g_parser.crc_errors);
    TEST_ASSERT_EQUAL_INT(1, feed_all(g_frame_b, sizeof(g_frame_b), &last, &crc_err, &len_err));
}

static void test_parse_length_errors(void)
{
    uint8_t hdr[sizeof(g_frame_a)];
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    /* 头长字节非 0x14 */
    memcpy(hdr, g_frame_a, sizeof(hdr));
    hdr[3] = 0x15;
    (void)feed_all(hdr, 4U, &last, &crc_err, &len_err);
    TEST_ASSERT_EQUAL_INT(1, len_err);

    /* payload_len = 257 */
    dev_proto_parser_init(&g_parser);
    len_err = 0;
    memcpy(hdr, g_frame_a, sizeof(hdr));
    hdr[12] = 0x01;
    hdr[13] = 0x01;
    (void)feed_all(hdr, 20U, &last, &crc_err, &len_err);
    TEST_ASSERT_EQUAL_INT(1, len_err);
    TEST_ASSERT_EQUAL_UINT32(1U, g_parser.len_errors);

    /* 之后仍能收正常帧 */
    TEST_ASSERT_EQUAL_INT(1, feed_all(g_frame_a, sizeof(g_frame_a), &last, &crc_err, &len_err));
}

static void test_parse_max_payload(void)
{
    static uint8_t payload[DEV_PROTO_MAX_PAYLOAD];
    static uint8_t out[DEV_PROTO_MAX_FRAME];
    size_t out_len = 0U;
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    for (size_t idx = 0U; idx < sizeof(payload); idx++) {
        payload[idx] = (uint8_t)idx; /* idx < 256，截断安全 */
    }
    TEST_ASSERT_EQUAL(DEV_PROTO_OK,
                      dev_proto_frame_build(out, sizeof(out), 0x0007U, DEV_PROTO_SENDER_APP, DEV_PROTO_TYPE_SET,
                                            payload, DEV_PROTO_MAX_PAYLOAD, &out_len));
    TEST_ASSERT_EQUAL_INT(1, feed_all(out, out_len, &last, &crc_err, &len_err));
    TEST_ASSERT_EQUAL_UINT16(DEV_PROTO_MAX_PAYLOAD, last.payload_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, last.payload, DEV_PROTO_MAX_PAYLOAD);
}

static void test_parse_null_args(void)
{
    dev_proto_frame_t frame;

    TEST_ASSERT_EQUAL(DEV_PROTO_PARSE_NONE, dev_proto_parser_feed(NULL, 0xAA, &frame));
    TEST_ASSERT_EQUAL(DEV_PROTO_PARSE_NONE, dev_proto_parser_feed(&g_parser, 0xAA, NULL));
}

static void test_parser_reset_keeps_counters(void)
{
    uint8_t bad[sizeof(g_frame_b)];
    dev_proto_frame_t last;
    int crc_err = 0;
    int len_err = 0;

    memcpy(bad, g_frame_b, sizeof(bad));
    bad[20] ^= 0xFFU;
    (void)feed_all(bad, sizeof(bad), &last, &crc_err, &len_err);
    (void)feed_all(g_frame_a, 10U, &last, &crc_err, &len_err); /* 留下半帧 */
    dev_proto_parser_reset(&g_parser);
    TEST_ASSERT_EQUAL_UINT32(1U, g_parser.crc_errors); /* 计数保留 */
    TEST_ASSERT_EQUAL_INT(0, feed_all(&g_frame_a[10], sizeof(g_frame_a) - 10U, &last, &crc_err, &len_err));
    TEST_ASSERT_EQUAL_INT(1, feed_all(g_frame_a, sizeof(g_frame_a), &last, &crc_err, &len_err));
    dev_proto_parser_init(&g_parser);
    TEST_ASSERT_EQUAL_UINT32(0U, g_parser.crc_errors); /* init 清零 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_build_query_frame);
    RUN_TEST(test_build_set_response);
    RUN_TEST(test_build_rejects_bad_args);
    RUN_TEST(test_response_type_mapping);
    RUN_TEST(test_parse_single_frame);
    RUN_TEST(test_parse_garbage_and_back_to_back);
    RUN_TEST(test_parse_aa_aa_resync);
    RUN_TEST(test_parse_split_across_feeds);
    RUN_TEST(test_parse_crc_error_then_recover);
    RUN_TEST(test_parse_length_errors);
    RUN_TEST(test_parse_max_payload);
    RUN_TEST(test_parse_null_args);
    RUN_TEST(test_parser_reset_keeps_counters);
    return UNITY_END();
}
