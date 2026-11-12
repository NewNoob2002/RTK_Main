// 验收测试：alarm_codec 与 AlarmSource（冻结输入）。布局来自 phone_protocol.md §5。
#include <array>
#include <cstdint>

#include "rtk/core/alarm_source.hpp"
#include "rtk/proto/alarm_codec.hpp"
#include "unity.h"

namespace ac = rtk::proto::alarm; // 不用 alarm 作别名：避免与 POSIX alarm() 冲突

static rtk::core::AlarmSource s_src;

extern "C" void setUp(void)
{
    s_src = rtk::core::AlarmSource{};
}
extern "C" void tearDown(void) {}

static void test_encode_status_layout()
{
    ac::Status st{};
    st.seq = 0x1234;
    st.state = ac::kStateAlert;
    st.level = 3;
    st.fix_quality = 0xFF;
    st.battery_pct = 87;
    st.flags = ac::kFlagPhoneOnline | ac::kFlagTestOverride;
    st.uptime_ms = 0xA1B2C3D4U;
    const std::array<uint8_t, 16> expect = {0x34, 0x12, 0x01, 0x03, 0xFF, 0x57, 0x03, 0x00,
                                            0xD4, 0xC3, 0xB2, 0xA1, 0x00, 0x00, 0x00, 0x00};
    std::array<uint8_t, 20> out{};
    out.fill(0xEE);
    TEST_ASSERT_EQUAL_UINT(16, ac::encode_status(st, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect.data(), out.data(), 16);
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[16]);
}

static void test_encode_status_short_buffer()
{
    std::array<uint8_t, 15> out{};
    out.fill(0xEE);
    TEST_ASSERT_EQUAL_UINT(0, ac::encode_status(ac::Status{}, out));
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[0]); // 不足时不写入
}

static void test_status_roundtrip()
{
    ac::Status st{};
    st.seq = 0xBEEF;
    st.state = ac::kStateNoFix;
    st.level = 1;
    st.battery_pct = 42;
    st.flags = ac::kFlagPhoneOnline;
    st.uptime_ms = 123456;
    std::array<uint8_t, 16> buf{};
    TEST_ASSERT_EQUAL_UINT(16, ac::encode_status(st, buf));
    auto back = ac::decode_status(buf);
    TEST_ASSERT_TRUE(back.has_value());
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, back->seq);
    TEST_ASSERT_EQUAL_HEX8(ac::kStateNoFix, back->state);
    TEST_ASSERT_EQUAL_UINT8(42, back->battery_pct);
    TEST_ASSERT_EQUAL_UINT32(123456, back->uptime_ms);
    TEST_ASSERT_FALSE(ac::decode_status(std::span<const uint8_t>(buf.data(), 15)).has_value());
}

static void test_decode_heartbeat()
{
    const std::array<uint8_t, 10> data = {0x05, 0x01, 0x50, 0x01, 0, 0, 0, 0, 9, 9};
    auto hb = ac::decode_heartbeat(data);
    TEST_ASSERT_TRUE(hb.has_value());
    TEST_ASSERT_EQUAL_HEX16(0x0105, hb->seq);
    TEST_ASSERT_EQUAL_UINT8(80, hb->battery_pct);
    TEST_ASSERT_EQUAL_UINT8(1, hb->local_state);
    TEST_ASSERT_FALSE(ac::decode_heartbeat(std::span<const uint8_t>(data.data(), 7)).has_value());
}

static void test_encode_heartbeat()
{
    ac::Heartbeat hb{};
    hb.seq = 0x0005;
    hb.battery_pct = 80;
    hb.local_state = 0;
    const std::array<uint8_t, 8> expect = {0x05, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::array<uint8_t, 8> out{};
    out.fill(0xEE);
    TEST_ASSERT_EQUAL_UINT(8, ac::encode_heartbeat(hb, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect.data(), out.data(), 8);
    TEST_ASSERT_EQUAL_UINT(0, ac::encode_heartbeat(hb, std::span<uint8_t>(out.data(), 7)));
}

static void test_src_defaults_unknown()
{
    TEST_ASSERT_FALSE(s_src.is_override());
    TEST_ASSERT_EQUAL_HEX8(ac::kStateUnknown, s_src.state());
    TEST_ASSERT_EQUAL_UINT8(0, s_src.level());
}

static void test_src_override_priority()
{
    s_src.set_fence(ac::kStateNormal);
    TEST_ASSERT_TRUE(s_src.set_override(ac::kStateAlert, 3));
    TEST_ASSERT_TRUE(s_src.is_override());
    TEST_ASSERT_EQUAL_HEX8(ac::kStateAlert, s_src.state());
    TEST_ASSERT_EQUAL_UINT8(3, s_src.level());
    s_src.clear_override();
    TEST_ASSERT_EQUAL_HEX8(ac::kStateNormal, s_src.state());
    TEST_ASSERT_EQUAL_UINT8(0, s_src.level());
}

static void test_src_fence_alert_level()
{
    s_src.set_fence(ac::kStateAlert);
    TEST_ASSERT_EQUAL_HEX8(ac::kStateAlert, s_src.state());
    TEST_ASSERT_EQUAL_UINT8(2, s_src.level());
}

static void test_src_override_rejects_invalid()
{
    TEST_ASSERT_FALSE(s_src.set_override(0x03, 1));
    TEST_ASSERT_FALSE(s_src.set_override(ac::kStateUnknown, 1));
    TEST_ASSERT_FALSE(s_src.set_override(ac::kStateAlert, ac::kLevelMax + 1));
    TEST_ASSERT_FALSE(s_src.is_override());
    TEST_ASSERT_TRUE(s_src.set_override(ac::kStateNoFix, 0));
    TEST_ASSERT_EQUAL_HEX8(ac::kStateNoFix, s_src.state());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_status_layout);
    RUN_TEST(test_encode_status_short_buffer);
    RUN_TEST(test_status_roundtrip);
    RUN_TEST(test_decode_heartbeat);
    RUN_TEST(test_encode_heartbeat);
    RUN_TEST(test_src_defaults_unknown);
    RUN_TEST(test_src_override_priority);
    RUN_TEST(test_src_fence_alert_level);
    RUN_TEST(test_src_override_rejects_invalid);
    return UNITY_END();
}
