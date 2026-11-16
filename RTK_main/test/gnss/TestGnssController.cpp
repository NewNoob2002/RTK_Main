// GNSS 控制器 host 测试：上电/探测/配置/运行/静默重探/失败退避/波特率切换/提示优先（ADR-0010 §2.6）。
// 用测试本地的假方言与假解码器驱动状态机，不依赖真实模块。
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "gnss/Controller.hpp"
#include "unity.h"

namespace {

using gnss::Action;
using gnss::CmdBuf;
using gnss::ConfigStep;
using gnss::Controller;
using gnss::Family;
using gnss::Model;
using gnss::ProbeResult;
using gnss::Reply;
using gnss::RoverConfig;

bool fake_first(uint8_t byte) noexcept
{
    return byte == '#';
}

demux::Step fake_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.empty()) {
        return demux::Step::kReject;
    }
    if (frame.back() == static_cast<uint8_t>('\n')) {
        return demux::Step::kFrame;
    }
    need = 1;
    return demux::Step::kNeedMore;
}

constexpr demux::Decoder kFakeDecoder{"fake", 32, &fake_first, &fake_step};

bool probe_a(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("VERSION\r\n");
}

bool probe_b(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("Q\r\n");
}

bool match_a(const demux::Decoder &, std::span<const uint8_t> frame, ProbeResult &out) noexcept
{
    if (frame.empty() || frame[0] != static_cast<uint8_t>('#')) {
        return false;
    }
    out.model = Model::kUm980;
    return true;
}

bool match_b(const demux::Decoder &, std::span<const uint8_t> frame, ProbeResult &out) noexcept
{
    if (frame.empty() || frame[0] != static_cast<uint8_t>('?')) {
        return false;
    }
    out.model = Model::kZedF9p;
    return true;
}

bool step_a(size_t index, Model, const RoverConfig &, ConfigStep &out) noexcept
{
    out = ConfigStep{};
    if (index == 0) {
        return out.cmd.append("A0\r\n");
    }
    if (index == 1) {
        return out.cmd.append("A1\r\n");
    }
    return false;
}

bool step_b(size_t index, Model, const RoverConfig &, ConfigStep &out) noexcept
{
    out = ConfigStep{};
    if (index == 0) {
        return out.cmd.append("B0\r\n");
    }
    return false;
}

bool step_switch(size_t index, Model, const RoverConfig &, ConfigStep &out) noexcept
{
    out = ConfigStep{};
    if (index == 0) {
        if (!out.cmd.append("S0\r\n")) {
            return false;
        }
        out.expect_reply = false;
        out.switch_baud = 57600;
        return true;
    }
    if (index == 1) {
        return out.cmd.append("S1\r\n");
    }
    return false;
}

Reply match_reply(const demux::Decoder &, std::span<const uint8_t> frame, std::span<const uint8_t>) noexcept
{
    if (frame.size() == 2 && frame[0] == static_cast<uint8_t>('O') && frame[1] == static_cast<uint8_t>('K')) {
        return Reply::kAck;
    }
    if (frame.size() == 2 && frame[0] == static_cast<uint8_t>('N') && frame[1] == static_cast<uint8_t>('O')) {
        return Reply::kNack;
    }
    return Reply::kNone;
}

uint32_t caps_none(Model) noexcept
{
    return 0;
}

constexpr gnss::Dialect kDialectA{"A", Family::kUnicore, {}, &probe_a, &match_a, &step_a, &match_reply, &caps_none};
constexpr gnss::Dialect kDialectB{"B", Family::kUblox, {}, &probe_b, &match_b, &step_b, &match_reply, &caps_none};
constexpr gnss::Dialect kDialectS{"S", Family::kComnav, {}, &probe_b, &match_b, &step_switch, &match_reply, &caps_none};

bool bytes_equal(std::span<const uint8_t> data, const char *text)
{
    size_t len = 0;
    while (text[len] != '\0') {
        if (len >= data.size() || data[len] != static_cast<uint8_t>(text[len])) {
            return false;
        }
        ++len;
    }
    return len == data.size();
}

bool is_power(const Action &action, bool on)
{
    return action.kind == Action::Kind::kPower && action.on == on;
}

bool is_write(const Action &action, const char *text)
{
    return action.kind == Action::Kind::kWrite && bytes_equal(action.bytes, text);
}

