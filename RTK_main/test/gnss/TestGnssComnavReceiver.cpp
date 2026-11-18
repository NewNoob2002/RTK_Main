// ComNav K803 驱动 host 测试：<OK/<ERROR 行解码、LOG VERSIONA ONCE 探测、NovAtel 风格脚本、应答匹配。
// 实验性驱动：按 ADR-0010 §2.2 与公开的 NovAtel 兼容资料断言，未经实物验证。
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "demux/decoders/UnicoreHash.hpp"
#include "gnss/drivers/ComnavReceiver.hpp"
#include "unity.h"

// 驱动层次约束（ADR-0012 §2.8、CPP-011）：家族基类不可直接实例化；型号只实例化平凡析构的 final 叶子类。
static_assert(!std::is_constructible_v<gnss::ComnavReceiver>);
static_assert(std::is_final_v<gnss::K803Receiver>);
static_assert(std::is_trivially_destructible_v<gnss::K803Receiver>);

namespace {
const gnss::Receiver &comnavDrv() noexcept
{
    return *gnss::receiverFor(gnss::Family::kComnav);
}

constexpr uint32_t kTestBaud = 921600U;

bool textEquals(std::span<const uint8_t> data, std::string_view text)
{
    if (data.size() != text.size()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (data[i] != static_cast<uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

/** 测试里把字符串当只读字节看；span 无法引用字符数据，必须做一次重解释。 */
std::span<const uint8_t> bytesOf(std::string_view text)
{
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

void testPromptDecoder()
{
    TEST_ASSERT_TRUE(gnss::comnav::promptFirstByte('<'));
    TEST_ASSERT_FALSE(gnss::comnav::promptFirstByte('$'));

    size_t need = 0;
    const std::string partial = "<OK";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::comnav::promptStep(bytesOf(partial), need)));
    TEST_ASSERT_EQUAL_UINT32(1, need);

    const std::string cr = "<OK\r";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::comnav::promptStep(bytesOf(cr), need)));

    const std::string ok = "<OK\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kFrame),
                      static_cast<int>(gnss::comnav::promptStep(bytesOf(ok), need)));

    const std::string binary = std::string("<OK\x01") + "\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::comnav::promptStep(bytesOf(binary), need)));
}

void testBuildProbe()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_TRUE(comnavDrv().buildProbe(cmd));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "LOG VERSIONA ONCE\r\n"));
}

void testMatchProbe()
{
    // VERSIONA 应答是 Unicore Hash 报文（'#' + crc32），不是 NMEA 的 '$'。
    const std::string frame = "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"K803\",\"1\",\"2\"*12345678\r\n";
    gnss::ProbeResult result{};
    TEST_ASSERT_TRUE(comnavDrv().matchProbe(demux::unicore_hash::kDecoder, bytesOf(frame), result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kK803), static_cast<int>(result.model));

    const std::string other =
        "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"OEM719\",\"1\",\"2\"*12345678\r\n";
    TEST_ASSERT_FALSE(
        comnavDrv().matchProbe(demux::unicore_hash::kDecoder, bytesOf(other), result));
}

void testRoverScript()
{
    gnss::RoverConfig cfg{};
    cfg.rateHz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    cfg.portBaud = kTestBaud;

    // 输出计划拆分后基础脚本只剩三步：语句输出改由 buildOutput 逐项下发（ADR-0012 §2.4）。
    const std::array<std::string_view, 3> expected{"UNLOGALL COM2\r\n", "MODE ROVER\r\n",
                                                   "COM COM2 921600\r\n"};
    for (size_t i = 0; i < 2; ++i) {
        gnss::ConfigStep step{};
        TEST_ASSERT_TRUE(comnavDrv().roverStep(i, gnss::Model::kK803, cfg, step));
        TEST_ASSERT_TRUE(textEquals(step.cmd.view(), expected[i]));
        TEST_ASSERT_TRUE(step.expectReply);
        TEST_ASSERT_EQUAL_UINT32(0, step.switchBaud);
    }
    gnss::ConfigStep last{};
    TEST_ASSERT_TRUE(comnavDrv().roverStep(2, gnss::Model::kK803, cfg, last));
    TEST_ASSERT_TRUE(textEquals(last.cmd.view(), expected[2]));
    TEST_ASSERT_FALSE(last.expectReply);
    TEST_ASSERT_EQUAL_UINT32(kTestBaud, last.switchBaud);

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(comnavDrv().roverStep(3, gnss::Model::kK803, cfg, none));

    // 不改波特率时脚本只有两步。
    cfg.portBaud = 0;
    TEST_ASSERT_FALSE(comnavDrv().roverStep(2, gnss::Model::kK803, cfg, none));
}

