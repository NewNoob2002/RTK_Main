// GNSS 控制器 host 测试：上电/探测/配置/运行/静默重探/失败退避/波特率切换/提示优先（ADR-0010 §2.6）。
// 用测试本地的假驱动（gnss::Receiver 子类）与假解码器驱动状态机，不依赖真实模块。
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <string_view>

#include "gnss/Controller.hpp"
#include "unity.h"

namespace {

using gnss::Ack;
using gnss::Action;
using gnss::CmdBuf;
using gnss::ConfigStep;
using gnss::Controller;
using gnss::Family;
using gnss::Model;
using gnss::ProbeResult;
using gnss::RoverConfig;

bool fakeFirst(uint8_t byte) noexcept
{
    return byte == '#';
}

demux::Step fakeStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
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

constexpr demux::Decoder kFakeDecoder{"fake", 32, &fakeFirst, &fakeStep};

bool probeA(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("VERSION\r\n");
}

bool probeB(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("Q\r\n");
}

bool matchA(const demux::Decoder &, std::span<const uint8_t> frame, ProbeResult &out) noexcept
{
    if (frame.empty() || frame[0] != static_cast<uint8_t>('#')) {
        return false;
    }
    out.model = Model::kUm980;
    return true;
}

bool matchB(const demux::Decoder &, std::span<const uint8_t> frame, ProbeResult &out) noexcept
{
    if (frame.empty() || frame[0] != static_cast<uint8_t>('?')) {
        return false;
    }
    out.model = Model::kZedF9p;
    return true;
}

bool stepA(size_t index, Model, const RoverConfig &, ConfigStep &out) noexcept
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

bool stepB(size_t index, Model, const RoverConfig &, ConfigStep &out) noexcept
{
    out = ConfigStep{};
    if (index == 0) {
        return out.cmd.append("B0\r\n");
    }
    return false;
}

bool stepSwitch(size_t index, Model, const RoverConfig &, ConfigStep &out) noexcept
{
    out = ConfigStep{};
    if (index == 0) {
        if (!out.cmd.append("S0\r\n")) {
            return false;
        }
        out.expectReply = false;
        out.switchBaud = 57600;
        return true;
    }
    if (index == 1) {
        return out.cmd.append("S1\r\n");
    }
    return false;
}

Ack fakeReply(std::span<const uint8_t> frame) noexcept
{
    if (frame.size() == 2 && frame[0] == static_cast<uint8_t>('O') &&
        frame[1] == static_cast<uint8_t>('K')) {
        return Ack::kAck;
    }
    if (frame.size() == 2 && frame[0] == static_cast<uint8_t>('N') &&
        frame[1] == static_cast<uint8_t>('O')) {
        return Ack::kNack;
    }
    return Ack::kNone;
}

/**
 * 假驱动：把上面的自由函数包成 gnss::Receiver 子类，其余成员走基类默认实现。
 * 只用于验证控制器的时序与状态迁移，不验证任何协议字节。
 */
class FakeReceiver final : public gnss::Receiver {
public:
    using ProbeFn = bool (*)(CmdBuf &);
    using MatchFn = bool (*)(const demux::Decoder &, std::span<const uint8_t>, ProbeResult &);
    using StepFn = bool (*)(size_t, Model, const RoverConfig &, ConfigStep &);

    constexpr FakeReceiver(Family family, const char *name, ProbeFn probe, MatchFn match,
                           StepFn step) noexcept
        : family_(family), name_(name), probe_(probe), match_(match), step_(step)
    {}

    [[nodiscard]] Family family() const noexcept override
    {
        return family_;
    }
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return name_;
    }
    [[nodiscard]] std::span<const demux::Decoder *const> decoders() const noexcept override
    {
        return {};
    }
    [[nodiscard]] uint32_t caps(Model) const noexcept override
    {
        return 0U;
    }
    [[nodiscard]] bool buildProbe(CmdBuf &out) const noexcept override
    {
        return probe_(out);
    }
    [[nodiscard]] bool matchProbe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  ProbeResult &out) const noexcept override
    {
        return match_(dec, frame, out);
    }
    [[nodiscard]] bool roverStep(size_t index, Model model, const RoverConfig &cfg,
                                 ConfigStep &out) const noexcept override
    {
        return step_(index, model, cfg, out);
    }
    [[nodiscard]] Ack classifyReply(const demux::Decoder &, std::span<const uint8_t> frame,
                                    std::span<const uint8_t>) const noexcept override
    {
        return fakeReply(frame);
    }

    bool saveStep(ConfigStep &out) const noexcept override
    {
        out = ConfigStep{};
        return family_ == Family::kUnicore && out.cmd.append("SAVE\r\n");
    }
    bool factoryResetStep(uint8_t, const ProbeResult &, ConfigStep &out) const noexcept override
    {
        out = ConfigStep{.expectReply = false, .switchBaud = 115200U};
        return out.cmd.append("RESET\r\n");
    }
    bool baudStep(uint8_t, uint32_t baud, ConfigStep &out) const noexcept override
    {
        out = ConfigStep{.expectReply = false, .switchBaud = baud};
        return out.cmd.append("BAUD\r\n");
    }
    bool modeStep(const gnss::ModeRequest &, uint8_t idx, ConfigStep &out) const noexcept override
    {
        if (family_ != Family::kUnicore) {
            return false;
        }
        out = ConfigStep{};
        if (idx == 0U) {
            return out.cmd.append("M0\r\n");
        }
        if (idx == 1U) {
            return out.cmd.append("M1\r\n");
        }
        return false;
    }

