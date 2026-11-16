// 验收测试：ByteWriter / ByteReader / fw::Status（冻结输入）。期望值来自 Bytes.hpp 契约。
#include <array>
#include <cstdint>
#include <cstring>

#include "proto/Bytes.hpp"
#include "fw/Status.hpp"
#include "unity.h"

using proto::ByteReader;
using proto::ByteWriter;

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

static void test_writer_little_endian()
{
    std::array<uint8_t, 8> buf{};
    ByteWriter w(buf);
    w.u8(0x11);
    w.u16(0x2233);
    w.u32(0x44556677);
    const std::array<uint8_t, 7> expect = {0x11, 0x33, 0x22, 0x77, 0x66, 0x55, 0x44};
    TEST_ASSERT_TRUE(w.ok());
    TEST_ASSERT_EQUAL_UINT(7, w.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect.data(), buf.data(), 7);
    TEST_ASSERT_EQUAL_UINT(7, w.written().size());
}

static void test_writer_sticky_overflow()
{
    std::array<uint8_t, 4> buf = {0xEE, 0xEE, 0xEE, 0xEE};
    ByteWriter w(std::span<uint8_t>(buf.data(), 3));
    w.u16(0xBBAA);
    w.u16(0xDDCC); // 只剩 1 字节：整体失败，不得部分写入
    TEST_ASSERT_FALSE(w.ok());
    TEST_ASSERT_EQUAL_UINT(2, w.size());
    w.u8(0x01); // 粘滞：之后的写入被忽略
    TEST_ASSERT_EQUAL_UINT(2, w.size());
    TEST_ASSERT_EQUAL_HEX8(0xAA, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, buf[3]); // span 之外绝不触碰
}

static void test_writer_fixed_str_pads_and_truncates()
{
    std::array<uint8_t, 16> buf{};
    buf.fill(0x55);
    ByteWriter w(buf);
    w.fixed_str("V1.1.0", 8);     // 短：补 0
    w.fixed_str("ABCDEFGHIJ", 4); // 长：截断
    TEST_ASSERT_TRUE(w.ok());
    TEST_ASSERT_EQUAL_UINT(12, w.size());
    TEST_ASSERT_EQUAL_MEMORY("V1.1.0\0\0", buf.data(), 8);
    TEST_ASSERT_EQUAL_MEMORY("ABCD", &buf[8], 4);
    TEST_ASSERT_EQUAL_HEX8(0x55, buf[12]);
}

static void test_writer_bytes_and_zeros()
{
    std::array<uint8_t, 6> buf{};
    buf.fill(0x77);
    const std::array<uint8_t, 2> src = {0x01, 0x02};
    ByteWriter w(buf);
    w.bytes(src);
    w.zeros(3);
    TEST_ASSERT_TRUE(w.ok());
    const std::array<uint8_t, 6> expect = {0x01, 0x02, 0x00, 0x00, 0x00, 0x77};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect.data(), buf.data(), 6);
    w.zeros(2); // 越界
    TEST_ASSERT_FALSE(w.ok());
    TEST_ASSERT_EQUAL_HEX8(0x77, buf[5]);
}

static void test_reader_values_and_overflow()
{
    const std::array<uint8_t, 7> data = {0x11, 0x33, 0x22, 0x77, 0x66, 0x55, 0x44};
    ByteReader r(data);
    TEST_ASSERT_EQUAL_HEX8(0x11, r.u8());
    TEST_ASSERT_EQUAL_HEX16(0x2233, r.u16());
    TEST_ASSERT_EQUAL_HEX32(0x44556677, r.u32());
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(0, r.remaining());
    TEST_ASSERT_EQUAL_HEX8(0, r.u8());
    TEST_ASSERT_FALSE(r.ok());
}

static void test_reader_take_and_skip()
{
    const std::array<uint8_t, 5> data = {1, 2, 3, 4, 5};
    ByteReader r(data);
    r.skip(1);
    auto view = r.take(3);
    TEST_ASSERT_EQUAL_UINT(3, view.size());
    TEST_ASSERT_EQUAL_HEX8(2, view[0]);
    TEST_ASSERT_EQUAL_UINT(1, r.remaining());
    TEST_ASSERT_EQUAL_UINT(0, r.take(2).size());
    TEST_ASSERT_FALSE(r.ok());
}

static void test_status_strings()
{
    TEST_ASSERT_TRUE(is_ok(fw::Status::kOk));
    TEST_ASSERT_FALSE(is_ok(fw::Status::kTimeout));
    TEST_ASSERT_NOT_NULL(to_string(fw::Status::kCorrupt));
    TEST_ASSERT_TRUE(std::strlen(to_string(fw::Status::kOk)) > 0U);
    TEST_ASSERT_EQUAL_STRING("?", to_string(static_cast<fw::Status>(0xEE)));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_writer_little_endian);
    RUN_TEST(test_writer_sticky_overflow);
    RUN_TEST(test_writer_fixed_str_pads_and_truncates);
    RUN_TEST(test_writer_bytes_and_zeros);
    RUN_TEST(test_reader_values_and_overflow);
    RUN_TEST(test_reader_take_and_skip);
    RUN_TEST(test_status_strings);
    return UNITY_END();
}
