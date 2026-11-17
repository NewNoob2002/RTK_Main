// AlarmService::fix_quality 透传测试（ADR-0010 §2.8）：验证 0x40 负载第 4 字节随 DeviceInfo::fix_quality
// 变化，且默认值为 0xFF（kValueUnknown）。
#include <array>
#include <cstdint>

#include "core/AlarmSource.hpp"
#include "core/CommandRouter.hpp"
#include "core/LinkManager.hpp"
#include "core/Reply.hpp"
#include "core/services/AlarmService.hpp"
#include "core/services/DeviceService.hpp"
#include "proto/AlarmCodec.hpp"
#include "proto/Frame.hpp"
#include "unity.h"

namespace {

constexpr size_t kFixQualityPayloadOffset = 4U;
constexpr uint32_t kNowMs = 1000U;
constexpr uint16_t kPushSeq = 0x002AU;

bool parse_wire(proto::FrameParser &parser, std::span<const uint8_t> wire,
                proto::FrameView &out) noexcept
{
    parser.reset();
    for (const uint8_t byte : wire) {
        const auto res = parser.feed(byte, out);
        if (res == proto::ParseResult::kFrame) {
            return true;
        }
        if (res != proto::ParseResult::kNone) {
            return false;
        }
    }
    return false;
}

void test_fix_quality_default_and_updates()
{
    core::AlarmSource alarm;
    core::LinkManager link;
    link.init(core::Policy::kConcurrent, {});
    core::services::DeviceService dev;
    core::services::AlarmService alarm_svc(alarm, link, dev.info());

    core::CommandRouter router;
    const std::array<core::RouteTable, 1U> tables = {alarm_svc.table()};
    TEST_ASSERT_TRUE(isOk(router.init(tables)));

    // 默认 DeviceInfo::fix_quality 为 0xFF，0x40 推送与查询的第 4 字节均为 0xFF。
    TEST_ASSERT_EQUAL_UINT8(proto::alarm::kValueUnknown, dev.info().fix_quality);

    core::Reply reply;
    proto::FrameParser parser;
    proto::FrameView parsed{};
    TEST_ASSERT_TRUE(alarm_svc.build_status_push(kPushSeq, kNowMs, reply));
    TEST_ASSERT_TRUE(parse_wire(parser, reply.bytes(), parsed));
    TEST_ASSERT_EQUAL_UINT(proto::alarm::kStatusLen, parsed.payload.size());
    TEST_ASSERT_EQUAL_UINT8(proto::alarm::kValueUnknown, parsed.payload[kFixQualityPayloadOffset]);

    // 更新为 RTK Fixed (4) 后，推送和 0x40 QUERY 应答第 4 字节均为 4。
    dev.info().fix_quality = 4U;
    TEST_ASSERT_TRUE(alarm_svc.build_status_push(kPushSeq + 1U, kNowMs + 100U, reply));
    TEST_ASSERT_TRUE(parse_wire(parser, reply.bytes(), parsed));
    TEST_ASSERT_EQUAL_UINT8(4U, parsed.payload[kFixQualityPayloadOffset]);

    proto::FrameView query{};
    query.msg_id = proto::alarm::kMsgStatus;
    query.type = proto::msg_type::kQuery;
    query.sender = proto::sender::kApp;
    reply.clear();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(core::Role::kAlarm, query, kNowMs + 200U, reply)));
    TEST_ASSERT_TRUE(parse_wire(parser, reply.bytes(), parsed));
    TEST_ASSERT_EQUAL_UINT(proto::alarm::kStatusLen, parsed.payload.size());
    TEST_ASSERT_EQUAL_UINT8(4U, parsed.payload[kFixQualityPayloadOffset]);

    // 超时回退为 0xFF。
    dev.info().fix_quality = proto::alarm::kValueUnknown;
    reply.clear();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(core::Role::kAlarm, query, kNowMs + 300U, reply)));
    TEST_ASSERT_TRUE(parse_wire(parser, reply.bytes(), parsed));
    TEST_ASSERT_EQUAL_UINT8(proto::alarm::kValueUnknown, parsed.payload[kFixQualityPayloadOffset]);
}

} // namespace

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_fix_quality_default_and_updates);
    return UNITY_END();
}