private:
    Family family_;
    const char *name_;
    ProbeFn probe_;
    MatchFn match_;
    StepFn step_;
};

constinit const FakeReceiver kReceiverA{Family::kUnicore, "A", &probeA, &matchA, &stepA};
constinit const FakeReceiver kReceiverB{Family::kUblox, "B", &probeB, &matchB, &stepB};
constinit const FakeReceiver kReceiverS{Family::kComnav, "S", &probeB, &matchB, &stepSwitch};

bool bytesEqual(std::span<const uint8_t> data, const char *text)
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

bool isPower(const Action &action, bool on)
{
    return action.kind == Action::Kind::kPower && action.on == on;
}

bool isWrite(const Action &action, const char *text)
{
    return action.kind == Action::Kind::kWrite && bytesEqual(action.bytes, text);
}

gnss::Timing fastTiming()
{
    gnss::Timing timing{};
    timing.powerOffMs = 10;
    timing.bootMs = 20;
    timing.probeTimeoutMs = 50;
    timing.probeRounds = 2;
    timing.stepRetries = 1;
    timing.silenceMs = 100;
    timing.retryBackoffMs = 30;
    return timing;
}

RoverConfig roverConfig()
{
    RoverConfig cfg{};
    cfg.rateHz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    return cfg;
}

void powerUp(Controller &controller, uint32_t &now)
{
    controller.start(now);
    TEST_ASSERT_TRUE(isPower(controller.poll(now), false));
    now += 10;
    TEST_ASSERT_TRUE(isPower(controller.poll(now), true));
    now += 20;
}

void feed(Controller &controller, const char *frame, uint32_t now)
{
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(frame),
                                         std::string(frame).size());
    controller.onFrame(kFakeDecoder, bytes, now);
}

} // namespace

void setUp() {}
void tearDown() {}

static void testInitRejectsBadArguments()
{
    Controller controller;
    RoverConfig cfg{};
    gnss::Timing timing{};
    const std::array<const gnss::Receiver *, 1> receivers{&kReceiverA};
    const std::array<uint32_t, 1> bauds{115200};
    const std::array<const gnss::Receiver *, 1> withNull{nullptr};
    const std::array<uint32_t, 1> zero{0};

    TEST_ASSERT_FALSE(controller.init({}, bauds, cfg, gnss::OutputPlan{}, {}, timing));
    TEST_ASSERT_FALSE(controller.init(receivers, {}, cfg, gnss::OutputPlan{}, {}, timing));
    TEST_ASSERT_FALSE(controller.init(withNull, bauds, cfg, gnss::OutputPlan{}, {}, timing));
    TEST_ASSERT_FALSE(controller.init(receivers, zero, cfg, gnss::OutputPlan{}, {}, timing));
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, timing));
}

static void testFullFlowProbeConfigureRunReprobe()
{
    Controller controller;
    RoverConfig cfg = roverConfig();
    const std::array<const gnss::Receiver *, 1> receivers{&kReceiverA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fastTiming();
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, timing));

    uint32_t now = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kOff), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone),
                      static_cast<int>(controller.poll(now).kind));

    powerUp(controller, now);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "VERSION\r\n"));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kProbing),
                      static_cast<int>(controller.state()));

    feed(controller, "#VERSIONA,1", now);
    ++now;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "A0\r\n"));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kConfiguring),
                      static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(Model::kUm980),
                      static_cast<int>(controller.detected().probe.model));
    TEST_ASSERT_EQUAL_UINT32(115200, controller.detected().baud);

    feed(controller, "OK", now);
    ++now;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "A1\r\n"));

    feed(controller, "OK", now);
    ++now;
    const Action action = controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone), static_cast<int>(action.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning),
                      static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().stepsSent);

    // 静默超时（>100 ms）→ 重新探测；任意帧刷新 last_frame。
    now += 50;
    feed(controller, "noise", now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone),
                      static_cast<int>(controller.poll(now + 90).kind));
    now += 101;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "VERSION\r\n"));
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().reprobes);

    controller.requestReprobe(now);
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().reprobes);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kWrite),
                      static_cast<int>(controller.poll(now).kind));
}

