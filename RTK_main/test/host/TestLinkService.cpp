// 验收测试：LinkService 端到端（冻结输入）。期望值来自 LinkService.hpp 契约与 phone_protocol.md。
// 用 FakeTransport 代替 SPP，验证 C 版中没有测试覆盖的“事件 → 解析 → 角色 → 分发 → 写回”胶水逻辑。
#include <array>
#include <cstdint>
#include <cstring>

#include "core/LinkService.hpp"
#include "core/services/ConfigService.hpp"
#include "core/services/DeviceService.hpp"
#include "core/services/PairService.hpp"
#include "TestSupport.hpp"
#include "unity.h"

namespace ac = proto::alarm;
namespace mt = proto::msg_type;
using core::BtAddr;
using core::Policy;
using core::Role;
using core::SessionHandle;

static constexpr SessionHandle kPhone = 0x81;
static constexpr SessionHandle kAlarm = 0x82;
static constexpr SessionHandle kOther = 0x83;
static constexpr BtAddr kPhoneAddr{{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
static constexpr BtAddr kAlarmAddr{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};
static constexpr BtAddr kOtherAddr{{0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}};

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

namespace {

struct Fixture {
    explicit Fixture(Policy policy, bool alarmWhitelisted)
    {
        const std::array<BtAddr, 1> wl = {kAlarmAddr};
        link.init(policy,
                  alarmWhitelisted ? std::span<const BtAddr>(wl) : std::span<const BtAddr>());
        // ADR-0007 §3.5：原来的 CommandDispatcher 换成 4 个服务 + CommandRouter（组装方式与 main 一致）。
        const std::array<core::RouteTable, 4> tables = {device.table(), config.table(),
                                                        alarmService.table(), pair.table()};
        TEST_ASSERT_TRUE(isOk(router.init(tables)));
    }
    test::FakeTransport transport;
    core::AlarmSource alarm;
    core::LinkManager link;
    core::services::DeviceService device;
    core::services::ConfigService config;
    // AlarmService 持有 DeviceService::info() 的引用，声明顺序必须在其后。
    core::services::AlarmService alarmService{alarm, link, device.info()};
    core::services::PairService pair{link};
    core::CommandRouter router;
    core::LinkService svc{transport, link, router, alarmService};
};

struct Bytes {
    std::array<uint8_t, proto::kMaxFrame * 2> data{};
    size_t len = 0;
    std::span<const uint8_t> view() const
    {
        return {data.data(), len};
    }
    void append(uint16_t msgId, uint8_t sender, uint8_t type, std::span<const uint8_t> payload = {})
    {
        size_t n = 0;
        TEST_ASSERT_TRUE(
            isOk(proto::buildFrame(std::span<uint8_t>(data.data() + len, data.size() - len), msgId,
                                   sender, type, payload, n)));
        len += n;
    }
};

Bytes frame(uint16_t msgId, uint8_t sender, uint8_t type, std::span<const uint8_t> payload = {})
{
    Bytes b;
    b.append(msgId, sender, type, payload);
    return b;
}

constexpr std::array<uint8_t, 8> kHeartbeat = {0x05, 0x00, 0x50, 0x00, 0, 0, 0, 0};

// 解析 FakeTransport 第 idx 次写入，返回 msgId 与 type。
test::FeedResult written(const test::FakeTransport &t, size_t idx)
{
    proto::FrameParser parser;
    return test::feedAll(parser,
                         std::span<const uint8_t>(t.writes[idx].data.data(), t.writes[idx].len));
}

} // namespace

static void testPhoneQueryGetsReply()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.onData(kPhone, q.view(), 10);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.writeCount);
    TEST_ASSERT_EQUAL_UINT32(kPhone, f.transport.writes[0].handle);
    TEST_ASSERT_EQUAL_UINT32(core::kWriteTimeoutMs, f.transport.lastTimeoutMs);
    auto r = written(f.transport, 0);
    TEST_ASSERT_EQUAL_INT(1, r.frames);
    TEST_ASSERT_EQUAL_HEX16(0x0001, r.last.msgId);
    TEST_ASSERT_EQUAL_HEX8(mt::kQueryRes, r.last.type);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().framesRx);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().repliesTx);
}

