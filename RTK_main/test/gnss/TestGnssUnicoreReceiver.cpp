// Unicore 驱动 host 测试：$command 应答帧、VERSIONA 探测、rover 配置脚本、ACK/NAK 匹配（ADR-0010 §2.2）。
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/Demux.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "demux/StaticDemux.hpp"
#include "gnss/drivers/UnicoreReceiver.hpp"
#include "unity.h"

namespace {

constexpr uint32_t kTestBaud = 921600U;
constexpr unsigned kNibbleBits = 4U;
constexpr unsigned kNibbleMask = 0x0FU;

std::string with_checksum(const std::string &body)
{
    unsigned checksum = 0;
    for (const char c : body) {
        checksum ^= static_cast<unsigned char>(c);
    }
    static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string out = "$" + body + "*";
    out += kHex[(checksum >> kNibbleBits) & kNibbleMask];
    out += kHex[checksum & kNibbleMask];
    return out + "\r\n";
}

std::string command_reply(const std::string &echo, const std::string &text)
{
    return with_checksum("command," + echo + ",response: " + text);
}

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

/** 测试里把字符串当只读字节看；span 无法引用字符数据，必须做一次重解释（见下方 NOLINT）。 */
std::span<const uint8_t> bytes_of(std::string_view text)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) 只读视图，不写入
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

bool result_equals(const std::string &sentence, const char *expected)
{
    const std::span<const uint8_t> frame = bytes_of(sentence);
    size_t need = 0;
    return gnss::unicore::response_step(frame, need) == demux::Step::kFrame && text_equals(frame, expected);
}

// 证明 $command 应答解码器与 demux::nmea 同时挂载时互不抢帧（ADR-0010 §2.2 / G-A 任务第 3 条）。
class RecordingSink final : public demux::Sink {
public:
    static constexpr size_t kCapacity = 4;
    struct Frame {
        uint8_t index = 0;
        size_t len = 0;
        std::array<uint8_t, 512> data{};
    };

    std::array<Frame, kCapacity> frames{};
    size_t count = 0;

    void on_frame(const demux::Decoder &, uint8_t index, std::span<const uint8_t> frame) noexcept override
    {
        if (count >= kCapacity) {
            return;
        }
        frames[count].index = index;
        frames[count].len = frame.size();
        for (size_t i = 0; i < frame.size(); ++i) {
            frames[count].data[i] = frame[i];
        }
        ++count;
    }
};

} // namespace

void setUp() {}
void tearDown() {}

static void test_response_streaming()
{
    const std::string reply = command_reply("UNLOG COM2", "OK");
    const std::span<const uint8_t> frame = bytes_of(reply);

    size_t need = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::unicore::response_step(frame.first(10), need)));
    TEST_ASSERT_GREATER_THAN_UINT32(0, need);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kFrame),
                      static_cast<int>(gnss::unicore::response_step(frame, need)));
    TEST_ASSERT_TRUE(gnss::unicore::response_first_byte('$'));
    TEST_ASSERT_FALSE(gnss::unicore::response_first_byte('#'));
}

static void test_response_rejects_and_window()
{
    size_t need = 0;
    const std::string bad_checksum = "$command,UNLOG COM2,response: OK*00\r\n";
    const std::span<const uint8_t> bad = bytes_of(bad_checksum);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::unicore::response_step(bad, need)));

    const std::string binary = std::string("$command,UNLOG\x01") + "\r\n";
    const std::span<const uint8_t> bin = bytes_of(binary);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::unicore::response_step(bin, need)));

    std::string full = "$command,";
    full.append(gnss::unicore::kResponseMaxFrame, 'A'); // 无终止符：帧窗口上限由 demux 负责，这里只报“还要 1 字节”
    const std::span<const uint8_t> big = bytes_of(full);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::unicore::response_step(big, need)));
    TEST_ASSERT_EQUAL_UINT32(1, need);
}

static void test_build_probe()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_TRUE(gnss::unicore_receiver().build_probe(cmd));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "VERSIONA\r\n"));
}

