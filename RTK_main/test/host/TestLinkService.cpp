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
    explicit Fixture(Policy policy, bool alarm_whitelisted)
    {
        const std::array<BtAddr, 1> wl = {kAlarmAddr};
        link.init(policy,
                  alarm_whitelisted ? std::span<const BtAddr>(wl) : std::span<const BtAddr>());
        // J2b：原来的 CommandDispatcher 换成 4 个服务 + CommandRouter（组装方式与 main 一致）。
        const std::array<core::RouteTable, 4> tables = {device.table(), config.table(),
                                                        alarm_service.table(), pair.table()};
        TEST_ASSERT_TRUE(isOk(router.init(tables)));
    }
    test::FakeTransport transport;
    core::AlarmSource alarm;
    core::LinkManager link;
    core::services::DeviceService device;
    core::services::ConfigService config;
    // AlarmService 持有 DeviceService::info() 的引用，声明顺序必须在其后。
    core::services::AlarmService alarm_service{alarm, link, device.info()};
    core::services::PairService pair{link};
    core::CommandRouter router;
    core::LinkService svc{transport, link, router, alarm_service};
};

struct Bytes {
    std::array<uint8_t, proto::kMaxFrame * 2> data{};
    size_t len = 0;
    std::span<const uint8_t> view() const
    {
        return {data.data(), len};
    }
    void append(uint16_t msg_id, uint8_t sender, uint8_t type,
                std::span<const uint8_t> payload = {})
    {
        size_t n = 0;
        TEST_ASSERT_TRUE(
            isOk(proto::build_frame(std::span<uint8_t>(data.data() + len, data.size() - len),
                                    msg_id, sender, type, payload, n)));
        len += n;
    }
};

Bytes frame(uint16_t msg_id, uint8_t sender, uint8_t type, std::span<const uint8_t> payload = {})
{
    Bytes b;
    b.append(msg_id, sender, type, payload);
    return b;
}

constexpr std::array<uint8_t, 8> kHeartbeat = {0x05, 0x00, 0x50, 0x00, 0, 0, 0, 0};

// 解析 FakeTransport 第 idx 次写入，返回 msg_id 与 type。
test::FeedResult written(const test::FakeTransport &t, size_t idx)
{
    proto::FrameParser parser;
    return test::feed_all(parser,
                          std::span<const uint8_t>(t.writes[idx].data.data(), t.writes[idx].len));
}

} // namespace

static void test_phone_query_gets_reply()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.on_data(kPhone, q.view(), 10);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.write_count);
    TEST_ASSERT_EQUAL_UINT32(kPhone, f.transport.writes[0].handle);
    TEST_ASSERT_EQUAL_UINT32(core::kWriteTimeoutMs, f.transport.last_timeout_ms);
    auto r = written(f.transport, 0);
    TEST_ASSERT_EQUAL_INT(1, r.frames);
    TEST_ASSERT_EQUAL_HEX16(0x0001, r.last.msg_id);
    TEST_ASSERT_EQUAL_HEX8(mt::kQueryRes, r.last.type);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().frames_rx);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().replies_tx);
}

static void test_split_frame_and_interleaved_sessions()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    f.svc.on_open(kAlarm, kAlarmAddr, 0);
    auto q = frame(0x0002, 0, mt::kQuery);
    auto sq = frame(ac::kMsgStatus, 3, mt::kQuery);
    // 两个会话交替送半帧：每会话独立解析器，互不干扰
    f.svc.on_data(kPhone, q.view().subspan(0, 7), 1);
    f.svc.on_data(kAlarm, sq.view().subspan(0, 11), 2);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.write_count);
    f.svc.on_data(kPhone, q.view().subspan(7), 3);
    f.svc.on_data(kAlarm, sq.view().subspan(11), 4);
    TEST_ASSERT_EQUAL_UINT(2, f.transport.write_count);
    TEST_ASSERT_EQUAL_UINT32(kPhone, f.transport.writes[0].handle);
    TEST_ASSERT_EQUAL_HEX16(0x0002, written(f.transport, 0).last.msg_id);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.writes[1].handle);
    TEST_ASSERT_EQUAL_HEX16(ac::kMsgStatus, written(f.transport, 1).last.msg_id);
}

