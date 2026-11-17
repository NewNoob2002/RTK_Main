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
    uint32_t lastMask = 0;
    uint32_t maskChanges = 0;
};

GnssQueryResult queryCb(void *ctx, uint16_t msgId, std::span<uint8_t> dst) noexcept
{
    auto *cache = static_cast<MockCache *>(ctx);
    const auto idxOpt = GnssService::typeIndexOf(msgId);
    if (!idxOpt.has_value()) {
        return {};
    }
    const size_t idx = *idxOpt;
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

void maskCb(void *ctx, uint32_t mask) noexcept
{
    auto *cache = static_cast<MockCache *>(ctx);
    cache->lastMask = mask;
    ++cache->maskChanges;
}

proto::FrameView makeReq(uint16_t msgId, uint8_t type, int8_t interval) noexcept
{
    proto::FrameView frame{};
    frame.msgId = msgId;
    frame.sender = proto::sender::kApp;
    frame.type = type;
    frame.interval = static_cast<uint8_t>(interval);
    return frame;
}

proto::ParseResult parseReplyBytes(proto::FrameParser &parser, std::span<const uint8_t> raw,
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

void assertReplyResult(const Reply &reply, uint16_t msgId, uint8_t expectType, uint8_t expectCode)
{
    TEST_ASSERT_EQUAL_UINT(proto::kHeaderLen + 2U + proto::kCrcLen, reply.len);
    proto::FrameParser parser;
    proto::FrameView out{};
    const auto res = parseReplyBytes(parser, reply.bytes(), out);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(proto::ParseResult::kFrame),
                            static_cast<uint8_t>(res));
    TEST_ASSERT_EQUAL_HEX16(msgId, out.msgId);
    TEST_ASSERT_EQUAL_HEX8(proto::sender::kDevice, out.sender);
    TEST_ASSERT_EQUAL_HEX8(expectType, out.type);
    TEST_ASSERT_EQUAL_UINT(2U, out.payload.size());
    TEST_ASSERT_EQUAL_HEX8(expectCode, out.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(expectCode, out.payload[1]);
}

void assertReplyPayload(const Reply &reply, uint16_t msgId, uint8_t expectType,
                        std::string_view expectText)
{
    proto::FrameParser parser;
    proto::FrameView out{};
    const auto res = parseReplyBytes(parser, reply.bytes(), out);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(proto::ParseResult::kFrame),
                            static_cast<uint8_t>(res));
    TEST_ASSERT_EQUAL_HEX16(msgId, out.msgId);
    TEST_ASSERT_EQUAL_HEX8(proto::sender::kDevice, out.sender);
    TEST_ASSERT_EQUAL_HEX8(expectType, out.type);
    TEST_ASSERT_EQUAL_UINT(expectText.size(), out.payload.size());
    TEST_ASSERT_EQUAL_MEMORY(expectText.data(), out.payload.data(), expectText.size());
}

void testIntervalConversion()
{
    constexpr int8_t kRate5Hz = 5;
    constexpr int8_t kRate20Hz = 20;
    constexpr int8_t kRate50Hz = 50;
    constexpr int8_t kRateMax = 127;
    constexpr int8_t kSec5 = -5;
    constexpr int8_t kSecMin = -128;

    TEST_ASSERT_EQUAL_UINT32(0U, GnssService::intervalToPeriodMs(0));
    TEST_ASSERT_EQUAL_UINT32(1000U, GnssService::intervalToPeriodMs(1));
    TEST_ASSERT_EQUAL_UINT32(200U, GnssService::intervalToPeriodMs(kRate5Hz));
    TEST_ASSERT_EQUAL_UINT32(50U, GnssService::intervalToPeriodMs(kRate20Hz));
    TEST_ASSERT_EQUAL_UINT32(50U, GnssService::intervalToPeriodMs(kRate50Hz));
    TEST_ASSERT_EQUAL_UINT32(50U, GnssService::intervalToPeriodMs(kRateMax));
    TEST_ASSERT_EQUAL_UINT32(1000U, GnssService::intervalToPeriodMs(-1));
    TEST_ASSERT_EQUAL_UINT32(5000U, GnssService::intervalToPeriodMs(kSec5));
    TEST_ASSERT_EQUAL_UINT32(128000U, GnssService::intervalToPeriodMs(kSecMin));
}

void testRouterIntegrationWithAllServices()
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
    TEST_ASSERT_TRUE(fw::isOk(router.init(tables)));
    TEST_ASSERT_EQUAL_UINT(kRouteCountAll, router.routeCount());
}

