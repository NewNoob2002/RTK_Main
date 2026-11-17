// 验收测试：frame（冻结输入）。期望值来自 phone_protocol.md 与 Python zlib.crc32 独立计算。
#include <array>
#include <cstdint>
#include <cstring>

#include "proto/Frame.hpp"
#include "TestSupport.hpp"
#include "unity.h"

// App 查询 0x0001，无 payload
static constexpr std::array<uint8_t, 24> kFrameA = {0xAA, 0x44, 0x18, 0x14, 0x01, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0x69, 0x17, 0x6E, 0x7E};
// 设备设置应答 0x0004，payload 01 01
static constexpr std::array<uint8_t, 26> kFrameB = {
    0xAA, 0x44, 0x18, 0x14, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x01, 0x01, 0xC0, 0x20, 0xDA, 0x74};
// 报警器心跳 0x0041，sender 3，type PUSH，payload 05 00 50 00 00 00 00 00
static constexpr std::array<uint8_t, 32> kFrameC = {
    0xAA, 0x44, 0x18, 0x14, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x03, 0x04, 0x00, 0x00, 0x05, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x34, 0x05, 0xAA};

static proto::FrameParser s_parser;

extern "C" void setUp(void)
{
    s_parser = proto::FrameParser{};
}
extern "C" void tearDown(void) {}

static void test_crc32_check_value()
{
    const std::array<uint8_t, 9> msg = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, proto::crc32(msg));
    const uint32_t part = proto::crc32(std::span<const uint8_t>(msg.data(), 4));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U,
                            proto::crc32(std::span<const uint8_t>(msg.data() + 4, 5), part));
    TEST_ASSERT_EQUAL_HEX32(0x12345678U, proto::crc32({}, 0x12345678U));
}

static void test_build_query_frame()
{
    std::array<uint8_t, 64> out{};
    size_t len = 0;
    TEST_ASSERT_TRUE(is_ok(
        proto::build_frame(out, 0x0001, proto::sender::kApp, proto::msg_type::kQuery, {}, len)));
    TEST_ASSERT_EQUAL_UINT(kFrameA.size(), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kFrameA.data(), out.data(), kFrameA.size());
}

static void test_build_set_response()
{
    const std::array<uint8_t, 2> payload = {0x01, 0x01};
    std::array<uint8_t, 64> out{};
    size_t len = 0;
    TEST_ASSERT_TRUE(is_ok(proto::build_frame(out, 0x0004, proto::sender::kDevice,
                                              proto::msg_type::kSetRes, payload, len)));
    TEST_ASSERT_EQUAL_UINT(kFrameB.size(), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kFrameB.data(), out.data(), kFrameB.size());
}

static void test_build_rejects_bad_args()
{
    static std::array<uint8_t, proto::kMaxPayload + 1> big{};
    static std::array<uint8_t, proto::kMaxFrame + 8> out{};
    size_t len = 0xABCD;
    TEST_ASSERT_EQUAL(fw::Status::kNoSpace,
                      proto::build_frame(std::span<uint8_t>(out.data(), kFrameB.size() - 1), 0x0004,
                                         1, 3, std::span<const uint8_t>(big.data(), 2), len));
    TEST_ASSERT_EQUAL(fw::Status::kInvalidArg, proto::build_frame(out, 0x0004, 1, 3, big, len));
    TEST_ASSERT_EQUAL_UINT(0xABCD, len);
    TEST_ASSERT_EQUAL(
        fw::Status::kOk,
        proto::build_frame(std::span<uint8_t>(out.data(), proto::kMaxFrame), 0x0004, 1, 3,
                           std::span<const uint8_t>(big.data(), proto::kMaxPayload), len));
    TEST_ASSERT_EQUAL_UINT(proto::kMaxFrame, len);
}

static void test_response_type_mapping()
{
    TEST_ASSERT_EQUAL_HEX8(0x01, proto::response_type(0x00));
    TEST_ASSERT_EQUAL_HEX8(0x03, proto::response_type(0x02));
    TEST_ASSERT_EQUAL_HEX8(0x05, proto::response_type(0x04));
    TEST_ASSERT_EQUAL_HEX8(0x05, proto::response_type(0x01));
}

static void test_parse_single_frame()
{
    auto r = test::feed_all(s_parser, kFrameC);
    TEST_ASSERT_EQUAL_INT(1, r.frames);
    TEST_ASSERT_EQUAL_HEX16(0x0041, r.last.msg_id);
    TEST_ASSERT_EQUAL_HEX8(0x03, r.last.sender);
    TEST_ASSERT_EQUAL_HEX8(0x04, r.last.type);
    TEST_ASSERT_EQUAL_UINT(8, r.last.payload.size());
    TEST_ASSERT_EQUAL_HEX8(0x05, r.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0x50, r.payload[2]);
    TEST_ASSERT_EQUAL_INT(0, r.crc_errors);
    TEST_ASSERT_EQUAL_INT(0, r.length_errors);
}

