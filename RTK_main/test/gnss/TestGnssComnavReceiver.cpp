// ComNav K803 驱动 host 测试：<OK/<ERROR 行解码、LOG VERSIONA ONCE 探测、NovAtel 风格脚本、应答匹配。
// 实验性驱动：按 ADR-0010 §2.2 与公开的 NovAtel 兼容资料断言，未经实物验证。
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "demux/decoders/UnicoreHash.hpp"
#include "gnss/drivers/ComnavReceiver.hpp"
#include "unity.h"

namespace {

bool text_equals(std::span<const uint8_t> data, const char *text)
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

std::span<const uint8_t> bytes_of(const std::string &text)
{
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
    TEST_ASSERT_TRUE(gnss::comnav_receiver().match_probe(demux::unicore_hash::kDecoder, bytes_of(frame), result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kK803), static_cast<int>(result.model));

    const std::string other = "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"OEM719\",\"1\",\"2\"*12345678\r\n";
    TEST_ASSERT_FALSE(gnss::comnav_receiver().match_probe(demux::unicore_hash::kDecoder, bytes_of(other), result));
}

void test_rover_script()
{
    gnss::RoverConfig cfg{};
    cfg.rate_hz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    cfg.port_baud = 921600;

    const char *expected[] = {"UNLOGALL COM2\r\n",           "LOG COM2 GPGGA ONTIME 1\r\n",
                              "LOG COM2 GPRMC ONTIME 1\r\n", "LOG COM2 GPGSA ONTIME 1\r\n",
                              "LOG COM2 GPGSV ONTIME 1\r\n", "MODE ROVER\r\n",
                              "COM COM2 921600\r\n"};
    for (size_t i = 0; i < 6; ++i) {
        gnss::ConfigStep step{};
        TEST_ASSERT_TRUE(gnss::comnav_receiver().rover_step(i, gnss::Model::kK803, cfg, step));
        TEST_ASSERT_TRUE(text_equals(step.cmd.view(), expected[i]));
        TEST_ASSERT_TRUE(step.expect_reply);
        TEST_ASSERT_EQUAL_UINT32(0, step.switch_baud);
    }
    gnss::ConfigStep last{};
    TEST_ASSERT_TRUE(gnss::comnav_receiver().rover_step(6, gnss::Model::kK803, cfg, last));
    TEST_ASSERT_TRUE(text_equals(last.cmd.view(), expected[6]));
    TEST_ASSERT_FALSE(last.expect_reply);
    TEST_ASSERT_EQUAL_UINT32(921600, last.switch_baud);

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(gnss::comnav_receiver().rover_step(7, gnss::Model::kK803, cfg, none));

    // 20 Hz → 0.05；GSV 关闭后脚本少一步。
    cfg.rate_hz = 20;
    cfg.gsv = false;
    gnss::ConfigStep fast{};
    TEST_ASSERT_TRUE(gnss::comnav_receiver().rover_step(1, gnss::Model::kK803, cfg, fast));
    TEST_ASSERT_TRUE(text_equals(fast.cmd.view(), "LOG COM2 GPGGA ONTIME 0.05\r\n"));
    TEST_ASSERT_TRUE(gnss::comnav_receiver().rover_step(4, gnss::Model::kK803, cfg, fast));
    TEST_ASSERT_TRUE(text_equals(fast.cmd.view(), "MODE ROVER\r\n"));
}

void test_match_reply()
{
    const std::string ok = "<OK\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kAck), static_cast<int>(gnss::comnav_receiver().classify_reply(
                                                             gnss::comnav::kPromptDecoder, bytes_of(ok), {})));
    const std::string nak = "<ERROR\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNack), static_cast<int>(gnss::comnav_receiver().classify_reply(
                                                              gnss::comnav::kPromptDecoder, bytes_of(nak), {})));
    const std::string other = "<WARNING\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone), static_cast<int>(gnss::comnav_receiver().classify_reply(
                                                              gnss::comnav::kPromptDecoder, bytes_of(other), {})));
    // 其他解码器的帧不认。
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone), static_cast<int>(gnss::comnav_receiver().classify_reply(
                                                              demux::unicore_hash::kDecoder, bytes_of(ok), {})));
}

void test_caps()
{
    const uint32_t k803 = gnss::comnav_receiver().caps(gnss::Model::kK803);
    TEST_ASSERT_TRUE((k803 & gnss::cap::kRtcmIn) != 0);
    TEST_ASSERT_TRUE((k803 & gnss::cap::kBase) != 0);
    TEST_ASSERT_EQUAL_UINT32(0, k803 & gnss::cap::kIns);
    TEST_ASSERT_EQUAL_UINT32(0, k803 & gnss::cap::kHeading);
}

} // namespace

void setUp() {}

void tearDown() {}

// ---- 黄金向量比对（ADR-0012 P3a 第 3 步）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenComnav.inc"

static void test_golden_probe()
{
    TEST_ASSERT_TRUE(golden_check::probe_matches(gnss::comnav_receiver(), golden_Comnav::kProbe));
}

static void test_golden_rover_steps()
{
    const size_t bad =
        golden_check::steps_mismatch(gnss::comnav_receiver(), golden_Comnav::kRover, golden_Comnav::kSteps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_probe_match()
{
    const size_t bad = golden_check::probe_match_mismatch(gnss::comnav_receiver(), golden_Comnav::kProbeMatch, nullptr);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_replies()
{
    const size_t bad =
        golden_check::reply_mismatch(gnss::comnav_receiver(), golden_Comnav::kReply, &gnss::comnav::kPromptDecoder);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_caps()
{
    const size_t bad = golden_check::caps_mismatch(gnss::comnav_receiver(), golden_Comnav::kCaps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_probe);
    RUN_TEST(test_golden_rover_steps);
    RUN_TEST(test_golden_probe_match);
    RUN_TEST(test_golden_replies);
    RUN_TEST(test_golden_caps);
    RUN_TEST(test_prompt_decoder);
    RUN_TEST(test_build_probe);
    RUN_TEST(test_match_probe);
    RUN_TEST(test_rover_script);
    RUN_TEST(test_match_reply);
    RUN_TEST(test_caps);
    return UNITY_END();
}