static void testSplitFrameAndInterleavedSessions()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    f.svc.onOpen(kAlarm, kAlarmAddr, 0);
    auto q = frame(0x0002, 0, mt::kQuery);
    auto sq = frame(ac::kMsgStatus, 3, mt::kQuery);
    // 两个会话交替送半帧：每会话独立解析器，互不干扰
    f.svc.onData(kPhone, q.view().subspan(0, 7), 1);
    f.svc.onData(kAlarm, sq.view().subspan(0, 11), 2);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.writeCount);
    f.svc.onData(kPhone, q.view().subspan(7), 3);
    f.svc.onData(kAlarm, sq.view().subspan(11), 4);
    TEST_ASSERT_EQUAL_UINT(2, f.transport.writeCount);
    TEST_ASSERT_EQUAL_UINT32(kPhone, f.transport.writes[0].handle);
    TEST_ASSERT_EQUAL_HEX16(0x0002, written(f.transport, 0).last.msgId);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.writes[1].handle);
    TEST_ASSERT_EQUAL_HEX16(ac::kMsgStatus, written(f.transport, 1).last.msgId);
}

static void testRejectedOpenIsDisconnectedAndIgnored()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    f.svc.onOpen(kOther, kOtherAddr, 1); // 第二部手机，无配对窗口
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnectCount);
    TEST_ASSERT_EQUAL_UINT32(kOther, f.transport.disconnects[0]);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().disconnects);
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.onData(kOther, q.view(), 2);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.writeCount); // 无解析器：数据丢弃
}

static void testPreemptDisconnectsAlarm()
{
    Fixture f(Policy::kPhonePreempt, true);
    f.svc.onOpen(kAlarm, kAlarmAddr, 0);
    f.svc.onOpen(kPhone, kPhoneAddr, 5);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnectCount);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.disconnects[0]);
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.onData(kAlarm, hb.view(), 6); // 被踢的句柄：解析器已释放
    TEST_ASSERT_FALSE(f.svc.lastHeartbeat().has_value());
}

static void testStatusPushPeriodAndSeq()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    f.svc.onOpen(kAlarm, kAlarmAddr, 0);
    f.svc.tick(100); // 首次调用即推送
    TEST_ASSERT_EQUAL_UINT(1, f.transport.writeCount);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.writes[0].handle); // 只推给 ALARM
    auto r0 = written(f.transport, 0);
    TEST_ASSERT_EQUAL_HEX16(ac::kMsgStatus, r0.last.msgId);
    TEST_ASSERT_EQUAL_HEX8(mt::kPush, r0.last.type);
    const uint16_t seq0 = static_cast<uint16_t>(r0.payload[0] | (r0.payload[1] << 8U));

    // 保持报警会话存活
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.onData(kAlarm, hb.view(), 900);
    f.svc.tick(1099); // 不足 1000 ms
    TEST_ASSERT_EQUAL_UINT(1, f.transport.writeCount);
    f.svc.tick(1100);
    TEST_ASSERT_EQUAL_UINT(2, f.transport.writeCount);
    auto r1 = written(f.transport, 1);
    const uint16_t seq1 = static_cast<uint16_t>(r1.payload[0] | (r1.payload[1] << 8U));
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(seq0 + 1U), seq1);
    TEST_ASSERT_EQUAL_UINT32(2, f.svc.stats().pushesTx);
    TEST_ASSERT_EQUAL_HEX8(ac::kFlagPhoneOnline, r1.payload[6]);
}

static void testAlarmTimeoutDisconnects()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.onOpen(kAlarm, kAlarmAddr, 0);
    f.svc.tick(core::kAlarmRxTimeoutMs + 1U);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnectCount);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.disconnects[0]);
    f.transport.clear();
    f.svc.tick(core::kAlarmRxTimeoutMs + 2000U);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.writeCount); // 已移除，不再推送
}

