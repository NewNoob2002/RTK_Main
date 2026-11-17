// u-blox 驱动 host 测试：UBX-MON-VER 探测、CFG-VALSET 脚本字节、Fletcher 校验、ACK/NAK（ADR-0010 §2.2/§2.4）。
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "demux/decoders/Ubx.hpp"
#include "gnss/drivers/UbloxReceiver.hpp"
#include "unity.h"

// S1（ADR-0012 §2.8、CPP-011）：家族基类不可直接实例化；型号只实例化平凡析构的 final 叶子类。
static_assert(!std::is_constructible_v<gnss::UbloxReceiver>);
static_assert(std::is_final_v<gnss::ZedF9pReceiver>);
static_assert(std::is_trivially_destructible_v<gnss::ZedF9pReceiver>);

namespace {
const gnss::Receiver &ubloxDrv() noexcept
{
    return *gnss::receiverFor(gnss::Family::kUblox);
}

constexpr uint32_t kTestBaud = 921600U;

using Bytes = std::vector<uint8_t>;

Bytes makeFrame(uint8_t cls, uint8_t id, const Bytes &payload)
{
    Bytes frame{0xB5,
                0x62,
                cls,
                id,
                static_cast<uint8_t>(payload.size() & 0xFF),
                static_cast<uint8_t>((payload.size() >> 8) & 0xFF)};
    frame.insert(frame.end(), payload.begin(), payload.end());
    uint8_t ckA = 0;
    uint8_t ckB = 0;
    for (size_t i = 2; i < frame.size(); ++i) {
        ckA = static_cast<uint8_t>(ckA + frame[i]);
        ckB = static_cast<uint8_t>(ckB + ckA);
    }
    frame.push_back(ckA);
    frame.push_back(ckB);
    return frame;
}

void appendText(Bytes &payload, const char *text, size_t width)
{
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    for (size_t i = 0; i < width; ++i) {
        payload.push_back(i < len ? static_cast<uint8_t>(text[i]) : 0U);
    }
}

Bytes monVer(const char *model, const char *fw)
{
    Bytes payload;
    appendText(payload, "EXT CORE 1.00 (61b2dd)", 30);
    appendText(payload, "00180000", 10);
    Bytes mod(30, 0);
    for (size_t i = 0; i < 4; ++i) {
        mod[i] = static_cast<uint8_t>("MOD="[i]);
    }
    for (size_t i = 0; model[i] != '\0' && 4 + i < mod.size(); ++i) {
        mod[4 + i] = static_cast<uint8_t>(model[i]);
    }
    payload.insert(payload.end(), mod.begin(), mod.end());
    Bytes ver(30, 0);
    for (size_t i = 0; i < 6; ++i) {
        ver[i] = static_cast<uint8_t>("FWVER="[i]);
    }
    for (size_t i = 0; fw[i] != '\0' && 6 + i < ver.size(); ++i) {
        ver[6 + i] = static_cast<uint8_t>(fw[i]);
    }
    payload.insert(payload.end(), ver.begin(), ver.end());
    return makeFrame(0x0A, 0x04, payload);
}

bool cmdEquals(const gnss::CmdBuf &cmd, const Bytes &expected)
{
    const std::span<const uint8_t> view = cmd.view();
    if (view.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < view.size(); ++i) {
        if (view[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

bool hasItem(const gnss::CmdBuf &cmd, uint32_t key, uint32_t value, size_t width)
{
    const std::span<const uint8_t> view = cmd.view();
    for (size_t i = 0; i + 5 + width <= view.size(); ++i) {
        const uint32_t k =
            static_cast<uint32_t>(view[i]) | (static_cast<uint32_t>(view[i + 1]) << 8) |
            (static_cast<uint32_t>(view[i + 2]) << 16) | (static_cast<uint32_t>(view[i + 3]) << 24);
        if (k != key) {
            continue;
        }
        uint32_t v = 0;
        for (size_t j = 0; j < width; ++j) {
            v |= static_cast<uint32_t>(view[i + 4 + j]) << (8 * j);
        }
        return v == value;
    }
    return false;
}

bool validFletcher(const Bytes &frame)
{
    uint8_t ckA = 0;
    uint8_t ckB = 0;
    for (size_t i = 2; i + 2 < frame.size(); ++i) {
        ckA = static_cast<uint8_t>(ckA + frame[i]);
        ckB = static_cast<uint8_t>(ckB + ckA);
    }
    return frame[frame.size() - 2] == ckA && frame[frame.size() - 1] == ckB;
}

} // namespace

void setUp() {}
void tearDown() {}

static void testBuildProbe()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_TRUE(ubloxDrv().buildProbe(cmd));
    TEST_ASSERT_TRUE(cmdEquals(cmd, Bytes{0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34}));
}

static void testMatchProbe()
{
    const Bytes frame = monVer("ZED-F9P", "HPG 1.32");
    std::span<const uint8_t> span(frame.data(), frame.size());
    gnss::ProbeResult result{};
    TEST_ASSERT_TRUE(ubloxDrv().matchProbe(demux::ubx::kDecoder, span, result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kZedF9p), static_cast<int>(result.model));
    TEST_ASSERT_EQUAL_STRING("HPG 1.32", result.fwVersion.data());

    const Bytes other = monVer("ZED-F9R", "HPG 1.32");
    std::span<const uint8_t> otherSpan(other.data(), other.size());
    TEST_ASSERT_FALSE(ubloxDrv().matchProbe(demux::ubx::kDecoder, otherSpan, result));
}

static void testRoverScript()
{
    gnss::RoverConfig cfg{};
    cfg.rateHz = 5;
    cfg.gsv = true;
    cfg.portBaud = kTestBaud;

    // P3b 之后基础脚本只剩两步：语句输出改由 buildOutput 逐项下发（ADR-0012 §2.4）。
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(ubloxDrv().roverStep(0, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_TRUE(step.expectReply);
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x30210001U, 200U, 2));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10730004U, 1U, 1));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10740002U, 1U, 1));

    TEST_ASSERT_TRUE(ubloxDrv().roverStep(1, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_FALSE(step.expectReply);
    TEST_ASSERT_EQUAL_UINT32(kTestBaud, step.switchBaud);
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x40520001U, kTestBaud, 4));
    TEST_ASSERT_TRUE(validFletcher(Bytes(step.cmd.view().begin(), step.cmd.view().end())));

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(ubloxDrv().roverStep(2, gnss::Model::kZedF9p, cfg, none));

    // 不改波特率时脚本只有一步。
    cfg.portBaud = 0;
    TEST_ASSERT_FALSE(ubloxDrv().roverStep(1, gnss::Model::kZedF9p, cfg, none));
}

