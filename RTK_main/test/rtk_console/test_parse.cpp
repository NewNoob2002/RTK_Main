// rtk::console 参数解析辅助 host 单元测试（parse_u32 / parse_u8 / parse_mac / parse_hex）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/console/console.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint32_t kU32Sentinel = 0x12345678U;
constexpr uint8_t kU8Sentinel = 0xA5U;
constexpr uint8_t kHexSentinel = 0x5AU;
constexpr size_t kWrittenSentinel = 77U;
constexpr uint32_t kVal42 = 42U;
constexpr uint32_t kVal7 = 7U;
constexpr uint32_t kValHex16 = 0x10U;
constexpr uint32_t kValHex255 = 0xFFU;
constexpr uint32_t kValDeadBeef = 0xDEADBEEFU;
constexpr uint8_t kVal127 = 127U;
constexpr size_t kSmallHexBuf = 2U;
constexpr size_t kHexOutCap = 4U;

void test_parse_u32_valid()
{
    uint32_t out = kU32Sentinel;

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0", out));
    TEST_ASSERT_EQUAL_UINT32(0U, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("1", out));
    TEST_ASSERT_EQUAL_UINT32(1U, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("42", out));
    TEST_ASSERT_EQUAL_UINT32(kVal42, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("007", out));
    TEST_ASSERT_EQUAL_UINT32(kVal7, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("4294967295", out));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0x0", out));
    TEST_ASSERT_EQUAL_UINT32(0U, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0X10", out));
    TEST_ASSERT_EQUAL_UINT32(kValHex16, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0xff", out));
    TEST_ASSERT_EQUAL_UINT32(kValHex255, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0xDeAdBeEf", out));
    TEST_ASSERT_EQUAL_UINT32(kValDeadBeef, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0x000000001", out));
    TEST_ASSERT_EQUAL_UINT32(1U, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0xffffffff", out));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u32("0XFFFFFFFF", out));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, out);
}

void test_parse_u32_invalid_preserves_out()
{
    const std::array<const char *, 19> bad_inputs{{
        nullptr, "",     "0x",  "0X",    " 1",  "1 ",    " 0x1",       "0x1 ",        "+1",          "-1",
        "+0x1",  "-0x1", "12a", "0x12g", "1.5", "0b101", "4294967296", "42949672950", "0x100000000",
    }};

    for (const char *input : bad_inputs) {
        uint32_t out = kU32Sentinel;
        TEST_ASSERT_FALSE(rtk::console::parse_u32(input, out));
        TEST_ASSERT_EQUAL_UINT32(kU32Sentinel, out);
    }
}

void test_parse_u8()
{
    uint8_t out = kU8Sentinel;

    TEST_ASSERT_TRUE(rtk::console::parse_u8("0", out));
    TEST_ASSERT_EQUAL_UINT8(0U, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u8("2", out));
    TEST_ASSERT_EQUAL_UINT8(2U, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u8("255", out));
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u8("0x0", out));
    TEST_ASSERT_EQUAL_UINT8(0U, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u8("0X7f", out));
    TEST_ASSERT_EQUAL_UINT8(kVal127, out);

    TEST_ASSERT_TRUE(rtk::console::parse_u8("0xff", out));
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, out);

    const std::array<const char *, 9> bad_inputs{{
        nullptr,
        "",
        "256",
        "0x100",
        "4294967295",
        "-1",
        "12x",
        " 1",
        "0x",
    }};
    for (const char *input : bad_inputs) {
        out = kU8Sentinel;
        TEST_ASSERT_FALSE(rtk::console::parse_u8(input, out));
        TEST_ASSERT_EQUAL_UINT8(kU8Sentinel, out);
    }
}

void test_parse_mac()
{
    std::array<uint8_t, rtk::console::kMacLen> mac{};

    TEST_ASSERT_TRUE(rtk::console::parse_mac("00:00:00:00:00:00", mac));
    const std::array<uint8_t, rtk::console::kMacLen> zeros{0U, 0U, 0U, 0U, 0U, 0U};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros.data(), mac.data(), rtk::console::kMacLen);

    TEST_ASSERT_TRUE(rtk::console::parse_mac("FF:FF:FF:FF:FF:FF", mac));
    const std::array<uint8_t, rtk::console::kMacLen> ones{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ones.data(), mac.data(), rtk::console::kMacLen);

    TEST_ASSERT_TRUE(rtk::console::parse_mac("a1:B2:c3:D4:e5:F6", mac));
    const std::array<uint8_t, rtk::console::kMacLen> mixed{0xA1U, 0xB2U, 0xC3U, 0xD4U, 0xE5U, 0xF6U};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mixed.data(), mac.data(), rtk::console::kMacLen);

    const std::array<uint8_t, rtk::console::kMacLen> sentinel{0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
    const std::array<const char *, 11> bad_macs{{
        nullptr,
        "",
        "AA:BB:CC:DD:EE",
        "AA:BB:CC:DD:EE:FF:00",
        "AA:BB:CC:DD:EE:F",
        "A:B:C:D:E:F",
        "AA-BB-CC-DD-EE-FF",
        "AA:BB:CC:DD:EE:FG",
        " AA:BB:CC:DD:EE:FF",
        "AA:BB:CC:DD:EE:FF ",
        "AA:BB:CC:DD:EE;FF",
    }};
    for (const char *input : bad_macs) {
        mac = sentinel;
        TEST_ASSERT_FALSE(rtk::console::parse_mac(input, mac));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(sentinel.data(), mac.data(), rtk::console::kMacLen);
    }
}

void test_parse_hex()
{
    std::array<uint8_t, kHexOutCap> buf{};
    buf.fill(kHexSentinel);
    size_t written = kWrittenSentinel;

    // 空串 -> 0 字节，不破坏缓冲
    TEST_ASSERT_TRUE(rtk::console::parse_hex("", buf, written));
    TEST_ASSERT_EQUAL_size_t(0U, written);
    TEST_ASSERT_EQUAL_UINT8(kHexSentinel, buf[0]);

    // 正常解码与大小写混合
    TEST_ASSERT_TRUE(rtk::console::parse_hex("01aBff", buf, written));
    TEST_ASSERT_EQUAL_size_t(3U, written);
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xABU, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xFFU, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(kHexSentinel, buf[3]);

    // 恰好填满缓冲
    TEST_ASSERT_TRUE(rtk::console::parse_hex("00112233", buf, written));
    TEST_ASSERT_EQUAL_size_t(kHexOutCap, written);
    const std::array<uint8_t, kHexOutCap> expect4{0x00U, 0x11U, 0x22U, 0x33U};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect4.data(), buf.data(), kHexOutCap);

    // 各类非法输入不得改动 out 或 written
    const std::array<uint8_t, kHexOutCap> sentinel_buf{kHexSentinel, kHexSentinel, kHexSentinel, kHexSentinel};
    const std::array<const char *, 9> bad_hex{{
        nullptr,
        "a",
        "123",
        "0102a",
        "zz",
        "01zz02",
        "01020g",
        "0x12",
        " 01",
    }};
    for (const char *input : bad_hex) {
        buf = sentinel_buf;
        written = kWrittenSentinel;
        TEST_ASSERT_FALSE(rtk::console::parse_hex(input, buf, written));
        TEST_ASSERT_EQUAL_size_t(kWrittenSentinel, written);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(sentinel_buf.data(), buf.data(), kHexOutCap);
    }

    // 超出 out 容量不得改动 out 或 written
    std::array<uint8_t, kSmallHexBuf> small_buf{kHexSentinel, kHexSentinel};
    written = kWrittenSentinel;
    TEST_ASSERT_FALSE(rtk::console::parse_hex("010203", small_buf, written));
    TEST_ASSERT_EQUAL_size_t(kWrittenSentinel, written);
    TEST_ASSERT_EQUAL_UINT8(kHexSentinel, small_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(kHexSentinel, small_buf[1]);

    written = kWrittenSentinel;
    TEST_ASSERT_FALSE(rtk::console::parse_hex("01", std::span<uint8_t>{}, written));
    TEST_ASSERT_EQUAL_size_t(kWrittenSentinel, written);
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_u32_valid);
    RUN_TEST(test_parse_u32_invalid_preserves_out);
    RUN_TEST(test_parse_u8);
    RUN_TEST(test_parse_mac);
    RUN_TEST(test_parse_hex);
    return UNITY_END();
}
