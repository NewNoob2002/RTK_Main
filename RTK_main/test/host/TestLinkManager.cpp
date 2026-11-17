// 验收测试：LinkManager（冻结输入）。期望值来自 LinkManager.hpp 契约。
#include <array>
#include <cstdint>

#include "core/LinkManager.hpp"
#include "unity.h"

using core::Actions;
using core::BtAddr;
using core::LinkManager;
using core::Policy;
using core::Role;
using core::SessionHandle;

static constexpr SessionHandle kPhone = 0x81;
static constexpr SessionHandle kAlarm = 0x82;
static constexpr SessionHandle kOther = 0x83;
static constexpr uint16_t kMsgQuery = 0x0001;

static constexpr BtAddr kPhoneAddr{{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
static constexpr BtAddr kAlarmAddr{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};
static constexpr BtAddr kOtherAddr{{0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}};

static LinkManager s_mgr;
static Actions s_act;

extern "C" void setUp(void)
{
    s_mgr = LinkManager{};
    s_act.handles.fill(0x5A5A5A5A); // 验证调用会先清空
    s_act.count = 3;
}
extern "C" void tearDown(void) {}

static void initWithAlarm(Policy policy)
{
    const std::array<BtAddr, 1> wl = {kAlarmAddr};
    s_mgr.init(policy, wl);
}

static void testWhitelistedIsAlarmUnknownIsPhone()
{
    initWithAlarm(Policy::kConcurrent);
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.onOpen(kPhone, kPhoneAddr, 100, s_act));
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.onOpen(kAlarm, kAlarmAddr, 200, s_act));
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.role(kPhone));
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.role(kAlarm));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kOther));
    TEST_ASSERT_TRUE(s_mgr.hasRole(Role::kPhone));
    TEST_ASSERT_FALSE(s_mgr.hasRole(Role::kPending));
}

static void testSecondPhoneRejectedWithoutPairing()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.onOpen(kOther, kOtherAddr, 10, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.view()[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kOther));
}

static void testDuplicateHandleAndFullTableRejected()
{
    initWithAlarm(Policy::kConcurrent);
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.onOpen(kPhone, kPhoneAddr, 1, s_act));
    static_cast<void>(s_mgr.onOpen(kAlarm, kAlarmAddr, 2, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.onOpen(kOther, kAlarmAddr, 3, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void testPreemptPhoneKicksAlarm()
{
    initWithAlarm(Policy::kPhonePreempt);
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.onOpen(kAlarm, kAlarmAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.onOpen(kPhone, kPhoneAddr, 5, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, s_act.handles[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kAlarm)); // 立即移除
}

static void testPreemptAlarmRejectedWhilePhone()
{
    initWithAlarm(Policy::kPhonePreempt);
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.onOpen(kAlarm, kAlarmAddr, 5, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, s_act.handles[0]);
    TEST_ASSERT_TRUE(Policy::kPhonePreempt == s_mgr.policy());
}

static void testCloseFreesSlot()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.onClose(kPhone);
    s_mgr.onClose(0x999); // 未知句柄忽略
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kPhone));
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.onOpen(kOther, kOtherAddr, 1, s_act));
}

static void testAlarmRxTimeoutWithWrap()
{
    const uint32_t t0 = 0xFFFFF000U; // 跨越 32 位回绕
    initWithAlarm(Policy::kConcurrent);
    static_cast<void>(s_mgr.onOpen(kAlarm, kAlarmAddr, t0, s_act));
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.onFrame(kAlarm, 0x03, 0x0041, t0 + 1000U, s_act));
    s_mgr.tick(t0 + 1000U + core::kAlarmRxTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count); // 等于阈值不断开
    s_mgr.tick(t0 + 1001U + core::kAlarmRxTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, s_act.handles[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kAlarm));
}

static void testPhoneHasNoRxTimeout()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.tick(600000, s_act);
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.role(kPhone));
}