static void test_match_probe()
{
    const std::string version =
        "#VERSIONA,96,GPS,FINE,2281,45803,0,0,18,10;\"UM982\",\"R4.10Build11233\",\"HRPT00-S10C-P\","
        "\"2310415000123\",\"R4.10.0\",\"ff\"*12345678";
    const std::span<const uint8_t> frame = bytes_of(version);
    gnss::ProbeResult result{};
    TEST_ASSERT_TRUE(gnss::unicore_receiver().match_probe(demux::unicore_hash::kDecoder, frame, result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kUm982), static_cast<int>(result.model));
    TEST_ASSERT_EQUAL_STRING("R4.10Build11233", result.fw_version.data());

    const std::string other = "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"OEM719\",\"1\",\"2\"*12345678";
    const std::span<const uint8_t> other_frame = bytes_of(other);
    TEST_ASSERT_FALSE(gnss::unicore_receiver().match_probe(demux::unicore_hash::kDecoder, other_frame, result));
}

static void test_rover_script()
{
    gnss::RoverConfig cfg{};
    cfg.rate_hz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    cfg.port_baud = kTestBaud;

    // P3b 之后基础脚本只剩三步：语句输出改由 build_output 逐项下发（ADR-0012 §2.4）。
    const std::array<std::string_view, 3> expected{"UNLOG COM2\r\n", "MODE ROVER\r\n", "CONFIG COM2 921600\r\n"};
    for (size_t i = 0; i < 2; ++i) {
        gnss::ConfigStep step{};
        TEST_ASSERT_TRUE(gnss::unicore_receiver().rover_step(i, gnss::Model::kUm982, cfg, step));
        TEST_ASSERT_TRUE(text_equals(step.cmd.view(), expected[i]));
        TEST_ASSERT_TRUE(step.expect_reply);
        TEST_ASSERT_EQUAL_UINT32(0, step.switch_baud);
    }
    gnss::ConfigStep last{};
    TEST_ASSERT_TRUE(gnss::unicore_receiver().rover_step(2, gnss::Model::kUm982, cfg, last));
    TEST_ASSERT_TRUE(text_equals(last.cmd.view(), expected[2]));
    TEST_ASSERT_FALSE(last.expect_reply);
    TEST_ASSERT_EQUAL_UINT32(kTestBaud, last.switch_baud);

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(gnss::unicore_receiver().rover_step(3, gnss::Model::kUm982, cfg, none));

    // 不改波特率时脚本只有两步。
    cfg.port_baud = 0;
    TEST_ASSERT_FALSE(gnss::unicore_receiver().rover_step(2, gnss::Model::kUm982, cfg, none));
}

/** build_output：周期按 10 ms 粒度下发，0 表示关闭，HEADINGA 受能力位约束。 */
static void test_build_output()
{
    const gnss::Receiver &r = gnss::unicore_receiver();
    gnss::OutputPlan plan{};
    plan.port = 2;
    gnss::CmdBuf cmd;

    // 未定义的语句与非法下标一律拒绝。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kNone, 1000, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kInvalidArg),
        static_cast<int>(r.build_output(static_cast<gnss::OutputMsg>(200), 1000, gnss::Model::kUm982, plan, cmd)));

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 1000, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "GPGGA COM2 1\r\n"));

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 50, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "GPGGA COM2 0.05\r\n"));

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 200, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "GPGGA COM2 0.2\r\n"));

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 0, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "UNLOG COM2 GPGGA\r\n"));

    // 10 ms 粒度以外的周期无法用小数秒表示，按参数错误处理。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kGga, 25, gnss::Model::kUm982, plan, cmd)));

    // HEADINGA 只在双天线型号上可用。
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kNotSupported),
        static_cast<int>(r.build_output(gnss::OutputMsg::kHeading, 1000, gnss::Model::kUm980, plan, cmd)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kOk),
        static_cast<int>(r.build_output(gnss::OutputMsg::kHeading, 1000, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "HEADINGA COM2 1\r\n"));

    // 端口来自输出计划。
    plan.port = 1;
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.build_output(gnss::OutputMsg::kRmc, 200, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(text_equals(cmd.view(), "GPRMC COM1 0.2\r\n"));
}