/** buildOutput：CFG-MSGOUT 单键 VALSET，值 = 每历元次数（周期 × 频率）。 */
static void testBuildOutput()
{
    const gnss::Receiver &r = ubloxDrv();
    gnss::OutputPlan plan{};
    plan.port = 2;
    plan.rateHz = 5;
    gnss::CmdBuf cmd;

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 200,
                                                     gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100BBU, 1U, 1));
    TEST_ASSERT_TRUE(validFletcher(Bytes(cmd.view().begin(), cmd.view().end())));

    // 1 Hz 下 2 s 周期 = 每两历元一次。
    plan.rateHz = 1;
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kRmc, 2000,
                                                     gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100ACU, 2U, 1));

    // 周期 0 表示关闭。
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kOk),
        static_cast<int>(r.buildOutput(gnss::OutputMsg::kGst, 0, gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100D4U, 0U, 1));

    // 20 Hz 下 50 ms 正好一个历元。
    plan.rateHz = 20;
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kGsv, 50,
                                                     gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100C3U, 1U, 1));

    // 该系列没有 SATSINFOA/HEADINGA。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kSatsInfo, 1000,
                                                     gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kHeading, 1000,
                                                     gnss::Model::kZedF9p, plan, cmd)));
}

static void testMatchReply()
{
    const Bytes ack = makeFrame(0x05, 0x01, Bytes{0x06, 0x8A});
    std::span<const uint8_t> ackSpan(ack.data(), ack.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kAck),
                      static_cast<int>(ubloxDrv().classifyReply(demux::ubx::kDecoder, ackSpan,
                                                                std::span<const uint8_t>())));

    const Bytes nak = makeFrame(0x05, 0x00, Bytes{0x06, 0x8A});
    std::span<const uint8_t> nakSpan(nak.data(), nak.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNack),
                      static_cast<int>(ubloxDrv().classifyReply(demux::ubx::kDecoder, nakSpan,
                                                                std::span<const uint8_t>())));

    const Bytes other = makeFrame(0x05, 0x01, Bytes{0x06, 0x01});
    std::span<const uint8_t> otherSpan(other.data(), other.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(ubloxDrv().classifyReply(demux::ubx::kDecoder, otherSpan,
                                                                std::span<const uint8_t>())));

    const Bytes nmea{0x24, 0x47, 0x4E, 0x47, 0x47, 0x41};
    std::span<const uint8_t> nmeaSpan(nmea.data(), nmea.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(ubloxDrv().classifyReply(demux::ubx::kDecoder, nmeaSpan,
                                                                std::span<const uint8_t>())));
}

