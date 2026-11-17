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

// S1（ADR-0012 §2.8、CPP-011）：家族基类不可直接实例化；型号只实例化平凡析构的 final 叶子类。
static_assert(!std::is_constructible_v<gnss::ComnavReceiver>);
static_assert(std::is_final_v<gnss::K803Receiver>);
static_assert(std::is_trivially_destructible_v<gnss::K803Receiver>);

namespace {

constexpr uint32_t kTestBaud = 921600U;

bool text_equals(std::span<const uint8_t> data, std::string_view text)
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
std::span<const uint8_t> bytes_of(std::string_view text)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) 只读视图，不写入
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

void test_prompt_decoder()
{
    TEST_ASSERT_TRUE(gnss::comnav::prompt_first_byte('<'));
    TEST_ASSERT_FALSE(gnss::comnav::prompt_first_byte('$'));

    size_t need = 0;
    const std::string partial = "<OK";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::comnav::prompt_step(bytes_of(partial), need)));
    TEST_ASSERT_EQUAL_UINT32(1, need);

    const std::string cr = "<OK\r";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::comnav::prompt_step(bytes_of(cr), need)));

    const std::string ok = "<OK\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kFrame),
                      static_cast<int>(gnss::comnav::prompt_step(bytes_of(ok), need)));

    const std::string binary = std::string("<OK\x01") + "\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::comnav::prompt_step(bytes_of(binary), need)));
}

void test_build_probe()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_TRUE(gnss::comnav_receiver().build_probe(cmd));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "LOG VERSIONA ONCE\r\n"));
}

void test_match_probe()
{
    // VERSIONA 应答是 Unicore Hash 报文（'#' + crc32），不是 NMEA 的 '$'。
    const std::string frame = "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"K803\",\"1\",\"2\"*12345678\r\n";
    gnss::ProbeResult result{};
    TEST_ASSERT_TRUE(gnss::comnav_receiver().match_probe(demux::unicore_hash::kDecoder,
                                                         bytes_of(frame), result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kK803), static_cast<int>(result.model));

    const std::string other =
        "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"OEM719\",\"1\",\"2\"*12345678\r\n";
    TEST_ASSERT_FALSE(gnss::comnav_receiver().match_probe(demux::unicore_hash::kDecoder,
                                                          bytes_of(other), result));
}

void test_rover_script()
{
    gnss::RoverConfig cfg{};
    cfg.rate_hz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    cfg.port_baud = kTestBaud;

    // P3b 之后基础脚本只剩三步：语句输出改由 build_output 逐项下发（ADR-0012 §2.4）。
    const std::array<std::string_view, 3> expected{"UNLOGALL COM2\r\n", "MODE ROVER\r\n",
                                                   "COM COM2 921600\r\n"};
    for (size_t i = 0; i < 2; ++i) {
        gnss::ConfigStep step{};
        TEST_ASSERT_TRUE(gnss::comnav_receiver().rover_step(i, gnss::Model::kK803, cfg, step));
        TEST_ASSERT_TRUE(text_equals(step.cmd.view(), expected[i]));
        TEST_ASSERT_TRUE(step.expect_reply);
        TEST_ASSERT_EQUAL_UINT32(0, step.switch_baud);
    }
    gnss::ConfigStep last{};
    TEST_ASSERT_TRUE(gnss::comnav_receiver().rover_step(2, gnss::Model::kK803, cfg, last));
    TEST_ASSERT_TRUE(text_equals(last.cmd.view(), expected[2]));
    TEST_ASSERT_FALSE(last.expect_reply);
    TEST_ASSERT_EQUAL_UINT32(kTestBaud, last.switch_baud);

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(gnss::comnav_receiver().rover_step(3, gnss::Model::kK803, cfg, none));

    // 不改波特率时脚本只有两步。
    cfg.port_baud = 0;
    TEST_ASSERT_FALSE(gnss::comnav_receiver().rover_step(2, gnss::Model::kK803, cfg, none));
}