gnss::Timing fast_timing()
{
    gnss::Timing timing{};
    timing.power_off_ms = 10;
    timing.boot_ms = 20;
    timing.probe_timeout_ms = 50;
    timing.probe_rounds = 2;
    timing.step_retries = 1;
    timing.silence_ms = 100;
    timing.retry_backoff_ms = 30;
    return timing;
}

RoverConfig rover_config()
{
    RoverConfig cfg{};
    cfg.rate_hz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    return cfg;
}

void power_up(Controller &controller, uint32_t &now)
{
    controller.start(now);
    TEST_ASSERT_TRUE(is_power(controller.poll(now), false));
    now += 10;
    TEST_ASSERT_TRUE(is_power(controller.poll(now), true));
    now += 20;
}

void feed(Controller &controller, const char *frame, uint32_t now)
{
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(frame), std::string(frame).size());
    controller.on_frame(kFakeDecoder, bytes, now);
}

} // namespace

void setUp() {}
void tearDown() {}

static void test_init_rejects_bad_arguments()
{
    Controller controller;
    RoverConfig cfg{};
    gnss::Timing timing{};
    const std::array<const gnss::Dialect *, 1> dialects{&kDialectA};
    const std::array<uint32_t, 1> bauds{115200};
    const std::array<const gnss::Dialect *, 1> with_null{nullptr};
    const std::array<uint32_t, 1> zero{0};

    TEST_ASSERT_FALSE(controller.init({}, bauds, cfg, {}, timing));
    TEST_ASSERT_FALSE(controller.init(dialects, {}, cfg, {}, timing));
    TEST_ASSERT_FALSE(controller.init(with_null, bauds, cfg, {}, timing));
    TEST_ASSERT_FALSE(controller.init(dialects, zero, cfg, {}, timing));
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, {}, timing));
}

static void test_full_flow_probe_configure_run_reprobe()
{
    Controller controller;
    RoverConfig cfg = rover_config();
    const std::array<const gnss::Dialect *, 1> dialects{&kDialectA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fast_timing();
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, {}, timing));

    uint32_t now = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kOff), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone), static_cast<int>(controller.poll(now).kind));

    power_up(controller, now);
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "VERSION\r\n"));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kProbing), static_cast<int>(controller.state()));

    feed(controller, "#VERSIONA,1", now);
    ++now;
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "A0\r\n"));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kConfiguring), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(Model::kUm980), static_cast<int>(controller.detected().probe.model));
    TEST_ASSERT_EQUAL_UINT32(115200, controller.detected().baud);

    feed(controller, "OK", now);
    ++now;
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "A1\r\n"));

    feed(controller, "OK", now);
    ++now;
    const Action action = controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone), static_cast<int>(action.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().steps_sent);

    // 静默超时（>100 ms）→ 重新探测；任意帧刷新 last_frame。
    now += 50;
    feed(controller, "noise", now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone), static_cast<int>(controller.poll(now + 90).kind));
    now += 101;
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "VERSION\r\n"));
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().reprobes);

    controller.request_reprobe(now);
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().reprobes);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kWrite), static_cast<int>(controller.poll(now).kind));
}

static void test_probe_timeout_exhausts_rounds()
{
    Controller controller;
    RoverConfig cfg = rover_config();
    const std::array<const gnss::Dialect *, 1> dialects{&kDialectA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fast_timing();
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, {}, timing));

    uint32_t now = 0;
    power_up(controller, now);
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "VERSION\r\n"));

    now += 50; // 超时 → 第一轮结束，进入第二轮
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "VERSION\r\n"));
    now += 50; // 第二轮超时 → 失败
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone), static_cast<int>(controller.poll(now).kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kFailed), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().probes_sent);
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().failures);

    now += 29; // 退避未到
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone), static_cast<int>(controller.poll(now).kind));
    now += 1; // 退避到 → 重新上电
    TEST_ASSERT_TRUE(is_power(controller.poll(now), false));
}

static void test_nack_retries_then_fails()
{
    Controller controller;
    RoverConfig cfg = rover_config();
    const std::array<const gnss::Dialect *, 1> dialects{&kDialectA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fast_timing();
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, {}, timing));

    uint32_t now = 0;
    power_up(controller, now);
    (void)controller.poll(now);
    feed(controller, "#V", now);
    ++now;
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "A0\r\n"));

    feed(controller, "NO", now);
    ++now;
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "A0\r\n")); // 第一次重发
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().step_nacks);

    feed(controller, "NO", now);
    ++now;
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone), static_cast<int>(controller.poll(now).kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kFailed), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().step_nacks);
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().failures);
}