static void testPairingPromotesPhoneRoleToAlarm()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kAlarm, kAlarmAddr, 0, s_act));
    s_mgr.pairOpen(10);
    TEST_ASSERT_TRUE(s_mgr.isPairing());
    TEST_ASSERT_TRUE(Role::kAlarm ==
                     s_mgr.onFrame(kAlarm, core::kHelloSender, core::kHelloMsgId, 20, s_act));
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    TEST_ASSERT_EQUAL_UINT(1, s_mgr.whitelist().size());
    TEST_ASSERT_TRUE(s_mgr.whitelist()[0] == kAlarmAddr);
    static_cast<void>(s_mgr.onFrame(kAlarm, core::kHelloSender, core::kHelloMsgId, 30, s_act));
    TEST_ASSERT_EQUAL_UINT(1, s_mgr.whitelist().size()); // 不重复加入
}

static void testPendingWhenPhoneOnlineAndPairing()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.pairOpen(0);
    TEST_ASSERT_TRUE(Role::kPending == s_mgr.onOpen(kAlarm, kAlarmAddr, 100, s_act));
    TEST_ASSERT_TRUE(Role::kAlarm ==
                     s_mgr.onFrame(kAlarm, core::kHelloSender, core::kHelloMsgId, 200, s_act));
}

static void testPendingNonHelloIsDisconnected()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.pairOpen(0);
    static_cast<void>(s_mgr.onOpen(kOther, kOtherAddr, 100, s_act));
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.onFrame(kOther, 0x00, kMsgQuery, 200, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
    TEST_ASSERT_TRUE(Role::kNone == s_mgr.role(kOther));
}

