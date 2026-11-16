// GnssService 单元测试（K4 / ADR-0011）：6 路路由、interval 换算、支持位掩码门限（ADR-0012 §2.5）、<=256B 查询、>256B
// 计数与重连归零。

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "core/AlarmSource.hpp"
#include "core/CommandRouter.hpp"
#include "core/LinkManager.hpp"
#include "core/Reply.hpp"
#include "core/services/AlarmService.hpp"
#include "core/services/ConfigService.hpp"
#include "core/services/DeviceService.hpp"
#include "core/services/GnssService.hpp"
#include "core/services/PairService.hpp"
#include "proto/Frame.hpp"
#include "unity.h"

namespace {

using core::AlarmSource;
using core::CommandRouter;
using core::DispatchResult;
using core::LinkManager;
using core::Reply;
using core::Role;
using core::RouteTable;
using core::services::AlarmService;
using core::services::ConfigService;
using core::services::DeviceService;
using core::services::GnssQueryResult;
using core::services::GnssService;
using core::services::PairService;

inline constexpr uint16_t kMsgGga = 0x0004;
inline constexpr uint16_t kMsgSat = 0x0005;
inline constexpr uint16_t kMsgHeading = 0x001F;
inline constexpr uint16_t kMsgGst = 0x0021;
inline constexpr uint16_t kMsgGsa = 0x0023;
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;
inline constexpr size_t kRouteCountAll = 19;

struct MockCache {
    std::array<std::string_view, core::services::kGnssMsgCount> sentences{};
    std::array<bool, core::services::kGnssMsgCount> oversize{};
    uint32_t last_mask = 0;
    uint32_t mask_changes = 0;
};

GnssQueryResult query_cb(void *ctx, uint16_t msg_id, std::span<uint8_t> dst) noexcept
{
    auto *cache = static_cast<MockCache *>(ctx);
    const auto idx_opt = GnssService::type_index_of(msg_id);
    if (!idx_opt.has_value()) {
        return {};
    }
    const size_t idx = *idx_opt;
    if (cache->oversize[idx]) {
        return {.ok = false, .oversize = true, .len = 0};
    }
    const std::string_view sv = cache->sentences[idx];
    if (sv.empty() || (sv.size() > dst.size())) {
        return {};
    }
    std::memcpy(dst.data(), sv.data(), sv.size());
    return {.ok = true, .oversize = false, .len = sv.size()};
}

void mask_cb(void *ctx, uint32_t mask) noexcept
{
    auto *cache = static_cast<MockCache *>(ctx);
    cache->last_mask = mask;
    ++cache->mask_changes;
}

proto::FrameView make_req(uint16_t msg_id, uint8_t type, int8_t interval) noexcept
{
    proto::FrameView frame{};
    frame.msg_id = msg_id;
    frame.sender = proto::sender::kApp;
    frame.type = type;
    frame.interval = static_cast<uint8_t>(interval);
    return frame;
}

proto::ParseResult parse_reply_bytes(proto::FrameParser &parser, std::span<const uint8_t> raw,
                                     proto::FrameView &out) noexcept
{
    proto::ParseResult res = proto::ParseResult::kNone;
    for (const uint8_t b : raw) {
        res = parser.feed(b, out);
        if (res != proto::ParseResult::kNone) {
            return res;
        }
    }
    return res;
}

void assert_reply_result(const Reply &reply, uint16_t msg_id, uint8_t expect_type, uint8_t expect_code)
{
    TEST_ASSERT_EQUAL_UINT(proto::kHeaderLen + 2U + proto::kCrcLen, reply.len);
    proto::FrameParser parser;
    proto::FrameView out{};
    const auto res = parse_reply_bytes(parser, reply.bytes(), out);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(proto::ParseResult::kFrame), static_cast<uint8_t>(res));
    TEST_ASSERT_EQUAL_HEX16(msg_id, out.msg_id);
    TEST_ASSERT_EQUAL_HEX8(proto::sender::kDevice, out.sender);
    TEST_ASSERT_EQUAL_HEX8(expect_type, out.type);
    TEST_ASSERT_EQUAL_UINT(2U, out.payload.size());
    TEST_ASSERT_EQUAL_HEX8(expect_code, out.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(expect_code, out.payload[1]);
}

void assert_reply_payload(const Reply &reply, uint16_t msg_id, uint8_t expect_type, std::string_view expect_text)
{
    proto::FrameParser parser;
    proto::FrameView out{};
    const auto res = parse_reply_bytes(parser, reply.bytes(), out);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(proto::ParseResult::kFrame), static_cast<uint8_t>(res));
    TEST_ASSERT_EQUAL_HEX16(msg_id, out.msg_id);
    TEST_ASSERT_EQUAL_HEX8(proto::sender::kDevice, out.sender);
    TEST_ASSERT_EQUAL_HEX8(expect_type, out.type);
    TEST_ASSERT_EQUAL_UINT(expect_text.size(), out.payload.size());
    TEST_ASSERT_EQUAL_MEMORY(expect_text.data(), out.payload.data(), expect_text.size());
}