static void testProbeTimeoutExhaustsRounds()
{
    Controller controller;
    RoverConfig cfg = roverConfig();
    const std::array<const gnss::Receiver *, 1> receivers{&kReceiverA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fastTiming();
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, timing));

    uint32_t now = 0;
    powerUp(controller, now);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "VERSION\r\n"));

    now += 50; // 超时 → 第一轮结束，进入第二轮
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "VERSION\r\n"));
    now += 50; // 第二轮超时 → 失败
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone),
                      static_cast<int>(controller.poll(now).kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kFailed), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().probesSent);
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().failures);

    now += 29; // 退避未到
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone),
                      static_cast<int>(controller.poll(now).kind));
    now += 1; // 退避到 → 重新上电
    TEST_ASSERT_TRUE(isPower(controller.poll(now), false));
}

static void testNackRetriesThenFails()
{
    Controller controller;
    RoverConfig cfg = roverConfig();
    const std::array<const gnss::Receiver *, 1> receivers{&kReceiverA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fastTiming();
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, timing));

    uint32_t now = 0;
    powerUp(controller, now);
    (void)controller.poll(now);
    feed(controller, "#V", now);
    ++now;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "A0\r\n"));

    feed(controller, "NO", now);
    ++now;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "A0\r\n")); // 第一次重发
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().stepNacks);

    feed(controller, "NO", now);
    ++now;
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kNone),
                      static_cast<int>(controller.poll(now).kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kFailed), static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(2, controller.stats().stepNacks);
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().failures);
}

static void testStepTimeoutRetries()
{
    Controller controller;
    RoverConfig cfg = roverConfig();
    const std::array<const gnss::Receiver *, 1> receivers{&kReceiverA};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fastTiming();
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, timing));

    uint32_t now = 0;
    powerUp(controller, now);
    (void)controller.poll(now);
    feed(controller, "#V", now);
    ++now;
    (void)controller.poll(now);

    now += 500; // ConfigStep 默认 timeoutMs=500
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "A0\r\n"));
    TEST_ASSERT_EQUAL_UINT32(1, controller.stats().stepTimeouts);
}

static void testBaudSwitchStep()
{
    Controller controller;
    RoverConfig cfg = roverConfig();
    const std::array<const gnss::Receiver *, 1> receivers{&kReceiverS};
    const std::array<uint32_t, 1> bauds{115200};
    const gnss::Timing timing = fastTiming();
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, timing));

    uint32_t now = 0;
    powerUp(controller, now);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "Q\r\n"));
    feed(controller, "?", now);
    ++now;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "S0\r\n"));

    const Action setBaud = controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kSetBaud), static_cast<int>(setBaud.kind));
    TEST_ASSERT_EQUAL_UINT32(57600, setBaud.baud);
    TEST_ASSERT_EQUAL_UINT32(57600, controller.hostBaud());
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "S1\r\n"));

    feed(controller, "OK", now);
    ++now;
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning),
                      static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(57600, controller.detected().baud);
}

static void testHintIsProbedFirst()
{
    Controller controller;
    RoverConfig cfg = roverConfig();
    const std::array<const gnss::Receiver *, 2> receivers{&kReceiverA, &kReceiverB};
    const std::array<uint32_t, 2> bauds{115200, 9600};
    const gnss::Timing timing = fastTiming();
    gnss::Hint hint{};
    hint.family = Family::kUblox;
    hint.baud = 9600;
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, hint, timing));

    uint32_t now = 0;
    powerUp(controller, now);
    const Action setBaud = controller.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(Action::Kind::kSetBaud), static_cast<int>(setBaud.kind));
    TEST_ASSERT_EQUAL_UINT32(9600, setBaud.baud);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "Q\r\n")); // 先试提示的 u-blox
}

