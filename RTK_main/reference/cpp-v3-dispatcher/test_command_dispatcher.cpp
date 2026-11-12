// 验收测试：CommandDispatcher（冻结输入）。期望值来自 phone_protocol.md §3–§5 与
// command_dispatcher.hpp 契约。
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

#include "rtk/core/command_dispatcher.hpp"
#include "test_support.hpp"
#include "unity.h"

namespace ac = rtk::proto::alarm;
namespace mt = rtk::proto::msg_type;
using rtk::core::Actions;
using rtk::core::AlarmSource;
using rtk::core::BtAddr;
using rtk::core::CommandDispatcher;
using rtk::core::DispatchResult;
using rtk::core::LinkManager;
using rtk::core::Reply;
using rtk::core::Role;

static constexpr rtk::core::SessionHandle kPhone = 0x81;
static constexpr rtk::core::SessionHandle kAlarm = 0x82;
static constexpr BtAddr kPhoneAddr{{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
static constexpr BtAddr kAlarmAddr{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};

static AlarmSource s_alarm;
static LinkManager s_link;
static Reply s_reply;
static std::array<uint8_t, rtk::proto::kMaxPayload> s_payload{};
// 每个用例重建 dispatcher（它持有引用、不可赋值）
alignas(CommandDispatcher) static std::array<uint8_t, sizeof(CommandDispatcher)> s_storage{};
static CommandDispatcher *s_disp = nullptr;

extern "C" void setUp(void)
{
    s_alarm = AlarmSource{};
    s_link = LinkManager{};
    s_link.init(rtk::core::Policy::kConcurrent, {});
    s_payload.fill(0);
    s_reply.buf.fill(0x5A);
    s_reply.len = 99;
    s_reply.whitelist_changed = true;
    s_disp = new (s_storage.data()) CommandDispatcher(s_alarm, s_link); // placement new，无堆
}
extern "C" void tearDown(void)
{
    s_disp->~CommandDispatcher();
    s_disp = nullptr;
}

static DispatchResult send(Role role, uint16_t msg_id, uint8_t sender, uint8_t type, size_t len, uint32_t now_ms = 0)
{
    rtk::proto::FrameView f{};
    f.msg_id = msg_id;
    f.sender = sender;
    f.type = type;
    f.payload = std::span<const uint8_t>(s_payload.data(), len);
    return s_disp->handle(role, f, now_ms, s_reply);
}

// 解析应答帧并检查头部；返回 payload 拷贝。
static test::FeedResult check_reply(uint16_t msg_id, uint8_t type, size_t len)
{
    TEST_ASSERT_TRUE(s_reply.len > 0U);
    rtk::proto::FrameParser parser;
    auto r = test::feed_all(parser, s_reply.bytes());
    TEST_ASSERT_EQUAL_INT(1, r.frames);
    TEST_ASSERT_EQUAL_UINT(rtk::proto::kHeaderLen + len + rtk::proto::kCrcLen, s_reply.len);
    TEST_ASSERT_EQUAL_HEX16(msg_id, r.last.msg_id);
    TEST_ASSERT_EQUAL_HEX8(rtk::proto::sender::kDevice, r.last.sender);
    TEST_ASSERT_EQUAL_HEX8(type, r.last.type);
    TEST_ASSERT_EQUAL_UINT(len, r.last.payload.size());
    return r;
}

static void test_fixed_str()
{
    rtk::core::FixedStr<8> s;
    s.assign("V1.1.0");
    TEST_ASSERT_EQUAL_UINT(6, s.view().size());
    s.assign("ABCDEFGHIJK");
    TEST_ASSERT_EQUAL_UINT(8, s.view().size());
    TEST_ASSERT_EQUAL_UINT(8, rtk::core::FixedStr<8>::capacity());
}

static void test_dev_info_0x01()
{
    s_disp->info().sn.assign("SN0001");
    s_disp->info().fw_ver.assign("V6.1.0");
    TEST_ASSERT_TRUE(DispatchResult::kReply == send(Role::kPhone, 0x0001, 0, mt::kQuery, 0));
    auto r = check_reply(0x0001, mt::kQueryRes, rtk::core::kDevInfoLen);
    const uint8_t *p = r.payload.data();
    TEST_ASSERT_EQUAL_MEMORY("E1-Lite\0", &p[0], 8);
    TEST_ASSERT_EQUAL_MEMORY("SN0001\0\0", &p[8], 8);
    TEST_ASSERT_EQUAL_MEMORY("V1.1.0\0\0", &p[24], 8); // 整个字段宽度，补 0
    TEST_ASSERT_EQUAL_MEMORY("V6.1.0", &p[32], 6);
    TEST_ASSERT_EQUAL_MEMORY("UM980\0\0\0", &p[56], 8);
    TEST_ASSERT_EQUAL_MEMORY("5002D\0\0\0", &p[96], 8);
}

static void test_dev_state_0x02_constant()
{
    std::array<uint8_t, rtk::core::kDevStateLen> expect{};
    expect[0] = 0x03;
    expect[1] = 0x01;
    expect[2] = 0x81;
    expect[3] = 0x83;
    expect[4] = 0x81;
    expect[9] = 0x81;
    expect[10] = 0x81;
    expect[11] = 0x81;
    expect[13] = 0x01;
    TEST_ASSERT_TRUE(DispatchResult::kReply == send(Role::kPhone, 0x0002, 0, mt::kQuery, 0));
    auto r = check_reply(0x0002, mt::kQueryRes, rtk::core::kDevStateLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect.data(), r.payload.data(), expect.size());
}

static void test_battery_0x03()
{
    s_disp->info().battery_pct = 64;
    s_disp->info().battery_temp_c = -5;
    TEST_ASSERT_TRUE(DispatchResult::kReply == send(Role::kPhone, 0x0003, 0, mt::kQuery, 0));
    auto r = check_reply(0x0003, mt::kQueryRes, rtk::core::kBatteryLen);
    TEST_ASSERT_EQUAL_HEX8(0x01, r.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(64, r.payload[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFB, r.payload[12]);
}

static void test_placeholders_0x04_0x05_0x06()
{
    const std::array<uint8_t, 2> ok = {0x01, 0x01};
    const std::array<uint8_t, rtk::core::kSatTrackLen> zeros{};
    static_cast<void>(send(Role::kPhone, 0x0004, 0, mt::kQuery, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok.data(), check_reply(0x0004, mt::kQueryRes, 2).payload.data(), 2);
    static_cast<void>(send(Role::kPhone, 0x0005, 0, mt::kQuery, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok.data(), check_reply(0x0005, mt::kQueryRes, 2).payload.data(), 2);
    static_cast<void>(send(Role::kPhone, 0x0006, 0, mt::kQuery, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zeros.data(), check_reply(0x0006, mt::kQueryRes, zeros.size()).payload.data(),
                                 zeros.size());
    static_cast<void>(send(Role::kPhone, 0x0006, 0, mt::kSet, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok.data(), check_reply(0x0006, mt::kSetRes, 2).payload.data(), 2);
}

static void test_work_mode_0x07_roundtrip_and_short()
{
    for (size_t i = 0; i < rtk::core::kWorkModeSetLen; ++i) {
        s_payload[i] = static_cast<uint8_t>(i + 1U); // < 256
    }
    TEST_ASSERT_TRUE(DispatchResult::kReply == send(Role::kPhone, 0x0007, 0, mt::kSet, rtk::core::kWorkModeSetLen));
    TEST_ASSERT_EQUAL_HEX8(0x01, check_reply(0x0007, mt::kSetRes, 2).payload[0]);
    static_cast<void>(send(Role::kPhone, 0x0007, 0, mt::kQuery, 0));
    auto r = check_reply(0x0007, mt::kQueryRes, rtk::core::kWorkModeLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_payload.data(), r.payload.data(), rtk::core::kWorkModeSetLen);
    static_cast<void>(send(Role::kPhone, 0x0007, 0, mt::kSet, rtk::core::kWorkModeSetLen - 1U));
    auto fail = check_reply(0x0007, mt::kSetRes, 2);
    TEST_ASSERT_EQUAL_HEX8(0x02, fail.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, fail.payload[1]);
}

static void test_com_0x0d()
{
    TEST_ASSERT_TRUE(DispatchResult::kReply == send(Role::kPhone, 0x000D, 0, mt::kQuery, 0));
    auto r = check_reply(0x000D, mt::kQueryRes, rtk::core::kComLen);
    const uint8_t *p = r.payload.data();
    TEST_ASSERT_EQUAL_HEX8(0x01, p[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, p[4]); // 115200 = 0x0001C200
    TEST_ASSERT_EQUAL_HEX8(0xC2, p[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p[6]);
    TEST_ASSERT_EQUAL_HEX8(0x00, p[7]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p[8]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p[27]);
    TEST_ASSERT_EQUAL_HEX8(0xDA, p[28]); // 218
    TEST_ASSERT_EQUAL_HEX8(0x00, p[29]);
    TEST_ASSERT_TRUE(DispatchResult::kNoReply == send(Role::kPhone, 0x000D, 0, mt::kSet, rtk::core::kComLen));
    TEST_ASSERT_EQUAL_UINT(0, s_reply.len); // 5.4 行为：不回复
}

static void test_radio_0x14()
{
    static_cast<void>(send(Role::kPhone, 0x0014, 0, mt::kQuery, 0));
    TEST_ASSERT_EQUAL_HEX8(0x05, check_reply(0x0014, mt::kQueryRes, rtk::core::kRadioLen).payload[17]);
    s_payload[0] = 0x02;
    s_payload[14] = 0x04;
    static_cast<void>(send(Role::kPhone, 0x0014, 0, mt::kSet, rtk::core::kRadioSetMinLen));
    static_cast<void>(check_reply(0x0014, mt::kSetRes, 2));
    static_cast<void>(send(Role::kPhone, 0x0014, 0, mt::kQuery, 0));
    auto r = check_reply(0x0014, mt::kQueryRes, rtk::core::kRadioLen);
    TEST_ASSERT_EQUAL_HEX8(0x02, r.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, r.payload[14]);
    TEST_ASSERT_EQUAL_HEX8(0x05, r.payload[17]); // 未提供则保持
    static_cast<void>(send(Role::kPhone, 0x0014, 0, mt::kSet, rtk::core::kRadioSetMinLen - 1U));
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(0x0014, mt::kSetRes, 2).payload[0]);
}

static void test_log_0x15()
{
    s_payload[38] = 0x77;
    static_cast<void>(send(Role::kPhone, 0x0015, 0, mt::kSet, rtk::core::kLogLen));
    static_cast<void>(check_reply(0x0015, mt::kSetRes, 2));
    static_cast<void>(send(Role::kPhone, 0x0015, 0, mt::kQuery, 0));
    TEST_ASSERT_EQUAL_HEX8(0x77, check_reply(0x0015, mt::kQueryRes, rtk::core::kLogLen).payload[38]);
    static_cast<void>(send(Role::kPhone, 0x0015, 0, mt::kSet, rtk::core::kLogLen - 1U));
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(0x0015, mt::kSetRes, 2).payload[0]);
}

static void test_reserved_and_unknown_no_reply()
{
    const std::array<uint16_t, 6> ids = {0x001E, 0x0021, 0x0025, 0x0028, 0x0034, 0x0099};
    for (uint16_t id : ids) {
        TEST_ASSERT_TRUE(DispatchResult::kNoReply == send(Role::kPhone, id, 0, mt::kQuery, 0));
        TEST_ASSERT_EQUAL_UINT(0, s_reply.len);
    }
    // 未知 ID 先于角色检查
    TEST_ASSERT_TRUE(DispatchResult::kNoReply == send(Role::kAlarm, 0x0099, 3, mt::kQuery, 0));
}

static void test_role_permissions()
{
    TEST_ASSERT_TRUE(DispatchResult::kDenied == send(Role::kAlarm, 0x0001, 3, mt::kQuery, 0));
    TEST_ASSERT_TRUE(DispatchResult::kDenied == send(Role::kAlarm, ac::kMsgTest, 3, mt::kSet, 3));
    TEST_ASSERT_TRUE(DispatchResult::kDenied == send(Role::kPending, 0x0001, 0, mt::kQuery, 0));
    TEST_ASSERT_TRUE(DispatchResult::kDenied == send(Role::kNone, 0x0001, 0, mt::kQuery, 0));
    TEST_ASSERT_TRUE(DispatchResult::kDenied == send(Role::kPhone, ac::kMsgHeartbeat, 0, mt::kPush, 8));
    TEST_ASSERT_EQUAL_UINT(0, s_reply.len);
}

static void test_heartbeat_0x41_from_alarm()
{
    s_payload[0] = 0x07;
    s_payload[2] = 55;
    s_payload[3] = 2;
    TEST_ASSERT_TRUE(DispatchResult::kNoReply ==
                     send(Role::kAlarm, ac::kMsgHeartbeat, 3, mt::kPush, ac::kHeartbeatLen));
    TEST_ASSERT_TRUE(s_reply.heartbeat.has_value());
    TEST_ASSERT_EQUAL_HEX16(0x0007, s_reply.heartbeat->seq);
    TEST_ASSERT_EQUAL_UINT8(55, s_reply.heartbeat->battery_pct);
    TEST_ASSERT_EQUAL_UINT8(2, s_reply.heartbeat->local_state);
    TEST_ASSERT_EQUAL_UINT(0, s_reply.len);
    TEST_ASSERT_FALSE(s_reply.whitelist_changed); // reply 先 clear()
}

static void test_test_override_0x42_and_status_query()
{
    Actions act;
    static_cast<void>(s_link.on_open(kPhone, kPhoneAddr, 0, act));
    s_payload[0] = 1;
    s_payload[1] = ac::kStateAlert;
    s_payload[2] = 3;
    TEST_ASSERT_TRUE(DispatchResult::kReply == send(Role::kPhone, ac::kMsgTest, 0, mt::kSet, 3));
    static_cast<void>(check_reply(ac::kMsgTest, mt::kSetRes, 2));
    TEST_ASSERT_EQUAL_HEX8(ac::kStateAlert, s_alarm.state());

    TEST_ASSERT_TRUE(DispatchResult::kReply == send(Role::kAlarm, ac::kMsgStatus, 3, mt::kQuery, 0, 0x01020304U));
    auto r = check_reply(ac::kMsgStatus, mt::kQueryRes, ac::kStatusLen);
    const uint8_t *p = r.payload.data();
    TEST_ASSERT_EQUAL_HEX8(0x00, p[0]);
    TEST_ASSERT_EQUAL_HEX8(ac::kStateAlert, p[2]);
    TEST_ASSERT_EQUAL_HEX8(3, p[3]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, p[4]);
    TEST_ASSERT_EQUAL_HEX8(ac::kFlagPhoneOnline | ac::kFlagTestOverride, p[6]);
    TEST_ASSERT_EQUAL_HEX8(0x04, p[8]);
    TEST_ASSERT_EQUAL_HEX8(0x01, p[11]);

    s_payload[1] = 0x09; // 非法状态
    static_cast<void>(send(Role::kPhone, ac::kMsgTest, 0, mt::kSet, 3));
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(ac::kMsgTest, mt::kSetRes, 2).payload[0]);
    s_payload[0] = 0; // 清除
    static_cast<void>(send(Role::kPhone, ac::kMsgTest, 0, mt::kSet, 3));
    TEST_ASSERT_FALSE(s_alarm.is_override());
}

static void test_fence_0x43_reserved()
{
    static_cast<void>(send(Role::kPhone, ac::kMsgFence, 0, mt::kSet, 8));
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(ac::kMsgFence, mt::kSetRes, 2).payload[0]);
    static_cast<void>(send(Role::kPhone, ac::kMsgFence, 0, mt::kQuery, 0));
    TEST_ASSERT_EQUAL_HEX8(0x02, check_reply(ac::kMsgFence, mt::kQueryRes, 2).payload[0]);
}

static void test_pairing_0x44()
{
    Actions act;
    s_payload[0] = 1;
    static_cast<void>(send(Role::kPhone, ac::kMsgPair, 0, mt::kSet, 1, 1000));
    static_cast<void>(check_reply(ac::kMsgPair, mt::kSetRes, 2));
    TEST_ASSERT_TRUE(s_link.is_pairing());

    static_cast<void>(s_link.on_open(kAlarm, kAlarmAddr, 1100, act));
    static_cast<void>(s_link.on_frame(kAlarm, 3, ac::kMsgHeartbeat, 1200, act));
    static_cast<void>(send(Role::kPhone, ac::kMsgPair, 0, mt::kQuery, 0, 1300));
    auto r = check_reply(ac::kMsgPair, mt::kQueryRes, 7);
    TEST_ASSERT_EQUAL_UINT8(1, r.payload[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kAlarmAddr.bytes.data(), &r.payload[1], 6);

    s_payload[0] = 0;
    static_cast<void>(send(Role::kPhone, ac::kMsgPair, 0, mt::kSet, 1, 1400));
    TEST_ASSERT_TRUE(s_reply.whitelist_changed);
    static_cast<void>(send(Role::kPhone, ac::kMsgPair, 0, mt::kQuery, 0, 1500));
    TEST_ASSERT_EQUAL_UINT8(0, check_reply(ac::kMsgPair, mt::kQueryRes, 1).payload[0]);
}

static void test_status_push()
{
    TEST_ASSERT_TRUE(s_alarm.set_override(ac::kStateNormal, 0));
    s_disp->info().battery_pct = 90;
    TEST_ASSERT_TRUE(s_disp->build_status_push(0xBEEF, 5000, s_reply));
    rtk::proto::FrameParser parser;
    auto r = test::feed_all(parser, s_reply.bytes());
    TEST_ASSERT_EQUAL_INT(1, r.frames);
    TEST_ASSERT_EQUAL_HEX16(ac::kMsgStatus, r.last.msg_id);
    TEST_ASSERT_EQUAL_HEX8(mt::kPush, r.last.type);
    TEST_ASSERT_EQUAL_HEX8(rtk::proto::sender::kDevice, r.last.sender);
    TEST_ASSERT_EQUAL_HEX8(0xEF, r.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, r.payload[1]);
    TEST_ASSERT_EQUAL_HEX8(ac::kStateNormal, r.payload[2]);
    TEST_ASSERT_EQUAL_UINT8(90, r.payload[5]);
    TEST_ASSERT_EQUAL_HEX8(ac::kFlagTestOverride, r.payload[6]);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_fixed_str);
    RUN_TEST(test_dev_info_0x01);
    RUN_TEST(test_dev_state_0x02_constant);
    RUN_TEST(test_battery_0x03);
    RUN_TEST(test_placeholders_0x04_0x05_0x06);
    RUN_TEST(test_work_mode_0x07_roundtrip_and_short);
    RUN_TEST(test_com_0x0d);
    RUN_TEST(test_radio_0x14);
    RUN_TEST(test_log_0x15);
    RUN_TEST(test_reserved_and_unknown_no_reply);
    RUN_TEST(test_role_permissions);
    RUN_TEST(test_heartbeat_0x41_from_alarm);
    RUN_TEST(test_test_override_0x42_and_status_query);
    RUN_TEST(test_fence_0x43_reserved);
    RUN_TEST(test_pairing_0x44);
    RUN_TEST(test_status_push);
    return UNITY_END();
}
