// CommandRouter 单元测试（ADR-0007 §3.5、任务卡 J2a）：init 的错误码、查找顺序、handle 先 clear()。
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"
#include "proto/Frame.hpp"
#include "unity.h"

namespace {

using core::CommandRouter;
using core::DispatchResult;
using core::kMaxRoutes;
using core::kRoleAlarm;
using core::kRolePhone;
using core::Reply;
using core::Role;
using core::role_bit;
using core::Route;
using core::RouteTable;

// Reply 哨兵值：确认 handle 先 clear()（长度与内容都必须被清掉）。
constexpr uint8_t kSentinelByte = 0x5A;
constexpr size_t kSentinelLen = 99;

// 测试用 msg_id（0x0041 为报警器心跳，0x0001 为设备信息）。
constexpr uint16_t kMsgHeartbeat = proto::alarm::kMsgHeartbeat;
constexpr uint16_t kMsgDevInfo = 0x0001;

// 文件作用域可变静态统一加 s_ 前缀（AGENTS.md §2）。
uint32_t s_calls = 0;
uint32_t s_last_now = 0;
uint16_t s_last_msg_id = 0;
size_t s_last_reply_len = 0;
void *s_last_self = nullptr;

DispatchResult handle_reply(void *self, const proto::FrameView &frame, uint32_t now_ms,
                            Reply &reply) noexcept
{
    ++s_calls;
    s_last_self = self;
    s_last_now = now_ms;
    s_last_msg_id = frame.msg_id;
    s_last_reply_len = reply.len; // handle 必须先 clear()，所以这里应恒为 0
    reply.len = 1;
    return DispatchResult::kReply;
}

DispatchResult handle_silent(void *, const proto::FrameView &frame, uint32_t now_ms,
                             Reply &reply) noexcept
{
    static_cast<void>(frame);
    static_cast<void>(now_ms);
    static_cast<void>(reply);
    return DispatchResult::kNoReply;
}

/** 一组单路由表：msg_id 0x0041，仅 ALARM。 */
struct Fixture {
    Reply reply{};
    proto::FrameView frame{};

    Fixture() noexcept
    {
        reply.buf.fill(kSentinelByte);
        reply.len = kSentinelLen;
        reply.whitelist_changed = true;
        reply.heartbeat = proto::alarm::Heartbeat{};
        frame.msg_id = kMsgHeartbeat;
    }
};

void test_role_bit_constants()
{
    TEST_ASSERT_EQUAL_UINT8(0x02, kRolePhone);
    TEST_ASSERT_EQUAL_UINT8(0x04, kRoleAlarm);
    TEST_ASSERT_EQUAL_UINT8(0x01, role_bit(Role::kNone));
    TEST_ASSERT_EQUAL_UINT8(0x08, role_bit(Role::kPending));
    TEST_ASSERT_EQUAL_UINT(48, kMaxRoutes);
}

void test_init_and_route_count()
{
    const std::array<Route, 2> routes = {
        Route{proto::alarm::kMsgStatus, kRoleAlarm, &handle_reply},
        Route{kMsgDevInfo, kRolePhone, &handle_reply},
    };
    const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, routes}};
    CommandRouter router;
    TEST_ASSERT_EQUAL_UINT8(0, router.route_count());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kOk),
                            static_cast<uint8_t>(router.init(tables)));
    TEST_ASSERT_EQUAL_UINT(2, router.route_count());
}

void test_init_rejects_bad_tables()
{
    const std::array<Route, 1> routes = {Route{kMsgHeartbeat, kRoleAlarm, &handle_reply}};
    const std::array<Route, 1> null_handler = {Route{kMsgHeartbeat, kRoleAlarm, nullptr}};
    const std::array<Route, 1> zero_roles = {Route{kMsgHeartbeat, 0, &handle_reply}};
    const std::array<Route, 2> duplicated = {
        Route{kMsgHeartbeat, kRoleAlarm, &handle_reply},
        Route{kMsgHeartbeat, kRolePhone, &handle_silent},
    };

    {
        CommandRouter router;
        const std::array<RouteTable, 1> tables = {RouteTable{nullptr, routes}};
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kInvalidArg),
                                static_cast<uint8_t>(router.init(tables)));
        TEST_ASSERT_EQUAL_UINT(0, router.route_count());
    }
    {
        CommandRouter router;
        const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, null_handler}};
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kInvalidArg),
                                static_cast<uint8_t>(router.init(tables)));
        TEST_ASSERT_EQUAL_UINT(0, router.route_count());
    }
    {
        CommandRouter router;
        const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, zero_roles}};
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kInvalidArg),
                                static_cast<uint8_t>(router.init(tables)));
    }
    {
        CommandRouter router;
        const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, duplicated}};
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kInvalidArg),
                                static_cast<uint8_t>(router.init(tables)));
        TEST_ASSERT_EQUAL_UINT(0, router.route_count());
    }
}