static void test_rejected_open_is_disconnected_and_ignored()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    f.svc.on_open(kOther, kOtherAddr, 1); // 第二部手机，无配对窗口
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnect_count);
    TEST_ASSERT_EQUAL_UINT32(kOther, f.transport.disconnects[0]);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().disconnects);
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.on_data(kOther, q.view(), 2);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.write_count); // 无解析器：数据丢弃
}

static void test_preempt_disconnects_alarm()
{
    Fixture f(Policy::kPhonePreempt, true);
    f.svc.on_open(kAlarm, kAlarmAddr, 0);
    f.svc.on_open(kPhone, kPhoneAddr, 5);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnect_count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.disconnects[0]);
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.on_data(kAlarm, hb.view(), 6); // 被踢的句柄：解析器已释放
    TEST_ASSERT_FALSE(f.svc.last_heartbeat().has_value());
}

static void test_status_push_period_and_seq()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    f.svc.on_open(kAlarm, kAlarmAddr, 0);
    f.svc.tick(100); // 首次调用即推送
    TEST_ASSERT_EQUAL_UINT(1, f.transport.write_count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.writes[0].handle); // 只推给 ALARM
    auto r0 = written(f.transport, 0);
    TEST_ASSERT_EQUAL_HEX16(ac::kMsgStatus, r0.last.msg_id);
    TEST_ASSERT_EQUAL_HEX8(mt::kPush, r0.last.type);
    const uint16_t seq0 = static_cast<uint16_t>(r0.payload[0] | (r0.payload[1] << 8U));

    // 保持报警会话存活
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.on_data(kAlarm, hb.view(), 900);
    f.svc.tick(1099); // 不足 1000 ms
    TEST_ASSERT_EQUAL_UINT(1, f.transport.write_count);
    f.svc.tick(1100);
    TEST_ASSERT_EQUAL_UINT(2, f.transport.write_count);
    auto r1 = written(f.transport, 1);
    const uint16_t seq1 = static_cast<uint16_t>(r1.payload[0] | (r1.payload[1] << 8U));
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(seq0 + 1U), seq1);
    TEST_ASSERT_EQUAL_UINT32(2, f.svc.stats().pushes_tx);
    TEST_ASSERT_EQUAL_HEX8(ac::kFlagPhoneOnline, r1.payload[6]);
}

static void test_alarm_timeout_disconnects()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.on_open(kAlarm, kAlarmAddr, 0);
    f.svc.tick(core::kAlarmRxTimeoutMs + 1U);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnect_count);
    TEST_ASSERT_EQUAL_UINT32(kAlarm, f.transport.disconnects[0]);
    f.transport.clear();
    f.svc.tick(core::kAlarmRxTimeoutMs + 2000U);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.write_count); // 已移除，不再推送
}

static void test_heartbeat_recorded()
{
    Fixture f(Policy::kConcurrent, true);
    f.svc.on_open(kAlarm, kAlarmAddr, 0);
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.on_data(kAlarm, hb.view(), 10);
    TEST_ASSERT_TRUE(f.svc.last_heartbeat().has_value());
    TEST_ASSERT_EQUAL_HEX16(0x0005, f.svc.last_heartbeat()->seq);
    TEST_ASSERT_EQUAL_UINT8(80, f.svc.last_heartbeat()->battery_pct);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.write_count);
}

