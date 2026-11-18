// 路由副作用须在reply信用取得后恰好执行一次，满窗口与CLOSE不能重放。
// 单线程host测试；纯组件，不访问硬件、不分配堆。
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/LinkService.hpp"
#include "unity.h"

namespace {
constexpr core::SessionHandle kHandle = 1;
constexpr core::SessionHandle kSecondHandle = 2;
constexpr uint16_t kCommand = 1;

class CreditTransport final : public core::Transport {
public:
    // 测试替身只在本测试线程使用，非ISR。
    fw::Status write(core::SessionHandle, std::span<const uint8_t>, uint32_t) noexcept override
    {
        ++legacyCalls;
        return fw::Status::kNotSupported;
    }
    fw::Status disconnect(core::SessionHandle handle) noexcept override
    {
        ++disconnectCalls;
        lastDisconnected = handle;
        return fw::Status::kOk;
    }
    bool supportsCredit() const noexcept override
    {
        return creditEnabled;
    }
    fw::Status reserveReply(core::SessionHandle handle, core::ReplyCredit &credit) noexcept override
    {
        ++reserves;
        if (reserveError != fw::Status::kOk) {
            return reserveError;
        }
        if (!replyReady(handle)) {
            return fw::Status::kTimeout;
        }
        reserved = true;
        credit.capacity = proto::kMaxFrame;
        return fw::Status::kOk;
    }
    fw::Status commitReply(const core::ReplyCredit &credit,
                           std::span<const uint8_t> data) noexcept override
    {
        if (!reserved || (credit.capacity < data.size())) {
            return fw::Status::kInvalidState;
        }
        reserved = false;
        ++commits;
        return commitStatus;
    }
    void cancelReply(const core::ReplyCredit &) noexcept override
    {
        reserved = false;
        ++cancels;
    }
    bool replyReady(core::SessionHandle handle) const noexcept override
    {
        return (handle == kSecondHandle) ? readySecond : ready;
    }