/** build_output：LOG/UNLOG 两种写法，周期 10 ms 粒度。 */
void test_build_output()
{
    const gnss::Receiver &r = gnss::comnav_receiver();
    gnss::OutputPlan plan{};
    plan.port = 2;
    gnss::CmdBuf cmd;

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 1000,
                                                      gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "LOG COM2 GPGGA ONTIME 1\r\n"));

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 200,
                                                      gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "LOG COM2 GPGGA ONTIME 0.2\r\n"));

    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kOk),
        static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 0, gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "UNLOG COM2 GPGGA\r\n"));

    // 该型号不下发 SATSINFOA/HEADINGA。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kSatsInfo, 1000,
                                                      gnss::Model::kK803, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kHeading, 1000,
                                                      gnss::Model::kK803, plan, cmd)));

    // 非 10 ms 倍数的周期无法表示。
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kInvalidArg),
        static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 33, gnss::Model::kK803, plan, cmd)));
}

void test_match_reply()
{
    const std::string ok = "<OK\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kAck),
                      static_cast<int>(gnss::comnav_receiver().classify_reply(
                          gnss::comnav::kPromptDecoder, bytes_of(ok), {})));
    const std::string nak = "<ERROR\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNack),
                      static_cast<int>(gnss::comnav_receiver().classify_reply(
                          gnss::comnav::kPromptDecoder, bytes_of(nak), {})));
    const std::string other = "<WARNING\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(gnss::comnav_receiver().classify_reply(
                          gnss::comnav::kPromptDecoder, bytes_of(other), {})));
    // 其他解码器的帧不认。
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(gnss::comnav_receiver().classify_reply(
                          demux::unicore_hash::kDecoder, bytes_of(ok), {})));
}

void test_caps()
{
    const uint32_t k803 = gnss::comnav_receiver().caps(gnss::Model::kK803);
    TEST_ASSERT_TRUE((k803 & gnss::cap::kRtcmIn) != 0);
    TEST_ASSERT_TRUE((k803 & gnss::cap::kBase) != 0);
    TEST_ASSERT_EQUAL_UINT32(0, k803 & gnss::cap::kIns);
    TEST_ASSERT_EQUAL_UINT32(0, k803 & gnss::cap::kHeading);
    // 单型号驱动：for_model 对任何型号都返回自身（ADR-0012 §2.8）。
    TEST_ASSERT_TRUE(&gnss::comnav_receiver().for_model(gnss::Model::kK803) ==
                     &gnss::comnav_receiver());
    TEST_ASSERT_TRUE(&gnss::comnav_receiver().for_model(gnss::Model::kUm982) ==
                     &gnss::comnav_receiver());
}

} // namespace

void setUp() {}

void tearDown() {}

// ---- 黄金向量比对（ADR-0012 P3a 第 3 步）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenComnav.inc"
#include "PlanCheck.hpp"

static void test_golden_probe()
{
    TEST_ASSERT_TRUE(golden_check::probe_matches(gnss::comnav_receiver(), golden_Comnav::kProbe));
}

/** P3b：基础脚本 + build_output 逐项下发，合起来的命令集合必须与基线脚本一致。 */
static void test_golden_output_plan()
{
    const size_t bad = plan_check::plan_mismatch(gnss::comnav_receiver(), golden_Comnav::kRover,
                                                 golden_Comnav::kSteps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void test_golden_probe_match()
{
    const size_t bad = golden_check::probe_match_mismatch(gnss::comnav_receiver(),
                                                          golden_Comnav::kProbeMatch, nullptr);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void test_golden_replies()
{
    const size_t bad = golden_check::reply_mismatch(gnss::comnav_receiver(), golden_Comnav::kReply,
                                                    &gnss::comnav::kPromptDecoder);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void test_golden_caps()
{
    const size_t bad = golden_check::caps_mismatch(gnss::comnav_receiver(), golden_Comnav::kCaps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_probe);
    RUN_TEST(test_golden_output_plan);
    RUN_TEST(test_golden_probe_match);
    RUN_TEST(test_golden_replies);
    RUN_TEST(test_golden_caps);
    RUN_TEST(test_prompt_decoder);
    RUN_TEST(test_build_probe);
    RUN_TEST(test_match_probe);
    RUN_TEST(test_rover_script);
    RUN_TEST(test_build_output);
    RUN_TEST(test_match_reply);
    RUN_TEST(test_caps);
    return UNITY_END();
}