static void test_pairing_marks_whitelist_dirty()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    const std::array<uint8_t, 1> open = {1};
    auto pair = frame(ac::kMsgPair, 0, mt::kSet, open);
    f.svc.on_data(kPhone, pair.view(), 10);
    TEST_ASSERT_FALSE(f.svc.consume_whitelist_dirty()); // 打开窗口本身不改白名单

    f.svc.on_open(kAlarm, kAlarmAddr, 20); // PENDING
    auto hb = frame(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat);
    f.svc.on_data(kAlarm, hb.view(), 30);
    TEST_ASSERT_TRUE(Role::kAlarm == f.link.role(kAlarm));
    TEST_ASSERT_TRUE(f.svc.consume_whitelist_dirty());
    TEST_ASSERT_FALSE(f.svc.consume_whitelist_dirty()); // 读取即清除

    const std::array<uint8_t, 1> clear = {0};
    auto wl_clear = frame(ac::kMsgPair, 0, mt::kSet, clear);
    f.svc.on_data(kPhone, wl_clear.view(), 40);
    TEST_ASSERT_TRUE(f.svc.consume_whitelist_dirty());
}

static void test_pending_disconnect_drops_rest_of_buffer()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    f.link.pair_open(0);
    f.svc.on_open(kOther, kOtherAddr, 1); // PENDING
    Bytes b;
    b.append(0x0001, 0, mt::kQuery);                       // 非 HELLO → 断开
    b.append(ac::kMsgHeartbeat, 3, mt::kPush, kHeartbeat); // 同一缓冲区中的后续帧必须被丢弃
    f.svc.on_data(kOther, b.view(), 2);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.disconnect_count);
    TEST_ASSERT_EQUAL_UINT32(kOther, f.transport.disconnects[0]);
    TEST_ASSERT_EQUAL_UINT(0, f.transport.write_count);
    TEST_ASSERT_EQUAL_UINT(0, f.link.whitelist().size());
    TEST_ASSERT_FALSE(f.svc.consume_whitelist_dirty());
}

static void test_write_failure_counted()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    f.transport.fail_writes = true;
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.on_data(kPhone, q.view(), 1);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.stats().write_errors);
    TEST_ASSERT_EQUAL_UINT32(0, f.svc.stats().replies_tx);
}

static void test_error_counters_and_rx_drop()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    auto bad = frame(0x0001, 0, mt::kQuery);
    bad.data[bad.len - 1U] ^= 0xFFU;
    f.svc.on_data(kPhone, bad.view(), 1);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.parser_crc_errors());
    f.svc.on_close(kPhone);
    TEST_ASSERT_EQUAL_UINT32(1, f.svc.parser_crc_errors()); // 释放槽位后累计值不回退
    f.svc.on_rx_dropped(5);
    f.svc.on_rx_dropped(7);
    TEST_ASSERT_EQUAL_UINT32(12, f.svc.stats().rx_dropped);
    TEST_ASSERT_EQUAL_UINT32(0, f.svc.parser_length_errors());
}

static void test_reopen_gets_fresh_parser()
{
    Fixture f(Policy::kConcurrent, false);
    f.svc.on_open(kPhone, kPhoneAddr, 0);
    auto q = frame(0x0001, 0, mt::kQuery);
    f.svc.on_data(kPhone, q.view().subspan(0, 10), 1); // 留下半帧
    f.svc.on_close(kPhone);
    f.svc.on_open(kPhone, kPhoneAddr, 2);
    f.svc.on_data(kPhone, q.view(), 3);
    TEST_ASSERT_EQUAL_UINT(1, f.transport.write_count);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_phone_query_gets_reply);
    RUN_TEST(test_split_frame_and_interleaved_sessions);
    RUN_TEST(test_rejected_open_is_disconnected_and_ignored);
    RUN_TEST(test_preempt_disconnects_alarm);
    RUN_TEST(test_status_push_period_and_seq);
    RUN_TEST(test_alarm_timeout_disconnects);
    RUN_TEST(test_heartbeat_recorded);
    RUN_TEST(test_pairing_marks_whitelist_dirty);
    RUN_TEST(test_pending_disconnect_drops_rest_of_buffer);
    RUN_TEST(test_write_failure_counted);
    RUN_TEST(test_error_counters_and_rx_drop);
    RUN_TEST(test_reopen_gets_fresh_parser);
    return UNITY_END();
}