void testSubscriptionAndCapabilityGate()
{
    constexpr int8_t kSec5 = -5;
    constexpr uint32_t kTimeBase = 100;
    constexpr uint32_t kStepMs = 10;

    GnssService gnss;
    MockCache cache{};
    gnss.setQueryHook(&queryCb, &cache);
    gnss.setMaskHook(&maskCb, &cache);

    CommandRouter router;
    const std::array<RouteTable, 1> tables = {gnss.table()};
    TEST_ASSERT_TRUE(fw::isOk(router.init(tables)));

    Reply reply{};
    // ALARM 不可访问 PHONE 路由。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kDenied),
        static_cast<uint8_t>(router.handle(
            Role::kAlarm, makeReq(kMsgGga, proto::msg_type::kQuery, 1), kTimeBase, reply)));

    // 默认无航向能力，0x1F 订阅被拒绝且不置掩码。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(
            Role::kPhone, makeReq(kMsgHeading, proto::msg_type::kQuery, 1), kTimeBase, reply)));
    assertReplyResult(reply, kMsgHeading, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.activeMask());

    // 接收机上报支持 HEADINGA 后 0x1F 订阅成功；0x05(SATSINFOA) 负周期 -5 -> 5000ms。
    gnss.setSupportedMask(core::services::kGnssDefaultSupportedMask | (1U << 2U));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, makeReq(kMsgHeading, proto::msg_type::kSet, 2),
                                kTimeBase + kStepMs, reply)));
    assertReplyResult(reply, kMsgHeading, proto::msg_type::kSetRes, kResultOk);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, makeReq(kMsgSat, proto::msg_type::kQuery, kSec5),
                                kTimeBase + (2U * kStepMs), reply)));
    assertReplyResult(reply, kMsgSat, proto::msg_type::kQueryRes, kResultOk);
    TEST_ASSERT_EQUAL_HEX32((1U << 1U) | (1U << 2U), gnss.activeMask());
    TEST_ASSERT_EQUAL_UINT32(5000U, gnss.periodMs(1U));
    TEST_ASSERT_EQUAL_UINT32(500U, gnss.periodMs(2U));
    TEST_ASSERT_EQUAL_HEX32(gnss.activeMask(), cache.lastMask);

    // 断线重连后订阅掩码清零，不自动恢复推送。
    gnss.resetSession();
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.activeMask());
    TEST_ASSERT_EQUAL_UINT32(0U, gnss.periodMs(1U));
    TEST_ASSERT_EQUAL_HEX32(0U, cache.lastMask);
}

