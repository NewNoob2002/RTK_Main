// 追踪事件点测试（ADR-0009）：安装记录型 sink，驱动一次 open/HELLO/close，断言事件序列。
// 单线程、非 ISR；sink 只在 fw::trace 调用点同步写入，测试自己读 s_events。

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/LinkManager.hpp"
#include "core/TraceCodes.hpp"
#include "fw/Trace.hpp"
#include "unity.h"

namespace {

using core::Actions;
using core::BtAddr;
using core::LinkManager;
using core::LinkReason;
using core::LinkTrace;
using core::Policy;
using core::Role;
using core::SessionHandle;

constexpr SessionHandle kPhoneHandle = 0x0081U;
constexpr SessionHandle kAlarmHandle = 0x0082U;
constexpr uint32_t kNowMs = 1234U;
constexpr size_t kMaxEvents = 8U;

/* 期望的事件序号：用枚举代替裸下标，避免魔法数字。 */
enum class Step : uint8_t {
    kOpenPhone = 0,
    kPairOpen = 1,
    kOpenPending = 2,
    kWhitelistAdd = 3,
    kRoleChange = 4,
    kClose = 5,
    kCount = 6,
};

// 文件作用域可变静态加 s_ 前缀（AGENTS.md §2）。
std::array<fw::TraceEvent, kMaxEvents> s_events{};
size_t s_count = 0U;

void record_sink(void * /*ctx*/, const fw::TraceEvent &event) noexcept
{
    if (s_count < s_events.size()) {
        s_events[s_count] = event;
        ++s_count;
    }
}

/* trace() 只在真正写事件时读时钟；固定值让时间戳可断言。 */
uint32_t fixed_clock() noexcept
{
    return kNowMs;
}

BtAddr make_addr(uint8_t last) noexcept
{
    BtAddr addr{};
    addr.bytes = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, last};
    return addr;
}

void assert_event(Step step, core::LinkTrace code, SessionHandle session, uint32_t a, uint32_t b)
{
    const auto index = static_cast<size_t>(step);
    TEST_ASSERT_LESS_THAN_UINT32(static_cast<uint32_t>(s_count), static_cast<uint32_t>(index));
    const fw::TraceEvent &event = s_events[index];
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::TraceCat::kLink), static_cast<uint8_t>(event.cat));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(code), event.code);
    TEST_ASSERT_EQUAL_UINT32(session, static_cast<uint32_t>(event.session));
    TEST_ASSERT_EQUAL_UINT32(a, event.a);
    TEST_ASSERT_EQUAL_UINT32(b, event.b);
    TEST_ASSERT_EQUAL_UINT32(kNowMs, event.t_ms);
}

} // namespace

/* open(PHONE) → pair_open → open(PENDING) → HELLO(变成 ALARM) → close 的完整事件序列。 */
static void test_open_hello_close_event_sequence()
{
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(fw::trace_install(&record_sink, nullptr, &fixed_clock)));

    LinkManager manager{};
    // 空白名单：init 会原样拷入 span 里的每一项，传空 span 才是“没有已授权报警器”。
    manager.init(Policy::kConcurrent, {});

    Actions actions{};
    // 第一部手机先占 PHONE 角色（无 PHONE 时未知设备即为 PHONE）。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Role::kPhone),
                            static_cast<uint8_t>(manager.on_open(kPhoneHandle, make_addr(0x01U), kNowMs, actions)));
    manager.pair_open(kNowMs);
    // 配对窗口内第二台设备等待 HELLO。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Role::kPending),
                            static_cast<uint8_t>(manager.on_open(kAlarmHandle, make_addr(0x02U), kNowMs, actions)));
    // HELLO 让 PENDING 变成 ALARM，并进入白名单。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(Role::kAlarm),
        static_cast<uint8_t>(manager.on_frame(kAlarmHandle, core::kHelloSender, core::kHelloMsgId, kNowMs, actions)));
    manager.on_close(kAlarmHandle);

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(Step::kCount), static_cast<uint32_t>(s_count));
    assert_event(Step::kOpenPhone, LinkTrace::kOpen, kPhoneHandle, static_cast<uint32_t>(Role::kPhone),
                 static_cast<uint32_t>(LinkReason::kFirstPhone));
    // 实现把窗口长度放在 a（b 未用），见 LinkManager.cpp 的 pair_open。
    assert_event(Step::kPairOpen, LinkTrace::kPairOpen, 0U, core::kPairWindowMs, 0U);
    assert_event(Step::kOpenPending, LinkTrace::kOpen, kAlarmHandle, static_cast<uint32_t>(Role::kPending),
                 static_cast<uint32_t>(LinkReason::kPendingHello));
    assert_event(Step::kWhitelistAdd, LinkTrace::kWhitelistAdd, kAlarmHandle, 1U, 0U);
    assert_event(Step::kRoleChange, LinkTrace::kRoleChange, kAlarmHandle, static_cast<uint32_t>(Role::kPending),
                 static_cast<uint32_t>(Role::kAlarm));
    assert_event(Step::kClose, LinkTrace::kClose, kAlarmHandle, static_cast<uint32_t>(Role::kAlarm), 0U);
}

/* Unity 在两个用例之间调用；本文件只有一个用例，清空记录即可。 */
extern "C" void setUp(void)
{
    s_count = 0U;
    s_events.fill(fw::TraceEvent{});
    // 运行时默认掩码为 0（ADR-0009 §2.7），本组用例要观察全部类别。
    fw::trace_set_mask(fw::kTraceAllMask);
}

extern "C" void tearDown(void) {}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_hello_close_event_sequence);
    return UNITY_END();
}
