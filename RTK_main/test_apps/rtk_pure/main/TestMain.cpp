// T15：C++20 纯逻辑冒烟测试；各用例独占栈上状态，无硬件或堆依赖。
#include <cstdint>
#include <array>
#include <span>

#include "core/LinkManager.hpp"
#include "proto/Bytes.hpp"
#include "unity.h"

namespace {

using core::Actions;
using core::BtAddr;
using core::LinkManager;
using core::Policy;
using core::Role;
using core::SessionHandle;
using proto::ByteWriter;

// 地址和句柄沿用冻结的 test/host/TestLinkManager.cpp，仅作逻辑身份，不连接蓝牙。
constexpr SessionHandle kPhone = 0x81;
constexpr SessionHandle kAlarm = 0x82;
constexpr BtAddr kPhoneAddr{{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
constexpr BtAddr kAlarmAddr{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};

// 来源：Bytes.hpp 粘滞错误契约与 host 的 test_writer_sticky_overflow。
TEST_CASE("C++ ByteWriter scalar overflow stays sticky", "[rtk_pure][bytes]")
{
    std::array<uint8_t, 4> buffer = {0xEE, 0xEE, 0xEE, 0xEE};
    ByteWriter writer(std::span<uint8_t>(buffer.data(), 3));
    writer.u16(0xBBAA);
    writer.u16(0xDDCC); // 剩余一字节，失败必须整体生效，且不能修改 span 外哨兵。
    TEST_ASSERT_FALSE(writer.ok());
    TEST_ASSERT_EQUAL_UINT(2, writer.size());
    writer.u8(0x01);
    writer.u32(0x44556677);
    TEST_ASSERT_FALSE(writer.ok());
    TEST_ASSERT_EQUAL_UINT(2, writer.size());
    TEST_ASSERT_EQUAL_UINT(2, writer.written().size());
    const std::array<uint8_t, 4> expected = {0xAA, 0xBB, 0xEE, 0xEE};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected.data(), buffer.data(), buffer.size());
}

// 来源：Bytes.hpp 的所有后续写入忽略；host 的 test_writer_bytes_and_zeros。
TEST_CASE("C++ ByteWriter bulk overflow blocks later writes", "[rtk_pure][bytes]")
{
    std::array<uint8_t, 6> buffer{};
    buffer.fill(0x77);
    const std::array<uint8_t, 2> source = {0x01, 0x02};
    ByteWriter writer(buffer);
    writer.bytes(source);
    writer.zeros(3);
    TEST_ASSERT_TRUE(writer.ok());
    writer.bytes(source); // 剩余一字节，不允许部分拷贝。
    TEST_ASSERT_FALSE(writer.ok());
    TEST_ASSERT_EQUAL_UINT(5, writer.size());
    writer.zeros(1);
    writer.fixed_str("A", 1);
    writer.bytes(std::span<const uint8_t>(source.data(), 1));
    TEST_ASSERT_FALSE(writer.ok());
    TEST_ASSERT_EQUAL_UINT(5, writer.size());
    TEST_ASSERT_EQUAL_UINT(5, writer.written().size());
    const std::array<uint8_t, 6> expected = {0x01, 0x02, 0, 0, 0, 0x77};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected.data(), buffer.data(), buffer.size());
}

// 来源：LinkManager.hpp 与 host 的 test_pairing_promotes_phone_role_to_alarm。
TEST_CASE("C++ LinkManager pairs initial phone role and remembers alarm", "[rtk_pure][pair]")
{
    LinkManager manager;
    Actions actions{};
    manager.init(Policy::kConcurrent, {});
    TEST_ASSERT_TRUE(Role::kPhone == manager.on_open(kAlarm, kAlarmAddr, 0, actions));
    manager.pair_open(10);
    TEST_ASSERT_TRUE(manager.is_pairing());
    TEST_ASSERT_TRUE(Role::kAlarm == manager.on_frame(kAlarm, core::kHelloSender, core::kHelloMsgId, 20, actions));
    TEST_ASSERT_EQUAL_UINT8(0, actions.count);
    TEST_ASSERT_TRUE(Role::kAlarm == manager.role(kAlarm));
    TEST_ASSERT_EQUAL_UINT(1, manager.whitelist().size());
    TEST_ASSERT_TRUE(kAlarmAddr == manager.whitelist()[0]);
    TEST_ASSERT_TRUE(Role::kAlarm == manager.on_frame(kAlarm, core::kHelloSender, core::kHelloMsgId, 30, actions));
    TEST_ASSERT_EQUAL_UINT(1, manager.whitelist().size());
    manager.on_close(kAlarm);
    TEST_ASSERT_TRUE(Role::kNone == manager.role(kAlarm));
    TEST_ASSERT_TRUE(Role::kAlarm == manager.on_open(kAlarm, kAlarmAddr, 40, actions));
    TEST_ASSERT_EQUAL_UINT8(0, actions.count);
}

// 来源：LinkManager.hpp 与 host 的 test_pending_when_phone_online_and_pairing。
TEST_CASE("C++ LinkManager pairs pending alarm while phone remains online", "[rtk_pure][pair]")
{
    LinkManager manager;
    Actions actions{};
    manager.init(Policy::kConcurrent, {});
    TEST_ASSERT_TRUE(Role::kPhone == manager.on_open(kPhone, kPhoneAddr, 0, actions));
    manager.pair_open(0);
    TEST_ASSERT_TRUE(manager.is_pairing());
    TEST_ASSERT_TRUE(Role::kPending == manager.on_open(kAlarm, kAlarmAddr, 100, actions));
    TEST_ASSERT_EQUAL_UINT8(0, actions.count);
    TEST_ASSERT_TRUE(Role::kAlarm == manager.on_frame(kAlarm, core::kHelloSender, core::kHelloMsgId, 200, actions));
    TEST_ASSERT_EQUAL_UINT8(0, actions.count);
    TEST_ASSERT_TRUE(Role::kPhone == manager.role(kPhone));
    TEST_ASSERT_EQUAL_UINT(1, manager.whitelist().size());
    TEST_ASSERT_TRUE(kAlarmAddr == manager.whitelist()[0]);
    manager.on_close(kAlarm);
    TEST_ASSERT_TRUE(Role::kNone == manager.role(kAlarm));
    TEST_ASSERT_TRUE(Role::kAlarm == manager.on_open(kAlarm, kAlarmAddr, 300, actions));
    TEST_ASSERT_EQUAL_UINT8(0, actions.count);
    TEST_ASSERT_TRUE(Role::kPhone == manager.role(kPhone));
}

// 来源：host 的 test_pair_window_expires、test_hello_without_pairing_is_unauthorized 与公共头阈值。
TEST_CASE("C++ LinkManager rejects hello at expired pairing window", "[rtk_pure][pair]")
{
    LinkManager manager;
    Actions actions{};
    manager.init(Policy::kConcurrent, {});
    manager.pair_open(500);
    manager.tick(500U + core::kPairWindowMs - 1U, actions);
    TEST_ASSERT_TRUE(manager.is_pairing());
    manager.tick(500U + core::kPairWindowMs, actions);
    TEST_ASSERT_FALSE(manager.is_pairing());
    TEST_ASSERT_TRUE(Role::kPhone == manager.on_open(kAlarm, kAlarmAddr, 500U + core::kPairWindowMs, actions));
    TEST_ASSERT_TRUE(Role::kNone == manager.on_frame(kAlarm, core::kHelloSender, core::kHelloMsgId,
                                                     501U + core::kPairWindowMs, actions));
    TEST_ASSERT_EQUAL_UINT8(1, actions.count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, actions.view()[0]);
    TEST_ASSERT_TRUE(Role::kNone == manager.role(kAlarm));
    TEST_ASSERT_EQUAL_UINT(0, manager.whitelist().size());
}

} // namespace

// 线程安全：仅由 IDF main 任务调用一次；ISR 不可调用 Unity runner。
extern "C" void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