/** 支持位掩码（ADR-0012 §2.5）：不支持的语句类型两个分支都回 02 02；改掩码不取消已有订阅。 */
void testUnsupportedTypeGate()
{
    constexpr uint32_t kTimeBase = 100;
    constexpr uint32_t kStepMs = 10;
    constexpr uint32_t kGsaBit = 1U << 5U;

    GnssService gnss;
    MockCache cache{};
    gnss.setQueryHook(&queryCb, &cache);
    gnss.setMaskHook(&maskCb, &cache);
    gnss.setSupportedMask(core::services::kGnssDefaultSupportedMask & ~kGsaBit);

    CommandRouter router;
    const std::array<RouteTable, 1> tables = {gnss.table()};
    TEST_ASSERT_TRUE(fw::isOk(router.init(tables)));

    Reply reply{};
    // 开机订阅：GSA 不支持 -> 02 02，掩码保持 0。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(
            Role::kPhone, makeReq(kMsgGsa, proto::msg_type::kQuery, 1), kTimeBase, reply)));
    assertReplyResult(reply, kMsgGsa, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.activeMask());

    // 关闭分支也一样被拒（不支持的类型不存在可关的订阅）。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, makeReq(kMsgGsa, proto::msg_type::kQuery, 0),
                                kTimeBase + kStepMs, reply)));
    assertReplyResult(reply, kMsgGsa, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.activeMask());

    // 支持的 GGA 正常订阅。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(Role::kPhone, makeReq(kMsgGga, proto::msg_type::kSet, 1),
                                           kTimeBase + (2U * kStepMs), reply)));
    assertReplyResult(reply, kMsgGga, proto::msg_type::kSetRes, kResultOk);
    TEST_ASSERT_EQUAL_HEX32(1U, gnss.activeMask());

    // 接收机能力变化只更新白名单，不取消已有订阅。
    gnss.setSupportedMask(core::services::kGnssDefaultSupportedMask & ~(1U << 0U));
    TEST_ASSERT_EQUAL_HEX32(1U, gnss.activeMask());
    TEST_ASSERT_EQUAL_UINT32(1000U, gnss.periodMs(0U));
}

void testIntervalZeroStopsAndQueriesLatest()
{
    constexpr uint32_t kTimeBase = 100;
    constexpr uint32_t kStepMs = 10;

    GnssService gnss;
    MockCache cache{};
    cache.sentences[0] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    cache.oversize[1] = true; // SATSINFOA > 256B
    gnss.setQueryHook(&queryCb, &cache);
    gnss.setMaskHook(&maskCb, &cache);

    CommandRouter router;
    const std::array<RouteTable, 1> tables = {gnss.table()};
    TEST_ASSERT_TRUE(fw::isOk(router.init(tables)));

    Reply reply{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(
            Role::kPhone, makeReq(kMsgGga, proto::msg_type::kQuery, 1), kTimeBase, reply)));
    TEST_ASSERT_EQUAL_HEX32(1U << 0U, gnss.activeMask());

    // interval=0：先关闭 0x04 订阅，再返回 <=256B 最新原句。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, makeReq(kMsgGga, proto::msg_type::kQuery, 0),
                                kTimeBase + kStepMs, reply)));
    TEST_ASSERT_EQUAL_HEX32(0U, gnss.activeMask());
    assertReplyPayload(reply, kMsgGga, proto::msg_type::kQueryRes, cache.sentences[0]);

    // 0x21(GST) 尚无缓存 -> 返回 02 02。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, makeReq(kMsgGst, proto::msg_type::kQuery, 0),
                                kTimeBase + (2U * kStepMs), reply)));
    assertReplyResult(reply, kMsgGst, proto::msg_type::kQueryRes, kResultFail);

    // 0x05(SATSINFOA) > 256B -> 返回 02 02 并递增 queryOversize。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(
                                Role::kPhone, makeReq(kMsgSat, proto::msg_type::kQuery, 0),
                                kTimeBase + (3U * kStepMs), reply)));
    assertReplyResult(reply, kMsgSat, proto::msg_type::kQueryRes, kResultFail);
    TEST_ASSERT_EQUAL_UINT32(1U, gnss.queryOversizeCount());
}

} // namespace

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testIntervalConversion);
    RUN_TEST(testRouterIntegrationWithAllServices);
    RUN_TEST(testSubscriptionAndCapabilityGate);
    RUN_TEST(testUnsupportedTypeGate);
    RUN_TEST(testIntervalZeroStopsAndQueriesLatest);
    return UNITY_END();
}