static void test_step_timeout_retries()
{
    Controller controller;
    RoverConfig cfg = rover_config();
    const std::array<const gnss::Dialect *, 1> dialects{&kDialectA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fast_timing();
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, {}, timing));

    uint32_t now = 0;
    power_up(controller, now);
    (void)controller.poll(now);
    feed(controller, "#V", now);
    ++now;
    (void)controller.poll(now);

    now += 500; // ConfigStep 默认 timeout_ms=500
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "A0\r\n"));
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().step_timeouts);
}

static void test_baud_switch_step()
{
    Controller controller;
    RoverConfig cfg = rover_config();
    const std::array<const gnss::Dialect *, 1> dialects{&kDialectS};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fast_timing();
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, {}, timing));

    uint32_t now = 0;
    power_up(controller, now);
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "Q\r\n"));
    feed(controller, "?", now);
    ++now;
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "S0\r\n"));

    const Action set_baud = controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kSetBaud), static_cast<int>(set_baud.kind));
    TEST_ASSERT_EQUAL_UINT32(57600, set_baud.baud);
    TEST_ASSERT_EQUAL_UINT32(57600, controller.host_baud());
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "S1\r\n"));

    feed(controller, "OK", now);
    ++now;
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(57600, controller.detected().baud);
}

static void test_hint_is_probed_first()
{
    Controller controller;
    RoverConfig cfg = rover_config();
    const std::array<const gnss::Dialect *, 2> dialects{&kDialectA, &kDialectB};
    const std::array<uint32_t, 2> bauds{115200, 9600};
    const gnss::Timing timing = fast_timing();
    gnss::Hint hint{};
    hint.family = Family::kUblox;
    hint.baud = 9600;
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, hint, timing));

    uint32_t now = 0;
    power_up(controller, now);
    const Action set_baud = controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kSetBaud), static_cast<int>(set_baud.kind));
    TEST_ASSERT_EQUAL_UINT32(9600, set_baud.baud);
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "Q\r\n")); // 先试提示的 u-blox
}

static void test_request_reconfigure()
{
    Controller controller;
    RoverConfig cfg = rover_config();
    const std::array<const gnss::Dialect *, 1> dialects{&kDialectA};
    const std::array<uint32_t, 1> bauds{115200};
    TEST_ASSERT_TRUE(controller.init(dialects, bauds, cfg, {}, fast_timing()));

    uint32_t now = 0;
    controller.request_reconfigure(now); // 尚未探测 → 等同 request_reprobe
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kProbing), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(0, controller.stats().reprobes);

    power_up(controller, now);
    (void)controller.poll(now);
    feed(controller, "#V", now);
    ++now;
    (void)controller.poll(now);
    feed(controller, "OK", now);
    ++now;
    (void)controller.poll(now);
    feed(controller, "OK", now);
    ++now;
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(controller.state()));

    controller.request_reconfigure(now);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kConfiguring), static_cast<int>(controller.state()));
    TEST_ASSERT_TRUE(is_write(controller.poll(now), "A0\r\n"));
}

static void test_state_names()
{
    TEST_ASSERT_EQUAL_STRING("off", gnss::state_name(gnss::State::kOff));
    TEST_ASSERT_EQUAL_STRING("power-up", gnss::state_name(gnss::State::kPowerUp));
    TEST_ASSERT_EQUAL_STRING("probing", gnss::state_name(gnss::State::kProbing));
    TEST_ASSERT_EQUAL_STRING("configuring", gnss::state_name(gnss::State::kConfiguring));
    TEST_ASSERT_EQUAL_STRING("running", gnss::state_name(gnss::State::kRunning));
    TEST_ASSERT_EQUAL_STRING("failed", gnss::state_name(gnss::State::kFailed));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_bad_arguments);
    RUN_TEST(test_full_flow_probe_configure_run_reprobe);
    RUN_TEST(test_probe_timeout_exhausts_rounds);
    RUN_TEST(test_nack_retries_then_fails);
    RUN_TEST(test_step_timeout_retries);
    RUN_TEST(test_baud_switch_step);
    RUN_TEST(test_hint_is_probed_first);
    RUN_TEST(test_request_reconfigure);
    RUN_TEST(test_state_names);
    return UNITY_END();
}