static void testRequestReconfigure()
{
    Controller controller;
    RoverConfig cfg = roverConfig();
    const std::array<const gnss::Receiver *, 1> receivers{&kReceiverA};
    const std::array<uint32_t, 1> bauds{115200};
    TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, fastTiming()));

    uint32_t now = 0;
    controller.requestReconfigure(now); // 尚未探测 → 等同 requestReprobe
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kProbing),
                      static_cast<int>(controller.state()));
    TEST_ASSERT_EQUAL_UINT32(0, controller.stats().reprobes);

    powerUp(controller, now);
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
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning),
                      static_cast<int>(controller.state()));

    controller.requestReconfigure(now);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kConfiguring),
                      static_cast<int>(controller.state()));
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "A0\r\n"));
}

/** @brief 让假接收机探测并完成基础配置。 */
void configureForMaintenance(Controller &controller, const FakeReceiver &receiver,
                             const RoverConfig &cfg, uint32_t &now)
{
    static constexpr std::array<uint32_t, 1> kBauds{115200U};
    static constexpr std::array<const gnss::Receiver *, 1> kReceiversA{&kReceiverA};
    static constexpr std::array<const gnss::Receiver *, 1> kReceiversB{&kReceiverB};
    const bool unicore = &receiver == &kReceiverA;
    TEST_ASSERT_TRUE(controller.init(unicore ? kReceiversA : kReceiversB, kBauds, cfg,
                                     gnss::OutputPlan{}, {}, fastTiming()));
    TEST_ASSERT_FALSE(controller.requestSave());
    TEST_ASSERT_FALSE(controller.requestFactoryReset());
    TEST_ASSERT_FALSE(controller.requestBaud(460800U));
    controller.requestReprobe(now);
    TEST_ASSERT_EQUAL(Action::Kind::kWrite, controller.poll(now).kind);
    feed(controller, unicore ? "#V" : "?V", ++now);
    TEST_ASSERT_EQUAL(Action::Kind::kWrite, controller.poll(now).kind);
    feed(controller, "OK", ++now);
    if (unicore) {
        TEST_ASSERT_EQUAL(Action::Kind::kWrite, controller.poll(now).kind);
        feed(controller, "OK", ++now);
    }
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL(gnss::State::kRunning, controller.state());
}

/** @brief 保存 ACK、NACK、超时与重复请求。 */
void testMaintenanceSave()
{
    Controller controller;
    const RoverConfig cfg = roverConfig();
    uint32_t now = 0U;
    configureForMaintenance(controller, kReceiverA, cfg, now);
    TEST_ASSERT_TRUE(controller.requestSave());
    TEST_ASSERT_FALSE(controller.requestSave());
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "SAVE\r\n"));
    feed(controller, "OK", ++now);
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL_UINT32(1U, controller.stats().maintenanceSeq);
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kOk, controller.stats().maintenanceResult);

    TEST_ASSERT_TRUE(controller.requestSave());
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "SAVE\r\n"));
    feed(controller, "NO", ++now);
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL_UINT32(2U, controller.stats().maintenanceSeq);
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kNack, controller.stats().maintenanceResult);

    TEST_ASSERT_TRUE(controller.requestSave());
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "SAVE\r\n"));
    (void)controller.poll(now + gnss::kStepTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(3U, controller.stats().maintenanceSeq);
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kTimeout, controller.stats().maintenanceResult);
}

/** @brief 验证目标速率探测成功、失败回旧速率并重新探测。 */
void testMaintenanceBaud()
{
    Controller controller;
    const RoverConfig cfg = roverConfig();
    uint32_t now = 0U;
    configureForMaintenance(controller, kReceiverA, cfg, now);
    TEST_ASSERT_TRUE(controller.requestBaud(460800U));
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "BAUD\r\n"));
    now += 100U;
    TEST_ASSERT_EQUAL_UINT32(460800U, controller.poll(now).baud);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "VERSION\r\n"));
    feed(controller, "#V", ++now);
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL_UINT32(460800U, controller.detected().baud);
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kOk, controller.stats().maintenanceResult);

    TEST_ASSERT_TRUE(controller.requestBaud(921600U));
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "BAUD\r\n"));
    now += 100U;
    TEST_ASSERT_EQUAL_UINT32(921600U, controller.poll(now).baud);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "VERSION\r\n"));
    now += 700U;
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL_UINT32(460800U, controller.poll(now).baud);
    TEST_ASSERT_EQUAL(gnss::State::kProbing, controller.state());
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kTimeout, controller.stats().maintenanceResult);
}

