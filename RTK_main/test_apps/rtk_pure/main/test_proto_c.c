/* T15：纯 C 公共接口冒烟测试；全部状态由当前用例独占，无堆分配。
 */
#include "dev_proto_frame.h"
#include "dev_proto_alarm.h"
#include "unity.h"

/* 来源：dev_proto_frame.h 的 CRC 契约、test/host/test_proto_frame_c.c 的已知向量。 */
TEST_CASE("C CRC32 known vector and segmented accumulation", "[rtk_pure][crc]")
{
    const uint8_t data[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, dev_proto_crc32(0U, data, 9U));
    const uint32_t prefix = dev_proto_crc32(0U, data, 4U);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, dev_proto_crc32(prefix, &data[4], 5U));
}

/* 来源：dev_proto_frame.h，NULL 或零长度均返回传入的 CRC。 */
TEST_CASE("C CRC32 empty and null input preserve seed", "[rtk_pure][crc]")
{
    const uint8_t data[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0x12345678U, dev_proto_crc32(0x12345678U, data, 0U));
    TEST_ASSERT_EQUAL_HEX32(0x12345678U, dev_proto_crc32(0x12345678U, NULL, 9U));
    TEST_ASSERT_EQUAL_HEX32(0U, dev_proto_crc32(0U, NULL, 0U));
}

/* 来源：host 的 test_build_set_response；固定线路向量同时约束编码与解析，避免互相掩盖错误。 */
TEST_CASE("C frame set response round trip matches frozen vector", "[rtk_pure][frame]")
{
    const uint8_t payload[] = {0x01, 0x01};
    const uint8_t expected[] = {0xAA, 0x44, 0x18, 0x14, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
                                0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x01, 0x01, 0xC0, 0x20, 0xDA, 0x74};
    uint8_t out[sizeof(expected)] = {0};
    size_t out_len = 0U;
    dev_proto_parser_t parser = {0};
    dev_proto_frame_t frame = {0};
    dev_proto_parser_init(&parser);
    TEST_ASSERT_EQUAL(DEV_PROTO_OK, dev_proto_frame_build(out, sizeof(out), 0x0004U, DEV_PROTO_SENDER_DEV,
                                                          DEV_PROTO_TYPE_SET_RES, payload, 2U, &out_len));
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
    for (size_t i = 0U; i < out_len; ++i) {
        const dev_proto_parse_result_t result = dev_proto_parser_feed(&parser, out[i], &frame);
        TEST_ASSERT_EQUAL(i + 1U == out_len ? DEV_PROTO_PARSE_FRAME : DEV_PROTO_PARSE_NONE, result);
    }
    TEST_ASSERT_EQUAL_HEX16(0x0004U, frame.msg_id);
    TEST_ASSERT_EQUAL_HEX8(DEV_PROTO_SENDER_DEV, frame.sender);
    TEST_ASSERT_EQUAL_HEX8(DEV_PROTO_TYPE_SET_RES, frame.type);
    TEST_ASSERT_EQUAL_HEX8(0U, frame.protocol);
    TEST_ASSERT_EQUAL_HEX8(0U, frame.interval);
    TEST_ASSERT_EQUAL_UINT16(2U, frame.payload_len);
    TEST_ASSERT_NOT_NULL(frame.payload);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, frame.payload, sizeof(payload));
}

/* 来源：C 公共头的空 payload/空间契约，以及 host 的 g_frame_a。 */
TEST_CASE("C empty frame exact capacity and short buffer", "[rtk_pure][frame]")
{
    const uint8_t expected[] = {0xAA, 0x44, 0x18, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x69, 0x17, 0x6E, 0x7E};
    uint8_t out[sizeof(expected)] = {0};
    size_t out_len = 0xABCDU;
    TEST_ASSERT_EQUAL(DEV_PROTO_ERR_SPACE, dev_proto_frame_build(out, sizeof(out) - 1U, 0x0001U, DEV_PROTO_SENDER_APP,
                                                                 DEV_PROTO_TYPE_QUERY, NULL, 0U, &out_len));
    TEST_ASSERT_EQUAL_UINT(0xABCDU, out_len);
    TEST_ASSERT_EQUAL(DEV_PROTO_OK, dev_proto_frame_build(out, sizeof(out), 0x0001U, DEV_PROTO_SENDER_APP,
                                                          DEV_PROTO_TYPE_QUERY, NULL, 0U, &out_len));
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), out_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
    dev_proto_parser_t parser = {0};
    dev_proto_frame_t frame = {0};
    dev_proto_parser_init(&parser);
    for (size_t i = 0U; i < out_len; ++i) {
        const dev_proto_parse_result_t result = dev_proto_parser_feed(&parser, out[i], &frame);
        TEST_ASSERT_EQUAL(i + 1U == out_len ? DEV_PROTO_PARSE_FRAME : DEV_PROTO_PARSE_NONE, result);
    }
    TEST_ASSERT_EQUAL_HEX16(0x0001U, frame.msg_id);
    TEST_ASSERT_EQUAL_HEX8(DEV_PROTO_SENDER_APP, frame.sender);
    TEST_ASSERT_EQUAL_HEX8(DEV_PROTO_TYPE_QUERY, frame.type);
    TEST_ASSERT_EQUAL_UINT16(0U, frame.payload_len);
}