void test_interval_conversion()
{
    constexpr int8_t kRate5Hz = 5;
    constexpr int8_t kRate20Hz = 20;
    constexpr int8_t kRate50Hz = 50;
    constexpr int8_t kRateMax = 127;
    constexpr int8_t kSec5 = -5;
    constexpr int8_t kSecMin = -128;

    TEST_ASSERT_EQUAL_UINT32(0U, GnssService::interval_to_period_ms(0));
    TEST_ASSERT_EQUAL_UINT32(1000U, GnssService::interval_to_period_ms(1));
    TEST_ASSERT_EQUAL_UINT32(200U, GnssService::interval_to_period_ms(kRate5Hz));
    TEST_ASSERT_EQUAL_UINT32(50U, GnssService::interval_to_period_ms(kRate20Hz));
    TEST_ASSERT_EQUAL_UINT32(50U, GnssService::interval_to_period_ms(kRate50Hz));
    TEST_ASSERT_EQUAL_UINT32(50U, GnssService::interval_to_period_ms(kRateMax));
    TEST_ASSERT_EQUAL_UINT32(1000U, GnssService::interval_to_period_ms(-1));
    TEST_ASSERT_EQUAL_UINT32(5000U, GnssService::interval_to_period_ms(kSec5));
    TEST_ASSERT_EQUAL_UINT32(128000U, GnssService::interval_to_period_ms(kSecMin));
}

void test_router_integration_with_all_services()
{
    DeviceService dev;
    ConfigService cfg;
    AlarmSource src;
    LinkManager mgr;
    AlarmService alm(src, mgr, dev.info());
    PairService pair(mgr);
    GnssService gnss;
    const std::array<RouteTable, 5> tables = {
        dev.table(), cfg.table(), alm.table(), pair.table(), gnss.table(),
    };
    CommandRouter router;
    TEST_ASSERT_TRUE(fw::is_ok(router.init(tables)));
    TEST_ASSERT_EQUAL_UINT(kRouteCountAll, router.route_count());
}

void test_subscription_and_capability_gate()
{
    constexpr int8_t kSec5 = -5;
    constexpr uint32_t kTimeBase = 100;
    constexpr uint32_t kStepMs = 10;

    GnssService gnss;
    MockCache cache{};
    gnss.set_query_hook(&query_cb, &cache);
    gnss.set_mask_hook(&mask_cb, &cache);

    CommandRouter router;
    const std::array<RouteTable, 1> tables = {gnss.table()};
    TEST_ASSERT_TRUE(fw::is_ok(router.init(tables)));

    Reply reply{};
    // ALARM 不可访问 PHONE 路由。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kDenied),
                            static_cast<uint8_t>(router.handle(
                                Role::kAlarm, make_req(kMsgGga, proto::msg_type::kQuery, 1), kTimeBase, reply)));

    // 默认无航向能力，0x1F 订阅被拒绝且不置掩码。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, make_req(kMsgHeading, proto::msg_type::kQuery, 1), kTimeBase, reply)));
    assert_reply_result(reply, kMsgHeading, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.active_mask());

    // 接收机上报支持 HEADINGA 后 0x1F 订阅成功；0x05(SATSINFOA) 负周期 -5 -> 5000ms。
    gnss.set_supported_mask(core::services::kGnssDefaultSupportedMask | (1U << 2U));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(
            router.handle(Role::kPhone, make_req(kMsgHeading, proto::msg_type::kSet, 2), kTimeBase + kStepMs, reply)));
    assert_reply_result(reply, kMsgHeading, proto::msg_type::kSetRes, kResultOk);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(Role::kPhone, make_req(kMsgSat, proto::msg_type::kQuery, kSec5),
                                           kTimeBase + (2U * kStepMs), reply)));
    assert_reply_result(reply, kMsgSat, proto::msg_type::kQueryRes, kResultOk);
    TEST_ASSERT_EQUAL_HEX32((1U << 1U) | (1U << 2U), gnss.active_mask());
    TEST_ASSERT_EQUAL_UINT32(5000U, gnss.period_ms(1U));
    TEST_ASSERT_EQUAL_UINT32(500U, gnss.period_ms(2U));
    TEST_ASSERT_EQUAL_HEX32(gnss.active_mask(), cache.last_mask);

    // 断线重连后订阅掩码清零，不自动恢复推送。
    gnss.reset_session();
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.active_mask());
    TEST_ASSERT_EQUAL_UINT32(0U, gnss.period_ms(1U));
    TEST_ASSERT_EQUAL_HEX32(0U, cache.last_mask);
}