static void test_match_reply()
{
    const std::string ok = command_reply("UNLOG COM2", "OK");
    const std::span<const uint8_t> ok_frame = bytes_of(ok);
    TEST_ASSERT_TRUE(result_equals(ok, ok.c_str()));
    const std::string sent_text = "UNLOG COM2\r\n";
    const std::span<const uint8_t> sent = bytes_of(sent_text);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kAck), static_cast<int>(gnss::unicore_receiver().classify_reply(
                                                             gnss::unicore::kResponseDecoder, ok_frame, sent)));

    const std::string nak = command_reply("UNLOG COM2", "ERROR");
    const std::span<const uint8_t> nak_frame = bytes_of(nak);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNack), static_cast<int>(gnss::unicore_receiver().classify_reply(
                                                              gnss::unicore::kResponseDecoder, nak_frame, sent)));

    const std::string other = command_reply("OTHER", "OK");
    const std::span<const uint8_t> other_frame = bytes_of(other);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone), static_cast<int>(gnss::unicore_receiver().classify_reply(
                                                              gnss::unicore::kResponseDecoder, other_frame, sent)));
}

static void test_coexists_with_nmea()
{
    const std::string gga = with_checksum("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    const std::string reply = command_reply("UNLOG COM2", "OK");
    const std::string stream = gga + reply;

    demux::StaticDemux<demux::nmea::kDecoder, gnss::unicore::kResponseDecoder> splitter;
    RecordingSink sink;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk), static_cast<int>(splitter.init(sink)));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk), static_cast<int>(splitter.feed(bytes_of(stream))));

    TEST_ASSERT_EQUAL_UINT32(2, sink.count);
    TEST_ASSERT_EQUAL_UINT8(0, sink.frames[0].index); // NMEA 归 nmea
    TEST_ASSERT_EQUAL_UINT8(1, sink.frames[1].index); // $command 归驱动应答解码器
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(gga.size()), static_cast<uint32_t>(sink.frames[0].len));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(reply.size()), static_cast<uint32_t>(sink.frames[1].len));
    TEST_ASSERT_EQUAL_UINT32(0, splitter.stats().unknown_bytes);
}

/** 按 GnssModule::init 的方式（NMEA、RTCM3 + 驱动 decoders）组分流器，把帧交给驱动判定。 */
class ReceiverSink final : public demux::Sink {
public:
    bool probe_hit = false;
    gnss::ProbeResult probe{};
    uint32_t acks = 0;
    uint32_t nmea = 0;

    void on_frame(const demux::Decoder &dec, uint8_t, std::span<const uint8_t> frame) noexcept override
    {
        if (&dec == &demux::nmea::kDecoder) {
            ++nmea;
        }
        gnss::ProbeResult r{};
        if (gnss::unicore_receiver().match_probe(dec, frame, r)) {
            probe_hit = true;
            probe = r;
        }
        static constexpr std::string_view kSent = "VERSIONA\r\n";
        const std::span<const uint8_t> sent = bytes_of(kSent);
        if (gnss::unicore_receiver().classify_reply(dec, frame, sent) == gnss::Ack::kAck) {
            ++acks;
        }
    }
};