static void test_parse_garbage_and_back_to_back()
{
    std::array<uint8_t, 128> stream{};
    size_t len = 0;
    for (uint8_t b : {uint8_t{0x00}, uint8_t{0xAA}, uint8_t{0x13}, uint8_t{0xAA}}) {
        stream[len++] = b;
    }
    std::memcpy(&stream[len], kFrameA.data(), kFrameA.size());
    len += kFrameA.size();
    std::memcpy(&stream[len], kFrameB.data(), kFrameB.size());
    len += kFrameB.size();
    auto r = test::feed_all(s_parser, std::span<const uint8_t>(stream.data(), len));
    TEST_ASSERT_EQUAL_INT(2, r.frames);
    TEST_ASSERT_EQUAL_HEX16(0x0004, r.last.msg_id);
    TEST_ASSERT_EQUAL_INT(0, r.crc_errors);
}

static void test_parse_aa_aa_resync()
{
    std::array<uint8_t, kFrameA.size() + 1> stream{};
    stream[0] = 0xAA;
    std::memcpy(&stream[1], kFrameA.data(), kFrameA.size());
    TEST_ASSERT_EQUAL_INT(1, test::feed_all(s_parser, stream).frames);
}

static void test_parse_split_across_feeds()
{
    const std::span<const uint8_t> all(kFrameC);
    TEST_ASSERT_EQUAL_INT(0, test::feed_all(s_parser, all.subspan(0, 3)).frames);
    TEST_ASSERT_EQUAL_INT(0, test::feed_all(s_parser, all.subspan(3, 20)).frames);
    TEST_ASSERT_EQUAL_INT(1, test::feed_all(s_parser, all.subspan(23)).frames);
}

static void test_parse_crc_error_then_recover()
{
    auto bad = kFrameB;
    bad[20] ^= 0xFFU;
    auto r = test::feed_all(s_parser, bad);
    TEST_ASSERT_EQUAL_INT(0, r.frames);
    TEST_ASSERT_EQUAL_INT(1, r.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(1, s_parser.crc_errors());
    TEST_ASSERT_EQUAL_INT(1, test::feed_all(s_parser, kFrameB).frames);
}

static void test_parse_length_errors()
{
    auto hdr = kFrameA;
    hdr[3] = 0x15;
    TEST_ASSERT_EQUAL_INT(
        1, test::feed_all(s_parser, std::span<const uint8_t>(hdr.data(), 4)).length_errors);

    s_parser = proto::FrameParser{};
    hdr = kFrameA;
    hdr[12] = 0x01; // payload_len = 257
    hdr[13] = 0x01;
    TEST_ASSERT_EQUAL_INT(
        1, test::feed_all(s_parser, std::span<const uint8_t>(hdr.data(), 20)).length_errors);
    TEST_ASSERT_EQUAL_UINT32(1, s_parser.length_errors());
    TEST_ASSERT_EQUAL_INT(1, test::feed_all(s_parser, kFrameA).frames);
}

static void test_parse_max_payload()
{
    static std::array<uint8_t, proto::kMaxPayload> payload{};
    static std::array<uint8_t, proto::kMaxFrame> out{};
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i); // i < 256
    }
    size_t len = 0;
    TEST_ASSERT_TRUE(is_ok(proto::build_frame(out, 0x0007, 0, 2, payload, len)));
    auto r = test::feed_all(s_parser, std::span<const uint8_t>(out.data(), len));
    TEST_ASSERT_EQUAL_INT(1, r.frames);
    TEST_ASSERT_EQUAL_UINT(proto::kMaxPayload, r.last.payload.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload.data(), r.payload.data(), proto::kMaxPayload);
}

static void test_reset_drops_partial_frame()
{
    TEST_ASSERT_EQUAL_INT(
        0, test::feed_all(s_parser, std::span<const uint8_t>(kFrameA.data(), 10)).frames);
    s_parser.reset();
    TEST_ASSERT_EQUAL_INT(
        0, test::feed_all(s_parser, std::span<const uint8_t>(kFrameA.data() + 10, 14)).frames);
    TEST_ASSERT_EQUAL_INT(1, test::feed_all(s_parser, kFrameA).frames);
}

int main()
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
    RUN_TEST(test_reset_drops_partial_frame);
    return UNITY_END();
}