/** 支持位掩码（ADR-0012 §2.5）：不支持的语句类型两个分支都回 02 02；改掩码不取消已有订阅。 */
void test_unsupported_type_gate()
{
    constexpr uint32_t kTimeBase = 100;
    constexpr uint32_t kStepMs = 10;
    constexpr uint32_t kGsaBit = 1U << 5U;

    GnssService gnss;
    MockCache cache{};
    gnss.set_query_hook(&query_cb, &cache);
    gnss.set_mask_hook(&mask_cb, &cache);
    gnss.set_supported_mask(core::services::kGnssDefaultSupportedMask & ~kGsaBit);

    CommandRouter router;
    const std::array<RouteTable, 1> tables = {gnss.table()};
    TEST_ASSERT_TRUE(fw::is_ok(router.init(tables)));

    Reply reply{};
    // 开机订阅：GSA 不支持 -> 02 02，掩码保持 0。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, make_req(kMsgGsa, proto::msg_type::kQuery, 1), kTimeBase, reply)));
    assert_reply_result(reply, kMsgGsa, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.active_mask());

    // 关闭分支也一样被拒（不支持的类型不存在可关的订阅）。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(
            router.handle(Role::kPhone, make_req(kMsgGsa, proto::msg_type::kQuery, 0), kTimeBase + kStepMs, reply)));
    assert_reply_result(reply, kMsgGsa, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.active_mask());

    // 支持的 GGA 正常订阅。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(Role::kPhone, make_req(kMsgGga, proto::msg_type::kSet, 1),
                                           kTimeBase + (2U * kStepMs), reply)));
    assert_reply_result(reply, kMsgGga, proto::msg_type::kSetRes, kResultOk);
    TEST_ASSERT_EQUAL_HEX32(1U, gnss.active_mask());

    // 接收机能力变化只更新白名单，不取消已有订阅。
    gnss.set_supported_mask(core::services::kGnssDefaultSupportedMask & ~(1U << 0U));
    TEST_ASSERT_EQUAL_HEX32(1U, gnss.active_mask());
    TEST_ASSERT_EQUAL_UINT32(1000U, gnss.period_ms(0U));
}

void test_interval_zero_stops_and_queries_latest()
{
    constexpr uint32_t kTimeBase = 100;
    constexpr uint32_t kStepMs = 10;

    GnssService gnss;
    MockCache cache{};
    cache.sentences[0] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    cache.oversize[1] = true; // SATSINFOA > 256B
    gnss.set_query_hook(&query_cb, &cache);
    gnss.set_mask_hook(&mask_cb, &cache);

    CommandRouter router;
    const std::array<RouteTable, 1> tables = {gnss.table()};
    TEST_ASSERT_TRUE(fw::is_ok(router.init(tables)));

    Reply reply{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, make_req(kMsgGga, proto::msg_type::kQuery, 1), kTimeBase, reply)));
    TEST_ASSERT_EQUAL_HEX32(1U << 0U, gnss.active_mask());

    // interval=0：先关闭 0x04 订阅，再返回 <=256B 最新原句。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(
            router.handle(Role::kPhone, make_req(kMsgGga, proto::msg_type::kQuery, 0), kTimeBase + kStepMs, reply)));
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.active_mask());
    assert_reply_payload(reply, kMsgGga, proto::msg_type::kQueryRes, cache.sentences[0]);

    // 0x21(GST) 尚无缓存 -> 返回 02 02。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(Role::kPhone, make_req(kMsgGst, proto::msg_type::kQuery, 0),
                                           kTimeBase + (2U * kStepMs), reply)));
    assert_reply_result(reply, kMsgGst, proto::msg_type::kQueryRes, kResultFail);

    // 0x05(SATSINFOA) > 256B -> 返回 02 02 并递增 query_oversize。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(Role::kPhone, make_req(kMsgSat, proto::msg_type::kQuery, 0),
                                           kTimeBase + (3U * kStepMs), reply)));
    assert_reply_result(reply, kMsgSat, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_UINT32(1U, gnss.query_oversize_count());
}

} // namespace

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_interval_conversion);
    RUN_TEST(test_router_integration_with_all_services);
    RUN_TEST(test_subscription_and_capability_gate);
    RUN_TEST(test_unsupported_type_gate);
    RUN_TEST(test_interval_zero_stops_and_queries_latest);
    return UNITY_END();
}