/* 来源：dev_proto_frame.h 与 host 的 test_parse_crc_error_then_recover。 */
TEST_CASE("C corrupted frame rejected then parser recovers", "[rtk_pure][frame]")
{
    const uint8_t payload[] = {0x01, 0x01};
    uint8_t out[DEV_PROTO_HEADER_LEN + sizeof(payload) + DEV_PROTO_CRC_LEN] = {0};
    size_t out_len = 0U;
    TEST_ASSERT_EQUAL(DEV_PROTO_OK, dev_proto_frame_build(out, sizeof(out), 0x0004U, DEV_PROTO_SENDER_DEV,
                                                          DEV_PROTO_TYPE_SET_RES, payload, 2U, &out_len));
    dev_proto_parser_t parser = {0};
    dev_proto_frame_t frame = {0};
    dev_proto_parser_init(&parser);
    out[DEV_PROTO_HEADER_LEN] ^= 0xFFU;
    for (size_t i = 0U; i < out_len; ++i) {
        const dev_proto_parse_result_t result = dev_proto_parser_feed(&parser, out[i], &frame);
        TEST_ASSERT_EQUAL(i + 1U == out_len ? DEV_PROTO_PARSE_CRC_ERR : DEV_PROTO_PARSE_NONE, result);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, parser.crc_errors);
    out[DEV_PROTO_HEADER_LEN] ^= 0xFFU;
    for (size_t i = 0U; i < out_len; ++i) {
        const dev_proto_parse_result_t result = dev_proto_parser_feed(&parser, out[i], &frame);
        TEST_ASSERT_EQUAL(i + 1U == out_len ? DEV_PROTO_PARSE_FRAME : DEV_PROTO_PARSE_NONE, result);
    }
    TEST_ASSERT_EQUAL_UINT16(2U, frame.payload_len);
    TEST_ASSERT_NOT_NULL(frame.payload);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, frame.payload, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(1U, parser.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0U, parser.len_errors);
}

/* 来源：host 的 test_encode_heartbeat；dev_proto_alarm.h 规定后四字节为零。 */
TEST_CASE("C heartbeat 0x41 layout and round trip", "[rtk_pure][heartbeat]")
{
    const dev_proto_alarm_heartbeat_t heartbeat = {.seq = 0x0005U, .battery_pct = 80U, .local_state = 0U};
    const uint8_t expected[DEV_PROTO_ALARM_HEARTBEAT_LEN] = {0x05, 0x00, 0x50, 0, 0, 0, 0, 0};
    uint8_t out[DEV_PROTO_ALARM_HEARTBEAT_LEN] = {0};
    dev_proto_alarm_heartbeat_t decoded = {0};
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), dev_proto_alarm_encode_heartbeat(&heartbeat, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
    TEST_ASSERT_TRUE(dev_proto_alarm_decode_heartbeat(out, sizeof(out), &decoded));
    TEST_ASSERT_EQUAL_HEX16(0x0005U, decoded.seq);
    TEST_ASSERT_EQUAL_UINT8(80U, decoded.battery_pct);
    TEST_ASSERT_EQUAL_UINT8(0U, decoded.local_state);
}

/* 来源：dev_proto_alarm.h 的参数错误契约及 host 的心跳编码/解码边界用例。 */
TEST_CASE("C heartbeat short buffers and null pointers rejected", "[rtk_pure][heartbeat]")
{
    const dev_proto_alarm_heartbeat_t heartbeat = {.seq = 0x0005U, .battery_pct = 80U, .local_state = 0U};
    uint8_t out[DEV_PROTO_ALARM_HEARTBEAT_LEN] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    const uint8_t expected[DEV_PROTO_ALARM_HEARTBEAT_LEN] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    dev_proto_alarm_heartbeat_t decoded = {0};
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_heartbeat(&heartbeat, out, sizeof(out) - 1U));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_heartbeat(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0U, dev_proto_alarm_encode_heartbeat(&heartbeat, NULL, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(out));
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_heartbeat(out, sizeof(out) - 1U, &decoded));
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_heartbeat(NULL, sizeof(out), &decoded));
    TEST_ASSERT_FALSE(dev_proto_alarm_decode_heartbeat(out, sizeof(out), NULL));
}
