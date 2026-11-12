// 测试辅助 demux_gen 及 demux::crc 已知向量与边界测试（ADR-0005）。
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/crc.hpp"
#include "demux_gen.hpp"
#include "rtk/proto/frame.hpp"
#include "unity.h"

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

static void test_crc24q_known_and_segmented()
{
    static constexpr std::array<uint8_t, 9> kVec{'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    TEST_ASSERT_EQUAL_HEX32(0xCDE703U, demux_test::crc24q(kVec));
    TEST_ASSERT_EQUAL_HEX32(0xCDE703U, demux::crc::crc24q(kVec));
    TEST_ASSERT_EQUAL_HEX32(0U, demux_test::crc24q({}));
    TEST_ASSERT_EQUAL_HEX32(0U, demux::crc::crc24q({}));
    TEST_ASSERT_EQUAL_HEX32(0x123456U, demux::crc::crc24q({}, 0x123456U));

    // 验证分段累计：f(b, f(a, init)) == f(a || b, init)
    const std::span<const uint8_t> full(kVec);
    for (std::size_t split = 0U; split <= kVec.size(); ++split) {
        const uint32_t part = demux::crc::crc24q(full.subspan(0, split));
        const uint32_t total = demux::crc::crc24q(full.subspan(split), part);
        TEST_ASSERT_EQUAL_HEX32(0xCDE703U, total);
    }
}

static void test_crc32_known_and_segmented()
{
    static constexpr std::array<uint8_t, 9> kVec{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    static constexpr std::string_view kUnicoreVer =
        "VERSION,97,GPS,FINE,2282,248561000,0,0,18,676;UM980,R4.10Build7923,"
        "HRPT00-S10C-P,2310415000001-MD22B1224962616,ff3bac96f31f9bdd,2022/09/28";
    static std::array<uint8_t, 256> s_ver_bytes{};

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, demux_test::crc32_ieee(kVec));
    TEST_ASSERT_EQUAL_HEX32(0x2DFD2D88U, demux_test::crc32_novatel(kVec));
    TEST_ASSERT_EQUAL_HEX32(0x2DFD2D88U, demux::crc::crc32_novatel(kVec));

    for (std::size_t i = 0U; i < kUnicoreVer.size(); ++i) {
        // ASCII 字符按无符号字节 0..255 解释
        s_ver_bytes[i] = static_cast<uint8_t>(kUnicoreVer[i]);
    }
    const std::span<const uint8_t> ver_span(s_ver_bytes.data(), kUnicoreVer.size());
    TEST_ASSERT_EQUAL_HEX32(0x7432D4EDU, demux_test::crc32_novatel(ver_span));
    TEST_ASSERT_EQUAL_HEX32(0x7432D4EDU, demux::crc::crc32_novatel(ver_span));

    TEST_ASSERT_EQUAL_HEX32(0U, demux_test::crc32_ieee({}));
    TEST_ASSERT_EQUAL_HEX32(0U, demux_test::crc32_novatel({}));
    TEST_ASSERT_EQUAL_HEX32(0U, demux::crc::crc32_novatel({}));
    TEST_ASSERT_EQUAL_HEX32(0x12345678U, demux::crc::crc32_novatel({}, 0x12345678U));

    // 验证 NovAtel CRC-32 分段累计
    const std::span<const uint8_t> full(kVec);
    for (std::size_t split = 0U; split <= kVec.size(); ++split) {
        const uint32_t part = demux::crc::crc32_novatel(full.subspan(0, split));
        const uint32_t total = demux::crc::crc32_novatel(full.subspan(split), part);
        TEST_ASSERT_EQUAL_HEX32(0x2DFD2D88U, total);
    }
}

static void test_crc_cross_check_random_bytes()
{
    std::array<uint8_t, 256> buf{};
    uint32_t rng = 0x13572468U;
    for (std::size_t i = 0U; i < buf.size(); ++i) {
        rng ^= rng << 13U;
        rng ^= rng >> 17U;
        rng ^= rng << 5U;
        // 掩码后在 0..255
        buf[i] = static_cast<uint8_t>(rng & 0xFFU);
    }

    const std::span<const uint8_t> data(buf);
    TEST_ASSERT_EQUAL_HEX32(demux_test::crc24q(data), demux::crc::crc24q(data));
    TEST_ASSERT_EQUAL_HEX32(demux_test::crc32_novatel(data), demux::crc::crc32_novatel(data));

    const uint32_t c24_p1 = demux::crc::crc24q(data.subspan(0, 77));
    TEST_ASSERT_EQUAL_HEX32(demux_test::crc24q(data), demux::crc::crc24q(data.subspan(77), c24_p1));

    const uint32_t c32_p1 = demux::crc::crc32_novatel(data.subspan(0, 129));
    TEST_ASSERT_EQUAL_HEX32(demux_test::crc32_novatel(data), demux::crc::crc32_novatel(data.subspan(129), c32_p1));
}

static void test_xor_and_ubx_ck()
{
    static constexpr std::string_view kNmea = "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
    static constexpr std::array<uint8_t, 7> kUbxBody{0x06U, 0x01U, 0x03U, 0x00U, 0xF0U, 0x00U, 0x01U};
    std::array<uint8_t, 64> nmea_bytes{};

    for (std::size_t i = 0U; i < kNmea.size(); ++i) {
        // ASCII 字符按无符号字节 0..255 解释
        nmea_bytes[i] = static_cast<uint8_t>(kNmea[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x47U, demux_test::xor_checksum(std::span<const uint8_t>(nmea_bytes.data(), kNmea.size())));

    const demux_test::UbxChecksum ck = demux_test::ubx_checksum(kUbxBody);
    TEST_ASSERT_EQUAL_HEX8(0xFBU, ck.ck_a);
    TEST_ASSERT_EQUAL_HEX8(0x10U, ck.ck_b);
}

static void test_generators_sanity_and_bounds()
{
    static std::array<uint8_t, 4100> s_buf{};
    static constexpr std::array<uint8_t, 3> kPayload{0x11U, 0x22U, 0x33U};
    static constexpr std::array<uint8_t, 15> kBadRest{};
    static constexpr std::array<uint8_t, 16> kGoodRest{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static std::array<uint8_t, demux_test::kUnicoreBinMaxPayload + 1U> s_too_big{};
    std::size_t len = 0U;

    const std::span<uint8_t> out(s_buf);

    // RTCM3
    TEST_ASSERT_TRUE(demux_test::gen_rtcm3(out, kPayload, len));
    TEST_ASSERT_EQUAL_UINT(9U, len);
    TEST_ASSERT_EQUAL_HEX8(0xD3U, s_buf[0]);
    TEST_ASSERT_FALSE(
        demux_test::gen_rtcm3(out, std::span<const uint8_t>(s_too_big.data(), demux_test::kRtcm3MaxPayload + 1U), len));
    TEST_ASSERT_FALSE(demux_test::gen_rtcm3(out.subspan(0, 8U), kPayload, len));

    // NMEA（含大小写校验）
    static constexpr std::array<uint8_t, 5> kExpNmeaTail{'*', '4', '7', '\r', '\n'};
    TEST_ASSERT_TRUE(
        demux_test::gen_nmea(out, "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", false, len));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpNmeaTail.data(), out.subspan(len - kExpNmeaTail.size()).data(),
                                 kExpNmeaTail.size());
    TEST_ASSERT_FALSE(demux_test::gen_nmea(out, "@BAD", false, len));

    // UBX
    TEST_ASSERT_TRUE(demux_test::gen_ubx(out, 0x01U, 0x07U, kPayload, len));
    TEST_ASSERT_EQUAL_UINT(11U, len);
    TEST_ASSERT_FALSE(demux_test::gen_ubx(
        out, 0x01U, 0x07U, std::span<const uint8_t>(s_too_big.data(), demux_test::kUbxMaxPayload + 1U), len));

    // Unicore 二进制
    TEST_ASSERT_TRUE(demux_test::gen_unicore_bin(out, 0x002AU, 55U, kGoodRest, kPayload, len));
    TEST_ASSERT_EQUAL_UINT(31U, len);
    TEST_ASSERT_FALSE(demux_test::gen_unicore_bin(out, 0x002AU, 55U, kBadRest, kPayload, len));
    TEST_ASSERT_FALSE(demux_test::gen_unicore_bin(out, 0x002AU, 55U, {}, s_too_big, len));

    // Unicore Hash CRC-32（对照真实 VERSION 向量）
    static constexpr std::array<uint8_t, 11> kExpHashTail{'*', '7', '4', '3', '2', 'd', '4', 'e', 'd', '\r', '\n'};
    TEST_ASSERT_TRUE(
        demux_test::gen_unicore_hash_crc32(out,
                                           "VERSION,97,GPS,FINE,2282,248561000,0,0,18,676;UM980,R4.10Build7923,"
                                           "HRPT00-S10C-P,2310415000001-MD22B1224962616,ff3bac96f31f9bdd,2022/09/28",
                                           true, len));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpHashTail.data(), out.subspan(len - kExpHashTail.size()).data(),
                                 kExpHashTail.size());

    // Unicore Hash XOR
    TEST_ASSERT_TRUE(demux_test::gen_unicore_hash_xor(out, "OBSVMA,1", false, len));
    TEST_ASSERT_EQUAL_UINT(14U, len);
    TEST_ASSERT_FALSE(demux_test::gen_unicore_hash_xor(out, "0BAD", false, len));

    // 产品帧
    TEST_ASSERT_TRUE(
        demux_test::gen_product(out, 0x0001U, rtk::proto::sender::kApp, rtk::proto::msg_type::kQuery, {}, len));
    TEST_ASSERT_EQUAL_UINT(24U, len);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_crc24q_known_and_segmented);
    RUN_TEST(test_crc32_known_and_segmented);
    RUN_TEST(test_crc_cross_check_random_bytes);
    RUN_TEST(test_xor_and_ubx_ck);
    RUN_TEST(test_generators_sanity_and_bounds);
    return UNITY_END();
}