static void testHeartbeatRecorded()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.onOpen(kAlarm, kAlarmAddr, 0);
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.onData(kAlarm, hb.view(), 10);
    TEST_ASSERT_TRUE(f.svc.lastHeartbeat().has_value());
    TEST_ASSERT_EQUAL_HEX16(0x0005, f.svc.lastHeartbeat()->seq);
    TEST_ASSERT_EQUAL_UINT8(80, f.svc.lastHeartbeat()->batteryPct);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.writeCount);
}

static void testPairingMarksWhitelistDirty()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    const std::array<uint8_t, 1> open = {1};
    auto pair = frame(ac::kMsgPair, 0, mt::kSet, open);
    f.svc.onData(kPhone, pair.view(), 10);
    TEST_ASSERT_FALSE(f.svc.consumeWhitelistDirty()); // 打开窗口本身不改白名单

    f.svc.onOpen(kAlarm, kAlarmAddr, 20); // PENDING
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.onData(kAlarm, hb.view(), 30);
    TEST_ASSERT_TRUE(Role::kAlarm == f.link.role(kAlarm));
    TEST_ASSERT_TRUE(f.svc.consumeWhitelistDirty());
    TEST_ASSERT_FALSE(f.svc.consumeWhitelistDirty()); // 读取即清除

    const std::array<uint8_t, 1> clear = {0};
    auto wlClear = frame(ac::kMsgPair, 0, mt::kSet, clear);
    f.svc.onData(kPhone, wlClear.view(), 40);
    TEST_ASSERT_TRUE(f.svc.consumeWhitelistDirty());
}

static void testPendingDisconnectDropsRestOfBuffer()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    f.link.pairOpen(0);
    f.svc.onOpen(kOther, kOtherAddr, 1); // PENDING
    Bytes b;
    b.append(0x0001, 0, mt::kQuery);                       // 非 HELLO → 断开
    b.append(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat); // 同一缓冲区中的后续帧必须被丢弃
    f.svc.onData(kOther, b.view(), 2);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnectCount);
    TEST_ASSERT_EQUAL_UINT32(kOther, f.transport.disconnects[0]);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.writeCount);
    TEST_ASSERT_EQUAL_UINT(0, f.link.whitelist().size());
    TEST_ASSERT_FALSE(f.svc.consumeWhitelistDirty());
}

static void testWriteFailureCounted()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    f.transport.failWrites = true;
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.onData(kPhone, q.view(), 1);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().writeErrors);
    TEST_ASSERT_EQUAL_UINT32(0, f.svc.stats().repliesTx);
}

static void testErrorCountersAndRxDrop()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    auto bad = frame(0x0001, 0, mt::kQuery);
    bad.data[bad.len - 1U] ^= 0xFFU;
    f.svc.onData(kPhone, bad.view(), 1);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.parserCrcErrors());
    f.svc.onClose(kPhone);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.parserCrcErrors()); // 释放槽位后累计值不回退
    f.svc.onRxDropped(5);
    f.svc.onRxDropped(7);
    TEST_ASSERT_EQUAL_UINT32(12, f.svc.stats().rxDropped);
    TEST_ASSERT_EQUAL_UINT32(0, f.svc.parserLengthErrors());
}

static void testReopenGetsFreshParser()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.onOpen(kPhone, kPhoneAddr, 0);
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.onData(kPhone, q.view().subspan(0, 10), 1); // 留下半帧
    f.svc.onClose(kPhone);
    f.svc.onOpen(kPhone, kPhoneAddr, 2);
    f.svc.onData(kPhone, q.view(), 3);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.writeCount);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testPhoneQueryGetsReply);
    RUN_TEST(testSplitFrameAndInterleavedSessions);
    RUN_TEST(testRejectedOpenIsDisconnectedAndIgnored);
    RUN_TEST(testPreemptDisconnectsAlarm);
    RUN_TEST(testStatusPushPeriodAndSeq);
    RUN_TEST(testAlarmTimeoutDisconnects);
    RUN_TEST(testHeartbeatRecorded);
    RUN_TEST(testPairingMarksWhitelistDirty);
    RUN_TEST(testPendingDisconnectDropsRestOfBuffer);
    RUN_TEST(testWriteFailureCounted);
    RUN_TEST(testErrorCountersAndRxDrop);
    RUN_TEST(testReopenGetsFreshParser);
    return UNITY_END();
}