/** buildOutput：LOG/UNLOG 两种写法，周期 10 ms 粒度。 */
void testBuildOutput()
{
    const gnss::Receiver &r = comnavDrv();
    gnss::OutputPlan plan{};
    plan.port = 2;
    gnss::CmdBuf cmd;

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 1000,
                                                     gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "LOG COM2 GPGGA ONTIME 1\r\n"));

    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kOk),
        static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 200, gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "LOG COM2 GPGGA ONTIME 0.2\r\n"));

    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kOk),
        static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 0, gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "UNLOG COM2 GPGGA\r\n"));

    // 该型号不下发 SATSINFOA/HEADINGA。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kSatsInfo, 1000,
                                                     gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kHeading, 1000,
                                                     gnss::Model::kK803, plan, cmd)));

    // 非 10 ms 倍数的周期无法表示。
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kInvalidArg),
        static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 33, gnss::Model::kK803, plan, cmd)));
}

void testMatchReply()
{
    const std::string ok = "<OK\r\n";
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Ack::kAck),
        static_cast<int>(comnavDrv().classifyReply(gnss::comnav::kPromptDecoder, bytesOf(ok), {})));
    const std::string nak = "<ERROR\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNack),
                      static_cast<int>(comnavDrv().classifyReply(gnss::comnav::kPromptDecoder,
                                                                 bytesOf(nak), {})));
    const std::string other = "<WARNING\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(comnavDrv().classifyReply(gnss::comnav::kPromptDecoder,
                                                                 bytesOf(other), {})));
    // 其他解码器的帧不认。
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(comnavDrv().classifyReply(demux::unicore_hash::kDecoder,
                                                                 bytesOf(ok), {})));
}

void testCaps()
{
    const uint32_t k803 = comnavDrv().caps(gnss::Model::kK803);
    TEST_ASSERT_TRUE((k803 & gnss::cap::kRtcmIn) != 0);
    TEST_ASSERT_TRUE((k803 & gnss::cap::kBase) != 0);
    TEST_ASSERT_EQUAL_UINT32(0, k803 & gnss::cap::kIns);
    TEST_ASSERT_EQUAL_UINT32(0, k803 & gnss::cap::kHeading);
    // 单型号驱动：forModel 对任何型号都返回自身（ADR-0012 §2.8）。
    TEST_ASSERT_TRUE(&comnavDrv().forModel(gnss::Model::kK803) == &comnavDrv());
    TEST_ASSERT_TRUE(&comnavDrv().forModel(gnss::Model::kUm982) == &comnavDrv());
}

} // namespace

void setUp() {}

void tearDown() {}

// ---- 黄金向量比对（ADR-0012 §2.4）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenComnav.inc"
#include "PlanCheck.hpp"

static void testGoldenProbe()
{
    TEST_ASSERT_TRUE(golden_check::probeMatches(comnavDrv(), golden_Comnav::kProbe));
}

/** 基础脚本 + buildOutput 逐项下发，合起来的命令集合必须与基线脚本一致。 */
static void testGoldenOutputPlan()
{
    const size_t bad =
        plan_check::planMismatch(comnavDrv(), golden_Comnav::kRover, golden_Comnav::kSteps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenProbeMatch()
{
    const size_t bad =
        golden_check::probeMatchMismatch(comnavDrv(), golden_Comnav::kProbeMatch, nullptr);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenReplies()
{
    const size_t bad = golden_check::replyMismatch(comnavDrv(), golden_Comnav::kReply,
                                                   &gnss::comnav::kPromptDecoder);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenCaps()
{
    const size_t bad = golden_check::capsMismatch(comnavDrv(), golden_Comnav::kCaps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

/** @brief C2 一次性命令的字节级合约。 */
static void testMaintenanceCommands()
{
    const auto &driver = comnavDrv();
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(driver.saveStep(step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "SAVECONFIG\r\n"));
    TEST_ASSERT_TRUE(driver.baudStep(2U, 460800U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "COM COM2 460800\r\n"));
    TEST_ASSERT_FALSE(driver.factoryResetStep(2U, {}, step));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testMaintenanceCommands);
    RUN_TEST(testGoldenProbe);
    RUN_TEST(testGoldenOutputPlan);
    RUN_TEST(testGoldenProbeMatch);
    RUN_TEST(testGoldenReplies);
    RUN_TEST(testGoldenCaps);
    RUN_TEST(testPromptDecoder);
    RUN_TEST(testBuildProbe);
    RUN_TEST(testMatchProbe);
    RUN_TEST(testRoverScript);
    RUN_TEST(testBuildOutput);
    RUN_TEST(testMatchReply);
    RUN_TEST(testCaps);
    return UNITY_END();
}