static void testPendingTimeout()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    s_mgr.pairOpen(0);
    static_cast<void>(s_mgr.onOpen(kOther, kOtherAddr, 1000, s_act));
    s_mgr.tick(1000U + core::kPendingTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(0, s_act.count);
    s_mgr.tick(1001U + core::kPendingTimeoutMs, s_act);
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void testHelloWithoutPairingIsUnauthorized()
{
    s_mgr.init(Policy::kConcurrent, {});
    static_cast<void>(s_mgr.onOpen(kOther, kOtherAddr, 0, s_act));
    TEST_ASSERT_TRUE(Role::kNone ==
                     s_mgr.onFrame(kOther, core::kHelloSender, core::kHelloMsgId, 10, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void testPairWindowExpires()
{
    s_mgr.init(Policy::kConcurrent, {});
    s_mgr.pairOpen(500);
    s_mgr.tick(500U + core::kPairWindowMs - 1U, s_act);
    TEST_ASSERT_TRUE(s_mgr.isPairing());
    s_mgr.tick(500U + core::kPairWindowMs, s_act);
    TEST_ASSERT_FALSE(s_mgr.isPairing());
}

static void testWhitelistFullRejectsNewAlarm()
{
    std::array<BtAddr, core::kMaxAlarms> full{};
    for (size_t i = 0; i < full.size(); ++i) {
        full[i].bytes.fill(static_cast<uint8_t>(0xC0U + i)); // i < 4
    }
    s_mgr.init(Policy::kConcurrent, full);
    static_cast<void>(s_mgr.onOpen(kOther, kOtherAddr, 0, s_act));
    s_mgr.pairOpen(0);
    TEST_ASSERT_TRUE(Role::kNone ==
                     s_mgr.onFrame(kOther, core::kHelloSender, core::kHelloMsgId, 10, s_act));
    TEST_ASSERT_EQUAL_UINT8(1, s_act.count);
    TEST_ASSERT_EQUAL_UINT32(kOther, s_act.handles[0]);
}

static void testInitTruncatesAndClear()
{
    std::array<BtAddr, core::kMaxAlarms + 2> many{};
    for (size_t i = 0; i < many.size(); ++i) {
        many[i].bytes.fill(static_cast<uint8_t>(i + 1U));
    }
    s_mgr.init(Policy::kConcurrent, many);
    TEST_ASSERT_EQUAL_UINT(core::kMaxAlarms, s_mgr.whitelist().size());
    s_mgr.whitelistClear();
    TEST_ASSERT_EQUAL_UINT(0, s_mgr.whitelist().size());
}

static void testHandlesAndClearKeepsRoles()
{
    initWithAlarm(Policy::kConcurrent);
    static_cast<void>(s_mgr.onOpen(kPhone, kPhoneAddr, 0, s_act));
    static_cast<void>(s_mgr.onOpen(kAlarm, kAlarmAddr, 0, s_act));
    std::array<SessionHandle, core::kMaxSessions> out{};
    TEST_ASSERT_EQUAL_UINT(1, s_mgr.handles(Role::kAlarm, out));
    TEST_ASSERT_EQUAL_UINT32(kAlarm, out[0]);
    TEST_ASSERT_EQUAL_UINT(0, s_mgr.handles(Role::kAlarm, std::span<SessionHandle>(out.data(), 0)));
    s_mgr.whitelistClear();
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.role(kAlarm));
}

static void testWhitelistAddRemoveSingle()
{
    using core::WhitelistEdit;
    initWithAlarm(Policy::kConcurrent);
    TEST_ASSERT_TRUE(WhitelistEdit::kUnchanged == s_mgr.whitelistAdd(kAlarmAddr)); // 已存在
    TEST_ASSERT_TRUE(WhitelistEdit::kDone == s_mgr.whitelistAdd(kOtherAddr));
    TEST_ASSERT_EQUAL_UINT(2, s_mgr.whitelist().size());
    TEST_ASSERT_TRUE(s_mgr.whitelist()[1] == kOtherAddr);
    // 手动加入的地址连接后直接是 ALARM。
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.onOpen(kOther, kOtherAddr, 0, s_act));
    // 删除首条：后续条目前移，已连接会话角色不变。
    static_cast<void>(s_mgr.onOpen(kAlarm, kAlarmAddr, 0, s_act));
    TEST_ASSERT_TRUE(WhitelistEdit::kDone == s_mgr.whitelistRemove(kAlarmAddr));
    TEST_ASSERT_EQUAL_UINT(1, s_mgr.whitelist().size());
    TEST_ASSERT_TRUE(s_mgr.whitelist()[0] == kOtherAddr);
    TEST_ASSERT_TRUE(Role::kAlarm == s_mgr.role(kAlarm));
    TEST_ASSERT_TRUE(WhitelistEdit::kUnchanged == s_mgr.whitelistRemove(kAlarmAddr)); // 不存在
    // 重连按新白名单判定：已有 ALARM 占位但无 PHONE → 未知地址成为 PHONE。
    s_mgr.onClose(kAlarm);
    TEST_ASSERT_TRUE(Role::kPhone == s_mgr.onOpen(kAlarm, kAlarmAddr, 10, s_act));
}

static void testWhitelistAddFull()
{
    std::array<BtAddr, core::kMaxAlarms> full{};
    for (size_t i = 0; i < full.size(); ++i) {
        full[i].bytes.fill(static_cast<uint8_t>(0xC0U + i));
    }
    s_mgr.init(Policy::kConcurrent, full);
    TEST_ASSERT_TRUE(core::WhitelistEdit::kFull == s_mgr.whitelistAdd(kOtherAddr));
    TEST_ASSERT_TRUE(core::WhitelistEdit::kDone == s_mgr.whitelistRemove(full[1]));
    TEST_ASSERT_TRUE(s_mgr.whitelist()[1] == full[2]);
    TEST_ASSERT_TRUE(core::WhitelistEdit::kDone == s_mgr.whitelistAdd(kOtherAddr));
    TEST_ASSERT_TRUE(s_mgr.whitelist()[core::kMaxAlarms - 1U] == kOtherAddr);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testWhitelistedIsAlarmUnknownIsPhone);
    RUN_TEST(testWhitelistAddRemoveSingle);
    RUN_TEST(testWhitelistAddFull);
    RUN_TEST(testSecondPhoneRejectedWithoutPairing);
    RUN_TEST(testDuplicateHandleAndFullTableRejected);
    RUN_TEST(testPreemptPhoneKicksAlarm);
    RUN_TEST(testPreemptAlarmRejectedWhilePhone);
    RUN_TEST(testCloseFreesSlot);
    RUN_TEST(testAlarmRxTimeoutWithWrap);
    RUN_TEST(testPhoneHasNoRxTimeout);
    RUN_TEST(testPairingPromotesPhoneRoleToAlarm);
    RUN_TEST(testPendingWhenPhoneOnlineAndPairing);
    RUN_TEST(testPendingNonHelloIsDisconnected);
    RUN_TEST(testPendingTimeout);
    RUN_TEST(testHelloWithoutPairingIsUnauthorized);
    RUN_TEST(testPairWindowExpires);
    RUN_TEST(testWhitelistFullRejectsNewAlarm);
    RUN_TEST(testInitTruncatesAndClear);
    RUN_TEST(testHandlesAndClearKeepsRoles);
    return UNITY_END();
}