/** @brief 不支持的保存，以及复位确认后进入通用重探。 */
void testMaintenanceReset()
{
    Controller controller;
    const RoverConfig cfg = roverConfig();
    uint32_t now = 0U;
    configureForMaintenance(controller, kReceiverB, cfg, now);
    TEST_ASSERT_TRUE(controller.requestSave());
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kUnsupported, controller.stats().maintenanceResult);
    TEST_ASSERT_TRUE(controller.requestFactoryReset());
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "RESET\r\n"));
    TEST_ASSERT_EQUAL(Action::Kind::kNone, controller.poll(now).kind);
    TEST_ASSERT_EQUAL(gnss::State::kProbing, controller.state());
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kOk, controller.stats().maintenanceResult);
    TEST_ASSERT_EQUAL_UINT32(2U, controller.stats().maintenanceSeq);
    now += fastTiming().bootMs;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "Q\r\n"));
    now += fastTiming().probeTimeoutMs;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "Q\r\n"));
    feed(controller, "?V", ++now);
    TEST_ASSERT_EQUAL(Action::Kind::kWrite, controller.poll(now).kind);
    feed(controller, "OK", ++now);
    (void)controller.poll(now);
    TEST_ASSERT_EQUAL(gnss::State::kRunning, controller.state());
}

static void testStateNames()
{
    TEST_ASSERT_EQUAL_STRING("off", gnss::stateName(gnss::State::kOff));
    TEST_ASSERT_EQUAL_STRING("power-up", gnss::stateName(gnss::State::kPowerUp));
    TEST_ASSERT_EQUAL_STRING("probing", gnss::stateName(gnss::State::kProbing));
    TEST_ASSERT_EQUAL_STRING("configuring", gnss::stateName(gnss::State::kConfiguring));
    TEST_ASSERT_EQUAL_STRING("running", gnss::stateName(gnss::State::kRunning));
    TEST_ASSERT_EQUAL_STRING("failed", gnss::stateName(gnss::State::kFailed));
}

/** @brief C5a 多步模式切换：成功、NACK 重试后失败、unsupported。 */
void testMaintenanceMode()
{
    Controller controller;
    const RoverConfig cfg = roverConfig();
    uint32_t now = 0U;
    configureForMaintenance(controller, kReceiverA, cfg, now);
    gnss::ModeRequest req{};
    TEST_ASSERT_TRUE(controller.requestMode(req));
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "M0\r\n"));
    // 第 0 步超时一次后重试并成功，推进到第 1 步。
    now += gnss::kStepTimeoutMs;
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "M0\r\n"));
    feed(controller, "OK", ++now);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "M1\r\n"));
    feed(controller, "OK", ++now);
    TEST_ASSERT_EQUAL(Action::Kind::kNone, controller.poll(now).kind);
    TEST_ASSERT_EQUAL(gnss::State::kRunning, controller.state());
    TEST_ASSERT_EQUAL(gnss::Maintenance::kMode, controller.stats().maintenance);
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kOk, controller.stats().maintenanceResult);

    // 连续 NACK 耗尽重试后返回 kNack。
    TEST_ASSERT_TRUE(controller.requestMode(req));
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "M0\r\n"));
    feed(controller, "NO", ++now);
    TEST_ASSERT_TRUE(isWrite(controller.poll(now), "M0\r\n"));
    feed(controller, "NO", ++now);
    TEST_ASSERT_EQUAL(Action::Kind::kNone, controller.poll(now).kind);
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kNack, controller.stats().maintenanceResult);

    // 不支持模式切换的驱动返回 kUnsupported。
    Controller ubloxCtrl;
    uint32_t t2 = 0U;
    configureForMaintenance(ubloxCtrl, kReceiverB, cfg, t2);
    TEST_ASSERT_TRUE(ubloxCtrl.requestMode(req));
    TEST_ASSERT_EQUAL(gnss::MaintenanceResult::kUnsupported, ubloxCtrl.stats().maintenanceResult);
    TEST_ASSERT_EQUAL(gnss::State::kRunning, ubloxCtrl.state());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testInitRejectsBadArguments);
    RUN_TEST(testFullFlowProbeConfigureRunReprobe);
    RUN_TEST(testProbeTimeoutExhaustsRounds);
    RUN_TEST(testNackRetriesThenFails);
    RUN_TEST(testStepTimeoutRetries);
    RUN_TEST(testBaudSwitchStep);
    RUN_TEST(testHintIsProbedFirst);
    RUN_TEST(testRequestReconfigure);
    RUN_TEST(testStateNames);
    RUN_TEST(testMaintenanceSave);
    RUN_TEST(testMaintenanceBaud);
    RUN_TEST(testMaintenanceReset);
    RUN_TEST(testMaintenanceMode);
    return UNITY_END();
}