void test_failed_init_leaves_router_unusable()
{
    // 先成功登记一条，再让 init 失败：路由器必须保持未初始化（handle 一律 kNoReply）。
    std::array<Route, kMaxRoutes + 1> too_many{};
    for (size_t i = 0; i < too_many.size(); ++i) {
        // i ≤ 48，uint8_t/uint16_t 截断安全。
        too_many[i] = Route{static_cast<uint16_t>(i), kRolePhone, &handle_reply};
    }
    const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, too_many}};

    CommandRouter router;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kNoSpace),
                            static_cast<uint8_t>(router.init(tables)));
    TEST_ASSERT_EQUAL_UINT(0, router.route_count());

    Fixture f;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kNoReply),
                            static_cast<uint8_t>(router.handle(Role::kAlarm, f.frame, 0, f.reply)));
    TEST_ASSERT_EQUAL_UINT(0, s_calls);
    TEST_ASSERT_EQUAL_UINT(0, f.reply.len); // 未 init 也必须先 clear()
    TEST_ASSERT_FALSE(f.reply.whitelist_changed);
    TEST_ASSERT_FALSE(f.reply.heartbeat.has_value());
}

void test_capacity_limits()
{
    std::array<Route, kMaxRoutes> exact{};
    for (size_t i = 0; i < exact.size(); ++i) {
        exact[i] = Route{static_cast<uint16_t>(i), kRolePhone, &handle_reply};
    }
    const std::array<RouteTable, 1> ok_tables = {RouteTable{&s_calls, exact}};
    CommandRouter full;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kOk),
                            static_cast<uint8_t>(full.init(ok_tables)));
    TEST_ASSERT_EQUAL_UINT(kMaxRoutes, full.route_count());

    std::array<Route, kMaxRoutes + 1> overflow{};
    for (size_t i = 0; i < overflow.size(); ++i) {
        overflow[i] = Route{static_cast<uint16_t>(i), kRolePhone, &handle_reply};
    }
    const std::array<RouteTable, 1> bad_tables = {RouteTable{&s_calls, overflow}};
    CommandRouter over;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kNoSpace),
                            static_cast<uint8_t>(over.init(bad_tables)));
}

void test_duplicate_init_returns_busy()
{
    const std::array<Route, 1> routes = {Route{kMsgHeartbeat, kRoleAlarm, &handle_reply}};
    const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, routes}};
    CommandRouter router;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kOk),
                            static_cast<uint8_t>(router.init(tables)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kBusy),
                            static_cast<uint8_t>(router.init(tables)));
    TEST_ASSERT_EQUAL_UINT(1, router.route_count());

    // 第一次登记仍然有效。
    Fixture f;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kReply),
                            static_cast<uint8_t>(router.handle(Role::kAlarm, f.frame, 7, f.reply)));
    TEST_ASSERT_EQUAL_UINT(1, s_calls);
}