    bool creditEnabled = true;
    bool ready = false;
    bool readySecond = false;
    bool reserved = false;
    uint32_t reserves = 0;
    uint32_t commits = 0;
    uint32_t cancels = 0;
    uint32_t legacyCalls = 0;
    uint32_t disconnectCalls = 0;
    core::SessionHandle lastDisconnected = 0;
    fw::Status reserveError = fw::Status::kOk;
    fw::Status commitStatus = fw::Status::kOk;
};

class LegacyTransport final : public core::Transport {
public:
    fw::Status write(core::SessionHandle handle, std::span<const uint8_t> data,
                     uint32_t) noexcept override
    {
        ++writes;
        lastHandle = handle;
        lastLen = data.size();
        return fw::Status::kOk;
    }
    fw::Status disconnect(core::SessionHandle) noexcept override
    {
        return fw::Status::kOk;
    }
    uint32_t writes = 0;
    core::SessionHandle lastHandle = 0;
    size_t lastLen = 0;
};

struct Counter {
    uint32_t effects = 0;
    bool reply = true;
    static core::DispatchResult handle(void *self, const proto::FrameView &, uint32_t,
                                       core::Reply &reply) noexcept
    {
        auto &counter = *static_cast<Counter *>(self); // self始终来自本对象的RouteTable，无窄化。
        ++counter.effects; // 用有副作用的handler直接检验不重放，而非只计算query回复数。
        reply.len = 1;
        return counter.reply ? core::DispatchResult::kReply : core::DispatchResult::kNoReply;
    }
};

void testCreditBeforeEffectAndCancel()
{
    CreditTransport transport;
    core::LinkManager manager;
    manager.init(core::Policy::kConcurrent, {});
    core::AlarmSource source;
    const core::DeviceInfo info{};
    const core::services::AlarmService alarm{source, manager, info};
    Counter counter;
    const std::array routes{core::Route{kCommand, core::kRolePhone, &Counter::handle}};
    const std::array tables{core::RouteTable{&counter, routes}};
    core::CommandRouter router;
    const auto initialized = router.init(tables);
    TEST_ASSERT_TRUE(initialized == fw::Status::kOk);
    core::LinkService service{transport, manager, router, alarm};
    service.onOpen(kHandle, {}, 0);

    std::array<uint8_t, proto::kMaxFrame> frame{};
    size_t size = 0;
    const auto built =
        proto::buildFrame(frame, kCommand, proto::sender::kApp, proto::msg_type::kSet, {}, size);
    TEST_ASSERT_TRUE(built == fw::Status::kOk);
    const auto wire = std::span<const uint8_t>{frame}.first(size);
    const size_t consumed = service.onData(kHandle, wire, 0);
    TEST_ASSERT_EQUAL(size, consumed);
    TEST_ASSERT_TRUE(service.paused(kHandle));
    service.resume(kHandle, 1);
    const size_t tail = service.onData(kHandle, wire, 1);
    TEST_ASSERT_EQUAL(0, tail);
    TEST_ASSERT_EQUAL(0, counter.effects);

    transport.ready = true;
    const uint32_t before = transport.reserves;
    service.resume(kHandle, 2);
    TEST_ASSERT_FALSE(service.paused(kHandle));
    TEST_ASSERT_EQUAL(1, counter.effects);
    TEST_ASSERT_EQUAL(1, transport.commits);
    TEST_ASSERT_EQUAL(before + 1U, transport.reserves); // commit不会二次reserve。
    service.resume(kHandle, 2);
    TEST_ASSERT_EQUAL(1, counter.effects);

    // 无回复命令归还信用；底层commit失败也不能重新执行副作用。
    counter.reply = false;
    const size_t noReply = service.onData(kHandle, wire, 2);
    TEST_ASSERT_EQUAL(size, noReply);
    TEST_ASSERT_EQUAL(2, counter.effects);
    TEST_ASSERT_EQUAL(1, transport.cancels);
    TEST_ASSERT_FALSE(transport.reserved);
    counter.reply = true;
    transport.commitStatus = fw::Status::kIoError;
    const size_t failed = service.onData(kHandle, wire, 2);
    TEST_ASSERT_EQUAL(size, failed);
    service.resume(kHandle, 2);
    TEST_ASSERT_EQUAL(3, counter.effects);
    TEST_ASSERT_EQUAL(2, transport.commits);
    TEST_ASSERT_EQUAL(1, service.stats().writeErrors);

    // CLOSE后不执行等待中的完整帧，重开同句柄也不会携带旧pending。
    transport.ready = false;
    const size_t pending = service.onData(kHandle, wire, 2);
    TEST_ASSERT_EQUAL(size, pending);
    service.onClose(kHandle);
    transport.ready = true;
    service.onOpen(kHandle, {}, 2);
    service.resume(kHandle, 2);
    TEST_ASSERT_EQUAL(3, counter.effects);
    TEST_ASSERT_EQUAL(1, service.stats().dispatchCancelled);
    TEST_ASSERT_EQUAL(0, transport.legacyCalls);
}

void testRoleRejectCancelsCreditAndNoLeak()
{
    constexpr uint32_t kTickOpenPhone = 2;
    constexpr uint32_t kTickPhoneFrame = 3;
    CreditTransport transport;
    transport.ready = true;
    transport.readySecond = true;
    constexpr core::BtAddr kAlarmAddr{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};
    constexpr core::BtAddr kPhoneAddr{{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
    const std::array whitelist{kAlarmAddr};
    core::LinkManager manager;
    manager.init(core::Policy::kConcurrent, whitelist);
    core::AlarmSource source;
    const core::DeviceInfo info{};
    const core::services::AlarmService alarm{source, manager, info};
    Counter counter;
    const std::array routes{core::Route{kCommand, core::kRolePhone, &Counter::handle}};
    const std::array tables{core::RouteTable{&counter, routes}};
    core::CommandRouter router;
    TEST_ASSERT_TRUE(router.init(tables) == fw::Status::kOk);
    core::LinkService service{transport, manager, router, alarm};
    service.onOpen(kHandle, kAlarmAddr, 0);

    std::array<uint8_t, proto::kMaxFrame> frame{};
    size_t size = 0;
    TEST_ASSERT_TRUE(proto::buildFrame(frame, kCommand, proto::sender::kAlarm,
                                       proto::msg_type::kQuery, {}, size) == fw::Status::kOk);
    const auto wire = std::span<const uint8_t>{frame}.first(size);
    TEST_ASSERT_EQUAL(size, service.onData(kHandle, wire, 1));
    TEST_ASSERT_EQUAL(0, counter.effects);
    TEST_ASSERT_EQUAL(0, transport.commits);
    TEST_ASSERT_EQUAL(1, transport.cancels);
    TEST_ASSERT_FALSE(transport.reserved);

    service.onOpen(kSecondHandle, kPhoneAddr, kTickOpenPhone);
    TEST_ASSERT_TRUE(proto::buildFrame(frame, kCommand, proto::sender::kApp, proto::msg_type::kSet,
                                       {}, size) == fw::Status::kOk);
    TEST_ASSERT_EQUAL(size,
                      service.onData(kSecondHandle, std::span<const uint8_t>{frame}.first(size),
                                     kTickPhoneFrame));
    TEST_ASSERT_EQUAL(1, counter.effects);
    TEST_ASSERT_EQUAL(1, transport.commits);
}

void testTwoSessionsIsolatedPauseAndFairResume()
{
    constexpr uint32_t kTickSecond = 2;
    constexpr uint32_t kTickResume = 3;
    constexpr core::BtAddr kPhoneAddr{{1, 0, 0, 0, 0, 1}};
    constexpr core::BtAddr kAlarmAddr{{2, 0, 0, 0, 0, 2}};
    const std::array whitelist{kAlarmAddr};
    CreditTransport transport;
    transport.ready = false;
    transport.readySecond = true;
    core::LinkManager manager;
    manager.init(core::Policy::kConcurrent, whitelist);
    core::AlarmSource source;
    const core::DeviceInfo info{};
    const core::services::AlarmService alarm{source, manager, info};
    Counter counter;
    const std::array routes{
        core::Route{kCommand, static_cast<core::RoleMask>(core::kRoleAlarm | core::kRolePhone),
                    &Counter::handle}};
    const std::array tables{core::RouteTable{&counter, routes}};
    core::CommandRouter router;
    TEST_ASSERT_TRUE(router.init(tables) == fw::Status::kOk);
    core::LinkService service{transport, manager, router, alarm};
    service.onOpen(kHandle, kPhoneAddr, 0);
    service.onOpen(kSecondHandle, kAlarmAddr, 0);

    std::array<uint8_t, proto::kMaxFrame> phoneFrame{};
    size_t phoneSize = 0;
    TEST_ASSERT_TRUE(proto::buildFrame(phoneFrame, kCommand, proto::sender::kApp,
                                       proto::msg_type::kSet, {}, phoneSize) == fw::Status::kOk);
    TEST_ASSERT_EQUAL(
        phoneSize,
        service.onData(kHandle, std::span<const uint8_t>{phoneFrame}.first(phoneSize), 1));
    TEST_ASSERT_TRUE(service.paused(kHandle));
    TEST_ASSERT_EQUAL(0, counter.effects);

    std::array<uint8_t, proto::kMaxFrame> alarmFrame{};
    size_t alarmSize = 0;
    TEST_ASSERT_TRUE(proto::buildFrame(alarmFrame, kCommand, proto::sender::kAlarm,
                                       proto::msg_type::kSet, {}, alarmSize) == fw::Status::kOk);
    TEST_ASSERT_EQUAL(alarmSize,
                      service.onData(kSecondHandle,
                                     std::span<const uint8_t>{alarmFrame}.first(alarmSize),
                                     kTickSecond));
    TEST_ASSERT_FALSE(service.paused(kSecondHandle));
    TEST_ASSERT_TRUE(service.paused(kHandle));
    TEST_ASSERT_EQUAL(1, counter.effects);
    TEST_ASSERT_EQUAL(1, transport.commits);

    transport.ready = true;
    service.resume(kHandle, kTickResume);
    TEST_ASSERT_FALSE(service.paused(kHandle));
    TEST_ASSERT_EQUAL(2, counter.effects);
    TEST_ASSERT_EQUAL(2, transport.commits);
}

void testReserveFatalErrorDisconnectsSession()
{
    CreditTransport transport;
    transport.reserveError = fw::Status::kIoError;
    core::LinkManager manager;
    manager.init(core::Policy::kConcurrent, {});
    core::AlarmSource source;
    const core::DeviceInfo info{};
    const core::services::AlarmService alarm{source, manager, info};
    Counter counter;
    const std::array routes{core::Route{kCommand, core::kRolePhone, &Counter::handle}};
    const std::array tables{core::RouteTable{&counter, routes}};
    core::CommandRouter router;
    TEST_ASSERT_TRUE(router.init(tables) == fw::Status::kOk);
    core::LinkService service{transport, manager, router, alarm};
    service.onOpen(kHandle, {}, 0);

    std::array<uint8_t, proto::kMaxFrame> frame{};
    size_t size = 0;
    TEST_ASSERT_TRUE(proto::buildFrame(frame, kCommand, proto::sender::kApp, proto::msg_type::kSet,
                                       {}, size) == fw::Status::kOk);
    TEST_ASSERT_EQUAL(size,
                      service.onData(kHandle, std::span<const uint8_t>{frame}.first(size), 1));
    TEST_ASSERT_FALSE(service.paused(kHandle));
    TEST_ASSERT_EQUAL(0, counter.effects);
    TEST_ASSERT_EQUAL(1, service.stats().writeErrors);
    TEST_ASSERT_EQUAL(1, service.stats().disconnects);
    TEST_ASSERT_EQUAL(1, transport.disconnectCalls);
    TEST_ASSERT_EQUAL(kHandle, transport.lastDisconnected);
}

void testLegacyTransportCompatWithoutCredit()
{
    LegacyTransport transport;
    core::LinkManager manager;
    manager.init(core::Policy::kConcurrent, {});
    core::AlarmSource source;
    const core::DeviceInfo info{};
    const core::services::AlarmService alarm{source, manager, info};
    Counter counter;
    const std::array routes{core::Route{kCommand, core::kRolePhone, &Counter::handle}};
    const std::array tables{core::RouteTable{&counter, routes}};
    core::CommandRouter router;
    TEST_ASSERT_TRUE(router.init(tables) == fw::Status::kOk);
    core::LinkService service{transport, manager, router, alarm};
    service.onOpen(kHandle, {}, 0);

    std::array<uint8_t, proto::kMaxFrame> frame{};
    size_t size = 0;
    TEST_ASSERT_TRUE(proto::buildFrame(frame, kCommand, proto::sender::kApp, proto::msg_type::kSet,
                                       {}, size) == fw::Status::kOk);
    TEST_ASSERT_EQUAL(size,
                      service.onData(kHandle, std::span<const uint8_t>{frame}.first(size), 1));
    TEST_ASSERT_EQUAL(1, counter.effects);
    TEST_ASSERT_EQUAL(1, transport.writes);
    TEST_ASSERT_EQUAL(kHandle, transport.lastHandle);
    TEST_ASSERT_TRUE(transport.lastLen > 0U);
}
} // namespace

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}
int main()
{
    UNITY_BEGIN();
    RUN_TEST(testCreditBeforeEffectAndCancel);
    RUN_TEST(testRoleRejectCancelsCreditAndNoLeak);
    RUN_TEST(testTwoSessionsIsolatedPauseAndFairResume);
    RUN_TEST(testReserveFatalErrorDisconnectsSession);
    RUN_TEST(testLegacyTransportCompatWithoutCredit);
    return UNITY_END();
}