static void testCaps()
{
    TEST_ASSERT_BITS(gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kRate20Hz,
                     gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kRate20Hz,
                     ubloxDrv().caps(gnss::Model::kZedF9p));
    TEST_ASSERT_BITS(gnss::cap::kHeading, 0U, ubloxDrv().caps(gnss::Model::kZedF9p));
    // 单型号驱动：forModel 对任何型号都返回自身（ADR-0012 §2.8）。
    TEST_ASSERT_TRUE(&ubloxDrv().forModel(gnss::Model::kZedF9p) == &ubloxDrv());
    TEST_ASSERT_TRUE(&ubloxDrv().forModel(gnss::Model::kUm982) == &ubloxDrv());
}

// ---- 黄金向量比对（ADR-0012 P3a 第 3 步）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenUblox.inc"
#include "PlanCheck.hpp"

static void testGoldenProbe()
{
    TEST_ASSERT_TRUE(golden_check::probeMatches(ubloxDrv(), golden_Ublox::kProbe));
}

/** P3b：基础脚本 + buildOutput 逐项下发，合起来必须与基线脚本一致（按 VALSET 键值对比较）。 */
static void testGoldenOutputPlan()
{
    const size_t bad =
        plan_check::ubxPlanMismatch(ubloxDrv(), golden_Ublox::kRover, golden_Ublox::kSteps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenProbeMatch()
{
    const size_t bad =
        golden_check::probeMatchMismatch(ubloxDrv(), golden_Ublox::kProbeMatch, nullptr);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenReplies()
{
    const size_t bad = golden_check::replyMismatch(ubloxDrv(), golden_Ublox::kReply, nullptr);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenCaps()
{
    const size_t bad = golden_check::capsMismatch(ubloxDrv(), golden_Ublox::kCaps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testGoldenProbe);
    RUN_TEST(testGoldenOutputPlan);
    RUN_TEST(testGoldenProbeMatch);
    RUN_TEST(testGoldenReplies);
    RUN_TEST(testGoldenCaps);
    RUN_TEST(testBuildProbe);
    RUN_TEST(testMatchProbe);
    RUN_TEST(testRoverScript);
    RUN_TEST(testBuildOutput);
    RUN_TEST(testMatchReply);
    RUN_TEST(testCaps);
    return UNITY_END();
}