void test_dispatch_and_arguments()
{
    constexpr uint32_t kNowMs = 0x1234;
    const std::array<Route, 1> routes = {Route{kMsgHeartbeat, kRoleAlarm, &handle_reply}};
    const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, routes}};
    CommandRouter router;
    TEST_ASSERT_TRUE(is_ok(router.init(tables)));

    Fixture f;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kReply),
        static_cast<uint8_t>(router.handle(Role::kAlarm, f.frame, kNowMs, f.reply)));
    TEST_ASSERT_EQUAL_UINT(1, s_calls);
    TEST_ASSERT_EQUAL_HEX32(kNowMs, s_last_now);
    TEST_ASSERT_EQUAL_HEX16(kMsgHeartbeat, s_last_msg_id);
    TEST_ASSERT_EQUAL_PTR(&s_calls, s_last_self);
    TEST_ASSERT_EQUAL_UINT(0, s_last_reply_len); // 进入 handler 前已 clear()
    TEST_ASSERT_EQUAL_UINT(1, f.reply.len);
    TEST_ASSERT_FALSE(f.reply.whitelist_changed);
    TEST_ASSERT_FALSE(f.reply.heartbeat.has_value());
}

void test_role_check_and_unknown_order()
{
    const std::array<Route, 1> phone_route = {Route{kMsgDevInfo, kRolePhone, &handle_reply}};
    const std::array<Route, 1> alarm_route = {Route{kMsgHeartbeat, kRoleAlarm, &handle_silent}};
    const std::array<RouteTable, 2> tables = {
        RouteTable{&s_calls, phone_route},
        RouteTable{&s_calls, alarm_route},
    };
    CommandRouter router;
    TEST_ASSERT_TRUE(is_ok(router.init(tables)));
    TEST_ASSERT_EQUAL_UINT(2, router.route_count());

    Fixture f;
    // 角色不符 → kDenied，handler 不被调用（0x41 路由只登记 ALARM）。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kDenied),
                            static_cast<uint8_t>(router.handle(Role::kPhone, f.frame, 0, f.reply)));
    TEST_ASSERT_EQUAL_UINT(0, s_calls);

    f.frame.msg_id = kMsgDevInfo;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kDenied),
                            static_cast<uint8_t>(router.handle(Role::kNone, f.frame, 0, f.reply)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(DispatchResult::kDenied),
        static_cast<uint8_t>(router.handle(Role::kPending, f.frame, 0, f.reply)));
    TEST_ASSERT_EQUAL_UINT(0, s_calls);

    // 未登记 ID 先于角色检查：任何角色都 kNoReply。
    // 未登记 ID：0x0000、5.4 保留 ID 0x1E、未分配 0x99/0xFF。
    const std::array<uint16_t, 4> unknown_ids = {0x0000, 0x001E, 0x0099, 0x00FF};
    for (const uint16_t id : unknown_ids) {
        f.frame.msg_id = id;
        for (Role role : {Role::kNone, Role::kPhone, Role::kAlarm, Role::kPending}) {
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kNoReply),
                                    static_cast<uint8_t>(router.handle(role, f.frame, 0, f.reply)));
        }
    }
    TEST_ASSERT_EQUAL_UINT(0, s_calls);
}

void test_handler_result_is_passed_through()
{
    const std::array<Route, 1> routes = {Route{kMsgHeartbeat, kRoleAlarm, &handle_silent}};
    const std::array<RouteTable, 1> tables = {RouteTable{&s_calls, routes}};
    CommandRouter router;
    TEST_ASSERT_TRUE(is_ok(router.init(tables)));

    Fixture f;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DispatchResult::kNoReply),
                            static_cast<uint8_t>(router.handle(Role::kAlarm, f.frame, 0, f.reply)));
    TEST_ASSERT_EQUAL_UINT(0, f.reply.len);
}

} // namespace

// Unity 默认运行器会在每个用例前后调用这两个符号（外部链接，见 test/host 同样的写法）。
extern "C" void setUp(void)
{
    s_calls = 0;
    s_last_now = 0;
    s_last_msg_id = 0;
    s_last_reply_len = kSentinelLen;
    s_last_self = nullptr;
}

extern "C" void tearDown(void) {}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_role_bit_constants);
    RUN_TEST(test_init_and_route_count);
    RUN_TEST(test_init_rejects_bad_tables);
    RUN_TEST(test_failed_init_leaves_router_unusable);
    RUN_TEST(test_capacity_limits);
    RUN_TEST(test_duplicate_init_returns_busy);
    RUN_TEST(test_dispatch_and_arguments);
    RUN_TEST(test_role_check_and_unknown_order);
    RUN_TEST(test_handler_result_is_passed_through);
    return UNITY_END();
}