/** 回归：UM980 实机抓包（2026-11-12，COM2@115200）经模块同款解码器表后必须命中探测与 ACK。 */
static void test_real_um980_capture_through_receiver_decoders()
{
    const std::string capture = "$devicename,COM2*64\r\n"
                                "$command,VERSIONA,response: OK*45\r\n"
                                "#VERSIONA,0,GPS,UNKNOWN,0,0,0,0,0,249;\"UM980\",\"R4.10Build11833\",\"HRPT00-S10C-P\","
                                "\"2310415000001-MD22A2244202679\",\"ff3b7c6f3c5fd90b\",\"2023/11/24\"*3f55fb4d\r\n"
                                "$GNRMC,,V,,,,,,,,,,N,V*37\r\n";

    std::array<const demux::Decoder *, demux::kMaxDecoders> decoders{};
    size_t count = 0;
    decoders[count++] = &demux::nmea::kDecoder;
    decoders[count++] = &demux::rtcm3::kDecoder;
    for (const demux::Decoder *dec : gnss::unicore_receiver().decoders()) {
        decoders[count++] = dec;
    }
    static std::array<uint8_t, 4096> buf{};
    demux::Demux splitter;
    ReceiverSink sink;
    TEST_ASSERT_EQUAL(
        static_cast<int>(demux::Error::kOk),
        static_cast<int>(splitter.init(std::span<const demux::Decoder *const>{decoders.data(), count}, buf, sink)));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk), static_cast<int>(splitter.feed(bytes_of(capture))));

    TEST_ASSERT_TRUE(sink.probe_hit);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kUm980), static_cast<int>(sink.probe.model));
    TEST_ASSERT_EQUAL_STRING("R4.10Build11833", sink.probe.fw_version.data());
    TEST_ASSERT_EQUAL_UINT32(1, sink.acks);
    TEST_ASSERT_EQUAL_UINT32(1, sink.nmea); // 只有 $GNRMC；小写 $devicename/$command 不归 NMEA
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(sizeof("$devicename,COM2*64\r\n") - 1U),
                             splitter.stats().unknown_bytes);
}

void test_caps()
{
    TEST_ASSERT_BITS(gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kHeading | gnss::cap::kRate20Hz,
                     gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kHeading | gnss::cap::kRate20Hz,
                     gnss::unicore_receiver().caps(gnss::Model::kUm982));
    TEST_ASSERT_BITS(gnss::cap::kIns, gnss::cap::kIns, gnss::unicore_receiver().caps(gnss::Model::kUm981));
    TEST_ASSERT_BITS(gnss::cap::kHeading, 0U, gnss::unicore_receiver().caps(gnss::Model::kUm980));
}

// ---- 黄金向量比对（ADR-0012 P3a/P3b）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenUnicore.inc"
#include "PlanCheck.hpp"

static void test_golden_probe()
{
    TEST_ASSERT_TRUE(golden_check::probe_matches(gnss::unicore_receiver(), golden_Unicore::kProbe));
}

/** P3b：基础脚本 + build_output 逐项下发，合起来的命令集合必须与基线脚本一致。 */
static void test_golden_output_plan()
{
    const size_t bad =
        plan_check::plan_mismatch(gnss::unicore_receiver(), golden_Unicore::kRover, golden_Unicore::kSteps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_probe_match()
{
    const size_t bad = golden_check::probe_match_mismatch(gnss::unicore_receiver(), golden_Unicore::kProbeMatch,
                                                          &gnss::unicore::kResponseDecoder);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_replies()
{
    const size_t bad = golden_check::reply_mismatch(gnss::unicore_receiver(), golden_Unicore::kReply,
                                                    &gnss::unicore::kResponseDecoder);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_caps()
{
    const size_t bad = golden_check::caps_mismatch(gnss::unicore_receiver(), golden_Unicore::kCaps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_probe);
    RUN_TEST(test_golden_output_plan);
    RUN_TEST(test_golden_probe_match);
    RUN_TEST(test_golden_replies);
    RUN_TEST(test_golden_caps);
    RUN_TEST(test_response_streaming);
    RUN_TEST(test_response_rejects_and_window);
    RUN_TEST(test_build_probe);
    RUN_TEST(test_match_probe);
    RUN_TEST(test_rover_script);
    RUN_TEST(test_build_output);
    RUN_TEST(test_match_reply);
    RUN_TEST(test_coexists_with_nmea);
    RUN_TEST(test_real_um980_capture_through_receiver_decoders);
    RUN_TEST(test_caps);
    return UNITY_END();
}
