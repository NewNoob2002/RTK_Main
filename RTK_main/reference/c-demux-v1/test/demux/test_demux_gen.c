// 测试辅助 demux_gen 自身已知向量与边界测试。
// Generated-by: pi + antigravity/gemini-3.8-flash，人工负责人：UNKNOWN（待人工审查签字）
#include <string.h>

#include "demux_gen.h"
#include "rtk_proto_frame.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_crc24q_known_vector(void)
{
    static const uint8_t s_vec[] = "123456789";

    TEST_ASSERT_EQUAL_HEX32(0xCDE703U, demux_gen_crc24q(s_vec, 9U));
    TEST_ASSERT_EQUAL_HEX32(0U, demux_gen_crc24q(NULL, 0U));
    TEST_ASSERT_EQUAL_HEX32(0U, demux_gen_crc24q(s_vec, 0U));
}

static void test_crc32_known_vectors(void)
{
    static const uint8_t s_vec[] = "123456789";
    static const char s_unicore_ver[] = "VERSION,97,GPS,FINE,2282,248561000,0,0,18,676;UM980,R4.10Build7923,"
                                        "HRPT00-S10C-P,2310415000001-MD22B1224962616,ff3bac96f31f9bdd,2022/09/28";

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, demux_gen_crc32_ieee(s_vec, 9U));
    TEST_ASSERT_EQUAL_HEX32(0x2DFD2D88U, demux_gen_crc32_novatel(s_vec, 9U));
    TEST_ASSERT_EQUAL_HEX32(0x7432D4EDU,
                            demux_gen_crc32_novatel((const uint8_t *)s_unicore_ver, strlen(s_unicore_ver)));
    TEST_ASSERT_EQUAL_HEX32(0U, demux_gen_crc32_ieee(NULL, 0U));
    TEST_ASSERT_EQUAL_HEX32(0U, demux_gen_crc32_novatel(NULL, 0U));
}

static void test_xor_and_ubx_ck(void)
{
    static const uint8_t s_nmea[] = "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
    static const uint8_t s_ubx_body[] = {0x06U, 0x01U, 0x03U, 0x00U, 0xF0U, 0x00U, 0x01U};
    uint8_t ck_a = 0U;
    uint8_t ck_b = 0U;

    TEST_ASSERT_EQUAL_HEX8(0x47U, demux_gen_xor(s_nmea, sizeof(s_nmea) - 1U));
    demux_gen_ubx_ck(s_ubx_body, sizeof(s_ubx_body), &ck_a, &ck_b);
    TEST_ASSERT_EQUAL_HEX8(0xFBU, ck_a);
    TEST_ASSERT_EQUAL_HEX8(0x10U, ck_b);
}

static void test_generators_sanity_and_bounds(void)
{
    static uint8_t s_buf[4100];
    static const uint8_t s_payload[] = {0x11U, 0x22U, 0x33U};
    size_t len = 0U;

    /* RTCM3 */
    TEST_ASSERT_TRUE(demux_gen_rtcm3(s_buf, sizeof(s_buf), s_payload, sizeof(s_payload), &len));
    TEST_ASSERT_EQUAL_UINT(9U, len);
    TEST_ASSERT_EQUAL_HEX8(0xD3U, s_buf[0]);
    TEST_ASSERT_FALSE(demux_gen_rtcm3(s_buf, sizeof(s_buf), s_payload, DEMUX_GEN_RTCM3_MAX_PAYLOAD + 1U, &len));
    TEST_ASSERT_FALSE(demux_gen_rtcm3(s_buf, 8U, s_payload, sizeof(s_payload), &len));

    /* NMEA（含大小写校验） */
    TEST_ASSERT_TRUE(demux_gen_nmea(s_buf, sizeof(s_buf),
                                    "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", false, &len));
    TEST_ASSERT_EQUAL_STRING_LEN("*47\r\n", (const char *)&s_buf[len - 5U], 5U);
    TEST_ASSERT_FALSE(demux_gen_nmea(s_buf, sizeof(s_buf), "@BAD", false, &len));

    /* UBX */
    TEST_ASSERT_TRUE(demux_gen_ubx(s_buf, sizeof(s_buf), 0x01U, 0x07U, s_payload, sizeof(s_payload), &len));
    TEST_ASSERT_EQUAL_UINT(11U, len);
    TEST_ASSERT_FALSE(
        demux_gen_ubx(s_buf, sizeof(s_buf), 0x01U, 0x07U, s_payload, DEMUX_GEN_UBX_MAX_PAYLOAD + 1U, &len));

    /* Unicore 二进制 */
    TEST_ASSERT_TRUE(
        demux_gen_unicore_bin(s_buf, sizeof(s_buf), 0x002AU, 55U, NULL, s_payload, sizeof(s_payload), &len));
    TEST_ASSERT_EQUAL_UINT(31U, len);
    TEST_ASSERT_FALSE(demux_gen_unicore_bin(s_buf, sizeof(s_buf), 0x002AU, 55U, NULL, s_payload,
                                            DEMUX_GEN_UNICORE_BIN_MAX_PAYLOAD + 1U, &len));

    /* Unicore Hash CRC-32（对照真实 VERSION 向量） */
    TEST_ASSERT_TRUE(
        demux_gen_unicore_hash_crc32(s_buf, sizeof(s_buf),
                                     "VERSION,97,GPS,FINE,2282,248561000,0,0,18,676;UM980,R4.10Build7923,"
                                     "HRPT00-S10C-P,2310415000001-MD22B1224962616,ff3bac96f31f9bdd,2022/09/28",
                                     true, &len));
    TEST_ASSERT_EQUAL_STRING_LEN("*7432d4ed\r\n", (const char *)&s_buf[len - 11U], 11U);

    /* Unicore Hash XOR */
    TEST_ASSERT_TRUE(demux_gen_unicore_hash_xor(s_buf, sizeof(s_buf), "OBSVMA,1", false, &len));
    TEST_ASSERT_EQUAL_UINT(14U, len);
    TEST_ASSERT_FALSE(demux_gen_unicore_hash_xor(s_buf, sizeof(s_buf), "0BAD", false, &len));

    /* 产品帧 */
    TEST_ASSERT_TRUE(
        demux_gen_product(s_buf, sizeof(s_buf), 0x0001U, RTK_PROTO_SENDER_APP, RTK_PROTO_TYPE_QUERY, NULL, 0U, &len));
    TEST_ASSERT_EQUAL_UINT(24U, len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc24q_known_vector);
    RUN_TEST(test_crc32_known_vectors);
    RUN_TEST(test_xor_and_ubx_ck);
    RUN_TEST(test_generators_sanity_and_bounds);
    return UNITY_END();
}
