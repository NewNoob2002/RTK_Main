// Unicore 驱动 host 测试：$command 应答帧、VERSIONA 探测、rover 配置脚本、ACK/NAK 匹配（ADR-0010 §2.2）。
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/Demux.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "demux/StaticDemux.hpp"
#include "gnss/Controller.hpp"
#include "gnss/drivers/UnicoreReceiver.hpp"
#include "../fixtures/Cm680Version.hpp"
#include "unity.h"
#include "PppSequence.hpp"

// 驱动层次约束（ADR-0012 §2.8、CPP-011）：家族基类不可直接实例化；型号只实例化平凡析构的 final 叶子类。
static_assert(!std::is_constructible_v<gnss::UnicoreReceiver>);
static_assert(std::is_final_v<gnss::Um980Receiver>);
static_assert(std::is_final_v<gnss::Um982Receiver>);
static_assert(std::is_trivially_destructible_v<gnss::Um980Receiver>);
static_assert(std::is_trivially_destructible_v<gnss::Um982Receiver>);

namespace {
// 驱动访问器已按 ADR-0013 §2.3 删除；测试经注册表取基准叶子实例。
const gnss::Receiver &unicoreDrv() noexcept
{
    return *gnss::receiverFor(gnss::Family::kUnicore);
}
const gnss::Receiver &um982Drv() noexcept
{
    return gnss::receiverFor(gnss::Family::kUnicore)->forModel(gnss::Model::kUm982);
}

constexpr uint32_t kTestBaud = 921600U;
constexpr unsigned kNibbleBits = 4U;
constexpr unsigned kNibbleMask = 0x0FU;

std::string withChecksum(const std::string &body)
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

std::string commandReply(const std::string &echo, const std::string &text)
{
    return withChecksum("command," + echo + ",response: " + text);
}

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

/** 测试里把字符串当只读字节看；span 无法引用字符数据，必须做一次重解释（见下方 NOLINT）。 */
std::span<const uint8_t> bytesOf(std::string_view text)
{
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

bool resultEquals(const std::string &sentence, const char *expected)
{
    const std::span<const uint8_t> frame = bytesOf(sentence);
    size_t need = 0;
    return gnss::unicore::responseStep(frame, need) == demux::Step::kFrame &&
           textEquals(frame, expected);
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

    void onFrame(const demux::Decoder &, uint8_t index,
                 std::span<const uint8_t> frame) noexcept override
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

static void testResponseStreaming()
{
    const std::string reply = commandReply("UNLOG COM2", "OK");
    const std::span<const uint8_t> frame = bytesOf(reply);

    size_t need = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::unicore::responseStep(frame.first(10), need)));
    TEST_ASSERT_GREATER_THAN_UINT32(0, need);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kFrame),
                      static_cast<int>(gnss::unicore::responseStep(frame, need)));
    TEST_ASSERT_TRUE(gnss::unicore::responseFirstByte('$'));
    TEST_ASSERT_FALSE(gnss::unicore::responseFirstByte('#'));
}

static void testResponseRejectsAndWindow()
{
    size_t need = 0;
    const std::string badChecksum = "$command,UNLOG COM2,response: OK*00\r\n";
    const std::span<const uint8_t> bad = bytesOf(badChecksum);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::unicore::responseStep(bad, need)));

    const std::string binary = std::string("$command,UNLOG\x01") + "\r\n";
    const std::span<const uint8_t> bin = bytesOf(binary);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::unicore::responseStep(bin, need)));

    std::string full = "$command,";
    full.append(gnss::unicore::kResponseMaxFrame,
                'A'); // 无终止符：帧窗口上限由 demux 负责，这里只报“还要 1 字节”
    const std::span<const uint8_t> big = bytesOf(full);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::unicore::responseStep(big, need)));
    TEST_ASSERT_EQUAL_UINT32(1, need);
}

static void testBuildProbe()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_TRUE(unicoreDrv().buildProbe(cmd));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "VERSIONA\r\n"));
}

static void testMatchProbe()
{
    const std::string version =
        "#VERSIONA,96,GPS,FINE,2281,45803,0,0,18,10;\"UM982\",\"R4.10Build11233\",\"HRPT00-S10C-P\","
        "\"2310415000123\",\"R4.10.0\",\"ff\"*12345678";
    const std::span<const uint8_t> frame = bytesOf(version);
    gnss::ProbeResult result{};
    TEST_ASSERT_TRUE(unicoreDrv().matchProbe(demux::unicore_hash::kDecoder, frame, result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kUm982), static_cast<int>(result.model));
    TEST_ASSERT_EQUAL_STRING("R4.10Build11233", result.fwVersion.data());

    const std::string other = "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"OEM719\",\"1\",\"2\"*12345678";
    const std::span<const uint8_t> otherFrame = bytesOf(other);
    TEST_ASSERT_FALSE(unicoreDrv().matchProbe(demux::unicore_hash::kDecoder, otherFrame, result));
}

/** @brief 实板 CM680 版本帧须精确识别，保留版本文本；单线程测试。 */
static void testMatchCm680()
{
    using gnss::Model;
    const auto &decoder = demux::unicore_hash::kDecoder;
    const auto &driver = unicoreDrv();
    gnss::ProbeResult result{};
    TEST_ASSERT_TRUE(
        driver.matchProbe(decoder, bytesOf(gnss_test::kCm680VersionFrames.front()), result));
    TEST_ASSERT_EQUAL(static_cast<int>(Model::kCm680), static_cast<int>(result.model));
    TEST_ASSERT_TRUE(std::string_view{result.fwVersion.data()}.starts_with("CM680_SDK.1.2.5a"));
    TEST_ASSERT_EQUAL_UINT32(driver.caps(Model::kUm980), driver.caps(Model::kCm680));
    TEST_ASSERT_EQUAL_PTR(&driver.forModel(Model::kUm980), &driver.forModel(Model::kCm680));
    TEST_ASSERT_EQUAL_PTR(&driver, &um982Drv().forModel(Model::kCm680));

    for (const std::string_view model : {"CM681", "CMX", "CM680A"}) {
        std::string frame{gnss_test::kCm680VersionFrames.front()};
        // matchProbe 的输入已由分流器判定，直接替换首个带引号字段即可测试型号匹配。
        frame.replace(frame.find("\"CM680\"") + 1U, std::string_view{"CM680"}.size(), model);
        TEST_ASSERT_FALSE(driver.matchProbe(decoder, bytesOf(frame), result));
    }
}

static void testRoverScript()
{
    gnss::RoverConfig cfg{};
    cfg.rateHz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    cfg.portBaud = kTestBaud;

    // 输出计划拆分后基础脚本只剩三步：语句输出改由 buildOutput 逐项下发（ADR-0012 §2.4）。
    const std::array<std::string_view, 3> expected{"UNLOG COM2\r\n", "MODE ROVER\r\n",
                                                   "CONFIG COM2 921600\r\n"};
    for (size_t i = 0; i < 2; ++i) {
        gnss::ConfigStep step{};
        TEST_ASSERT_TRUE(unicoreDrv().roverStep(i, gnss::Model::kUm982, cfg, step));
        TEST_ASSERT_TRUE(textEquals(step.cmd.view(), expected[i]));
        TEST_ASSERT_TRUE(step.expectReply);
        TEST_ASSERT_EQUAL_UINT32(0, step.switchBaud);
    }
    gnss::ConfigStep last{};
    TEST_ASSERT_TRUE(unicoreDrv().roverStep(2, gnss::Model::kUm982, cfg, last));
    TEST_ASSERT_TRUE(textEquals(last.cmd.view(), expected[2]));
    TEST_ASSERT_FALSE(last.expectReply);
    TEST_ASSERT_EQUAL_UINT32(kTestBaud, last.switchBaud);

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(unicoreDrv().roverStep(3, gnss::Model::kUm982, cfg, none));

    // 不改波特率时脚本只有两步。
    cfg.portBaud = 0;
    TEST_ASSERT_FALSE(unicoreDrv().roverStep(2, gnss::Model::kUm982, cfg, none));
}

/** @brief 验证 COM1 的清空命令与 CM680 的输出能力；单线程主机测试。 */
static void testCm680Configuration()
{
    using gnss::Model;
    using gnss::OutputMsg;
    const auto &driver = unicoreDrv();
    const gnss::RoverConfig cfg{.port = 1U};
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(driver.roverStep(0U, Model::kCm680, cfg, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "UNLOGALL COM1\r\n"));
    TEST_ASSERT_TRUE(step.expectReply);
    TEST_ASSERT_EQUAL_UINT32(0U, step.switchBaud);
    TEST_ASSERT_TRUE(driver.roverStep(0U, Model::kUm980, cfg, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "UNLOG COM1\r\n"));

    constexpr uint32_t kSatsInfoBit = 1U << static_cast<uint32_t>(OutputMsg::kSatsInfo);
    constexpr uint32_t kGstBit = 1U << static_cast<uint32_t>(OutputMsg::kGst);
    constexpr uint32_t kGsvBit = 1U << static_cast<uint32_t>(OutputMsg::kGsv);
    TEST_ASSERT_EQUAL_UINT32(driver.outputMask(Model::kUm980) & ~kSatsInfoBit,
                             driver.outputMask(Model::kCm680));
    TEST_ASSERT_BITS(kSatsInfoBit | kGstBit | kGsvBit, kGstBit | kGsvBit,
                     driver.outputMask(Model::kCm680));
    const gnss::OutputPlan plan{.port = 1U};
    for (const uint32_t periodMs : {0U, 1000U}) {
        gnss::CmdBuf cmd{};
        TEST_ASSERT_EQUAL(
            fw::Status::kNotSupported,
            driver.buildOutput(OutputMsg::kSatsInfo, periodMs, Model::kCm680, plan, cmd));
        TEST_ASSERT_TRUE(cmd.view().empty());
    }
}

/** @brief 验证 CM680 只钳制 GSA 亚秒周期；单线程主机测试。 */
static void testCm680GsaPeriod()
{
    using gnss::Model;
    using gnss::OutputMsg;
    struct Case {
        Model model;
        OutputMsg msg;
        uint32_t periodMs;
        std::string_view expected;
    };
    constexpr std::array kCases{
        Case{Model::kCm680, OutputMsg::kGsa, 100U, "GPGSA COM1 1\r\n"},
        Case{Model::kCm680, OutputMsg::kGsa, 50U, "GPGSA COM1 1\r\n"},
        Case{Model::kUm980, OutputMsg::kGsa, 100U, "GPGSA COM1 0.1\r\n"},
        Case{Model::kCm680, OutputMsg::kGga, 100U, "GPGGA COM1 0.1\r\n"},
        Case{Model::kCm680, OutputMsg::kGsa, 0U, "UNLOG COM1 GPGSA\r\n"},
        Case{Model::kCm680, OutputMsg::kGsa, 1000U, "GPGSA COM1 1\r\n"},
        Case{Model::kCm680, OutputMsg::kGsa, 2000U, "GPGSA COM1 2\r\n"},
    };
    const gnss::OutputPlan plan{.port = 1U};
    for (const Case &test : kCases) {
        gnss::CmdBuf cmd{};
        TEST_ASSERT_EQUAL(fw::Status::kOk,
                          unicoreDrv().buildOutput(test.msg, test.periodMs, test.model, plan, cmd));
        TEST_ASSERT_TRUE(textEquals(cmd.view(), test.expected));
    }
}

/** buildOutput：周期按 10 ms 粒度下发，0 表示关闭，HEADINGA 受能力位约束。 */
static void testBuildOutput()
{
    const gnss::Receiver &r = unicoreDrv();
    gnss::OutputPlan plan{};
    plan.port = 2;
    gnss::CmdBuf cmd;

    // 未定义的语句与非法下标一律拒绝。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kNone, 1000,
                                                     gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(r.buildOutput(static_cast<gnss::OutputMsg>(200), 1000,
                                                     gnss::Model::kUm982, plan, cmd)));

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 1000,
                                                     gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "GPGGA COM2 1\r\n"));

    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kOk),
        static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 50, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "GPGGA COM2 0.05\r\n"));

    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 200,
                                                     gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "GPGGA COM2 0.2\r\n"));

    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kOk),
        static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 0, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "UNLOG COM2 GPGGA\r\n"));

    // 10 ms 粒度以外的周期无法用小数秒表示，按参数错误处理。
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kInvalidArg),
        static_cast<int>(r.buildOutput(gnss::OutputMsg::kGga, 25, gnss::Model::kUm982, plan, cmd)));

    // HEADINGA 只有双天线型号的驱动能下发：家族驱动即使拿到 kUm982 也按 UM980 处理（ADR-0012 §2.8）。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kHeading, 1000,
                                                     gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_BITS(static_cast<uint32_t>(1U << 2), 0U, r.outputMask(gnss::Model::kUm982));
    const gnss::Receiver &dual = r.forModel(gnss::Model::kUm982);
    TEST_ASSERT_BITS(static_cast<uint32_t>(1U << 2), static_cast<uint32_t>(1U << 2),
                     dual.outputMask(gnss::Model::kUm982));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(dual.buildOutput(gnss::OutputMsg::kHeading, 1000,
                                                        gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "HEADINGA COM2 1\r\n"));

    // 端口来自输出计划。
    plan.port = 1;
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kRmc, 200,
                                                     gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_TRUE(textEquals(cmd.view(), "GPRMC COM1 0.2\r\n"));
}

static void testMatchReply()
{
    const std::string ok = commandReply("UNLOG COM2", "OK");
    const std::span<const uint8_t> okFrame = bytesOf(ok);
    TEST_ASSERT_TRUE(resultEquals(ok, ok.c_str()));
    const std::string sentText = "UNLOG COM2\r\n";
    const std::span<const uint8_t> sent = bytesOf(sentText);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kAck),
                      static_cast<int>(unicoreDrv().classifyReply(gnss::unicore::kResponseDecoder,
                                                                  okFrame, sent)));

    const std::string nak = commandReply("UNLOG COM2", "ERROR");
    const std::span<const uint8_t> nakFrame = bytesOf(nak);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNack),
                      static_cast<int>(unicoreDrv().classifyReply(gnss::unicore::kResponseDecoder,
                                                                  nakFrame, sent)));

    const std::string other = commandReply("OTHER", "OK");
    const std::span<const uint8_t> otherFrame = bytesOf(other);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(unicoreDrv().classifyReply(gnss::unicore::kResponseDecoder,
                                                                  otherFrame, sent)));
}

static void testCoexistsWithNmea()
{
    const std::string gga =
        withChecksum("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    const std::string reply = commandReply("UNLOG COM2", "OK");
    const std::string stream = gga + reply;

    demux::StaticDemux<demux::nmea::kDecoder, gnss::unicore::kResponseDecoder> splitter;
    RecordingSink sink;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk), static_cast<int>(splitter.init(sink)));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                      static_cast<int>(splitter.feed(bytesOf(stream))));

    TEST_ASSERT_EQUAL_UINT32(2, sink.count);
    TEST_ASSERT_EQUAL_UINT8(0, sink.frames[0].index); // NMEA 归 nmea
    TEST_ASSERT_EQUAL_UINT8(1, sink.frames[1].index); // $command 归驱动应答解码器
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(gga.size()),
                             static_cast<uint32_t>(sink.frames[0].len));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(reply.size()),
                             static_cast<uint32_t>(sink.frames[1].len));
    TEST_ASSERT_EQUAL_UINT32(0, splitter.stats().unknownBytes);
}

/** 按 GnssModule::init 的方式（NMEA、RTCM3 + 驱动 decoders）组分流器，把帧交给驱动判定。 */
class ReceiverSink final : public demux::Sink {
public:
    bool probeHit = false;
    gnss::ProbeResult probe{};
    uint32_t acks = 0;
    uint32_t nmea = 0;

    void onFrame(const demux::Decoder &dec, uint8_t,
                 std::span<const uint8_t> frame) noexcept override
    {
        if (&dec == &demux::nmea::kDecoder) {
            ++nmea;
        }
        gnss::ProbeResult r{};
        if (unicoreDrv().matchProbe(dec, frame, r)) {
            probeHit = true;
            probe = r;
        }
        static constexpr std::string_view kSent = "VERSIONA\r\n";
        const std::span<const uint8_t> sent = bytesOf(kSent);
        if (unicoreDrv().classifyReply(dec, frame, sent) == gnss::Ack::kAck) {
            ++acks;
        }
    }
};

/** 回归：UM980 实机抓包（2026-11-12，COM2@115200）经模块同款解码器表后必须命中探测与 ACK。 */
static void testRealUm980CaptureThroughReceiverDecoders()
{
    const std::string capture =
        "$devicename,COM2*64\r\n"
        "$command,VERSIONA,response: OK*45\r\n"
        "#VERSIONA,0,GPS,UNKNOWN,0,0,0,0,0,249;\"UM980\",\"R4.10Build11833\",\"HRPT00-S10C-P\","
        "\"2310415000001-MD22A2244202679\",\"ff3b7c6f3c5fd90b\",\"2023/11/24\"*3f55fb4d\r\n"
        "$GNRMC,,V,,,,,,,,,,N,V*37\r\n";

    std::array<const demux::Decoder *, demux::kMaxDecoders> decoders{};
    size_t count = 0;
    decoders[count++] = &demux::nmea::kDecoder;
    decoders[count++] = &demux::rtcm3::kDecoder;
    for (const demux::Decoder *dec : unicoreDrv().decoders()) {
        decoders[count++] = dec;
    }
    static std::array<uint8_t, 4096> buf{};
    demux::Demux splitter;
    ReceiverSink sink;
    TEST_ASSERT_EQUAL(
        static_cast<int>(demux::Error::kOk),
        static_cast<int>(splitter.init(
            std::span<const demux::Decoder *const>{decoders.data(), count}, buf, sink)));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                      static_cast<int>(splitter.feed(bytesOf(capture))));

    TEST_ASSERT_TRUE(sink.probeHit);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kUm980), static_cast<int>(sink.probe.model));
    TEST_ASSERT_EQUAL_STRING("R4.10Build11833", sink.probe.fwVersion.data());
    TEST_ASSERT_EQUAL_UINT32(1, sink.acks);
    TEST_ASSERT_EQUAL_UINT32(1, sink.nmea); // 只有 $GNRMC；小写 $devicename/$command 不归 NMEA
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(sizeof("$devicename,COM2*64\r\n") - 1U),
                             splitter.stats().unknownBytes);
}

void testCaps()
{
    const gnss::Receiver &family = unicoreDrv();
    // 家族驱动描述 UM980/UM981 的能力；UM982 的 HEADING 能力在型号驱动上（ADR-0012 §2.8）。
    TEST_ASSERT_BITS(gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kRate20Hz,
                     gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kRate20Hz,
                     family.caps(gnss::Model::kUm982));
    TEST_ASSERT_BITS(gnss::cap::kHeading, 0U, family.caps(gnss::Model::kUm982));
    TEST_ASSERT_BITS(gnss::cap::kHeading, 0U, family.caps(gnss::Model::kUm980));
    TEST_ASSERT_BITS(gnss::cap::kIns, gnss::cap::kIns, family.caps(gnss::Model::kUm981));
    // 换到型号驱动后才有 HEADING。
    const gnss::Receiver &dual = family.forModel(gnss::Model::kUm982);
    TEST_ASSERT_BITS(
        gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kHeading | gnss::cap::kRate20Hz,
        gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kHeading | gnss::cap::kRate20Hz,
        dual.caps(gnss::Model::kUm982));
}

/** ADR-0012 §2.8：只有实现上真有差异的型号才换驱动实例。 */
void testForModelSplitsModels()
{
    const gnss::Receiver &family = unicoreDrv();
    TEST_ASSERT_TRUE(&family.forModel(gnss::Model::kUm980) == &family);
    TEST_ASSERT_TRUE(&family.forModel(gnss::Model::kUm981) == &family);
    TEST_ASSERT_TRUE(&family.forModel(gnss::Model::kUnknown) == &family);
    TEST_ASSERT_TRUE(&family.forModel(gnss::Model::kZedF9p) == &family);

    const gnss::Receiver &dual = family.forModel(gnss::Model::kUm982);
    TEST_ASSERT_TRUE(&dual == &um982Drv());
    TEST_ASSERT_TRUE(&dual.forModel(gnss::Model::kUm982) == &dual);
    TEST_ASSERT_TRUE(&dual.forModel(gnss::Model::kUm980) == &dual);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore), static_cast<int>(dual.family()));
    TEST_ASSERT_TRUE(dual.name() == std::string_view{"unicore-um982"});

    // 三款 UM98x 解同一个方言：解码器集合不变。
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(family.decoders().size()),
                             static_cast<uint32_t>(dual.decoders().size()));
}

/**
 * @brief 控制器探测命中后必须换成该型号的驱动（ADR-0012 §2.8）：
 *        UM982 → um982Drv() 且能力含 kHeading；UM980 → 家族驱动本身且无 kHeading。
 */
void testControllerDetectsModelDriver()
{
    gnss::RoverConfig cfg{};
    cfg.rateHz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    // 时序取短值：只关心“命中后换了哪个驱动”。
    constexpr uint32_t kPowerOffMs = 10;
    constexpr uint32_t kBootMs = 20;
    constexpr uint32_t kProbeTimeoutMs = 50;
    constexpr uint32_t kProbeRounds = 2;
    constexpr uint32_t kStepRetries = 1;
    constexpr uint32_t kSilenceMs = 100;
    constexpr uint32_t kRetryBackoffMs = 30;
    gnss::Timing timing{};
    timing.powerOffMs = kPowerOffMs;
    timing.bootMs = kBootMs;
    timing.probeTimeoutMs = kProbeTimeoutMs;
    timing.probeRounds = static_cast<uint8_t>(kProbeRounds);
    timing.stepRetries = static_cast<uint8_t>(kStepRetries);
    timing.silenceMs = kSilenceMs;
    timing.retryBackoffMs = kRetryBackoffMs;
    const std::array<const gnss::Receiver *, 1> receivers{&unicoreDrv()};
    const std::array<uint32_t, 1> bauds{115200};

    const std::array<std::string, 2> replies{
        std::string(
            "#VERSIONA,96,GPS,FINE,2281,45803,0,0,18,10;\"UM982\",\"R4.10Build11233\",\"HRPT00-S10C-P\","
            "\"2310415000123\",\"R4.10.0\",\"ff\"*12345678"),
        std::string(
            "#VERSIONA,96,GPS,FINE,2281,45803,0,0,18,10;\"UM980\",\"R4.10Build11833\",\"HRPT00-S10C-P\","
            "\"2310415000123\",\"R4.10.0\",\"ff\"*12345678"),
    };
    const std::array<const gnss::Receiver *, 2> expected{&um982Drv(), &unicoreDrv()};
    const std::array<uint32_t, 2> heading{static_cast<uint32_t>(gnss::cap::kHeading), 0U};

    for (size_t i = 0; i < replies.size(); ++i) {
        gnss::Controller controller;
        TEST_ASSERT_TRUE(controller.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, timing));
        uint32_t now = 0;
        controller.start(now);
        TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kPower),
                          static_cast<int>(controller.poll(now).kind));
        now += timing.powerOffMs;
        TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kPower),
                          static_cast<int>(controller.poll(now).kind));
        now += timing.bootMs;
        TEST_ASSERT_TRUE(textEquals(controller.poll(now).bytes, "VERSIONA\r\n"));
        TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kProbing),
                          static_cast<int>(controller.state()));

        controller.onFrame(demux::unicore_hash::kDecoder, bytesOf(replies[i]), now);
        TEST_ASSERT_TRUE(controller.detected().receiver == expected[i]);
        // 能力在进入配置阶段时定格：caps 由 detected_.receiver 给出。
        ++now;
        TEST_ASSERT_TRUE(textEquals(controller.poll(now).bytes, "UNLOG COM2\r\n"));
        TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kConfiguring),
                          static_cast<int>(controller.state()));
        TEST_ASSERT_EQUAL_UINT32(heading[i], controller.detected().caps & gnss::cap::kHeading);
    }
}

// ---- 黄金向量比对（ADR-0012 §2.4）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenUnicore.inc"
#include "PlanCheck.hpp"

static void testGoldenProbe()
{
    TEST_ASSERT_TRUE(golden_check::probeMatches(unicoreDrv(), golden_Unicore::kProbe));
}

/** 基础脚本 + buildOutput 逐项下发，合起来的命令集合必须与基线脚本一致。 */
static void testGoldenOutputPlan()
{
    const size_t bad =
        plan_check::planMismatch(unicoreDrv(), golden_Unicore::kRover, golden_Unicore::kSteps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenProbeMatch()
{
    const size_t bad = golden_check::probeMatchMismatch(unicoreDrv(), golden_Unicore::kProbeMatch,
                                                        &gnss::unicore::kResponseDecoder);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenReplies()
{
    const size_t bad = golden_check::replyMismatch(unicoreDrv(), golden_Unicore::kReply,
                                                   &gnss::unicore::kResponseDecoder);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

static void testGoldenCaps()
{
    const size_t bad = golden_check::capsMismatch(unicoreDrv(), golden_Unicore::kCaps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch),
                             static_cast<uint32_t>(bad));
}

/**
 * @brief 在黄金向量的脚本步骤里找第一条 "HEADINGA …" 命令（基线脚本下发的原文）。
 * @retval bytes: 该命令的字节；空表示黄金向量里没有 HEADINGA。
 */
static std::span<const uint8_t> goldenHeadingCommand() noexcept
{
    constexpr std::string_view kName = "HEADINGA ";
    for (const golden::Step &step : golden_Unicore::kSteps) {
        const std::span<const uint8_t> bytes = step.bytes.view();
        if (bytes.size() <= kName.size()) {
            continue;
        }
        size_t same = 0;
        while ((same < kName.size()) && (bytes[same] == static_cast<uint8_t>(kName[same]))) {
            ++same;
        }
        if (same == kName.size()) {
            return bytes;
        }
    }
    return {};
}

/** ADR-0012 §2.8：UM982 驱动下发的 HEADINGA 必须与基线脚本的原文逐字节一致（端口取黄金用例里的那个）。 */
static void testGoldenHeadingThroughModelDriver()
{
    const std::span<const uint8_t> goldenHeading = goldenHeadingCommand();
    TEST_ASSERT_TRUE(!goldenHeading.empty());

    // 端口号从黄金命令里取（"HEADINGA COM1 …" → 1），这样比对不受用例首例端口影响。
    constexpr size_t kPortOffset = sizeof("HEADINGA COM") - 1U;
    TEST_ASSERT_TRUE(goldenHeading.size() > kPortOffset);
    const uint8_t portDigit = goldenHeading[kPortOffset];
    TEST_ASSERT_TRUE((portDigit >= static_cast<uint8_t>('0')) &&
                     (portDigit <= static_cast<uint8_t>('9')));

    gnss::OutputPlan plan{};
    plan.port = static_cast<uint8_t>(portDigit - static_cast<uint8_t>('0'));
    plan.rateHz = 1;
    gnss::CmdBuf cmd;
    const gnss::Receiver &dual = unicoreDrv().forModel(gnss::Model::kUm982);
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(dual.buildOutput(gnss::OutputMsg::kHeading, 1000,
                                                        gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(goldenHeading.size()),
                             static_cast<uint32_t>(cmd.view().size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(goldenHeading.data(), cmd.view().data(), goldenHeading.size());
}

/** @brief C2 一次性命令的字节级合约。 */
static void testMaintenanceCommands()
{
    const auto &driver = unicoreDrv();
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(driver.saveStep(step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "SAVECONFIG\r\n"));
    TEST_ASSERT_TRUE(driver.factoryResetStep(2U, {}, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "FRESET\r\n"));
    TEST_ASSERT_TRUE(driver.baudStep(2U, 460800U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "CONFIG COM2 460800\r\n"));
}

/** @brief C5a 模式切换与基站 RTCM 命令的字节级合约。 */
static void testWorkModeSteps()
{
    const auto &driver = unicoreDrv();
    gnss::ModeRequest req{};
    req.port = 2U;
    req.rtcmPeriodMs = {1000U, 0U, 10000U, 10000U, 1000U, 1000U, 1000U, 1000U, 0U, 0U, 0U, 0U};
    gnss::ConfigStep step{};

    // kRover：MODE ROVER + 11 条 UNLOG COM2 RTCM*（跳过 1230）。
    req.action = gnss::ModeAction::kRover;
    TEST_ASSERT_TRUE(driver.modeStep(req, 0U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "MODE ROVER\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 1U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "UNLOG COM2 RTCM1005\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 3U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "UNLOG COM2 RTCM1033\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 4U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "UNLOG COM2 RTCM1074\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 11U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "UNLOG COM2 RTCM1127\r\n"));
    TEST_ASSERT_FALSE(driver.modeStep(req, 12U, step));

    // kBasePrep：MODE ROVER + RTCM 开关配置。
    req.action = gnss::ModeAction::kBasePrep;
    TEST_ASSERT_TRUE(driver.modeStep(req, 0U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "MODE ROVER\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 1U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "RTCM1005 COM2 1\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 2U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "UNLOG COM2 RTCM1006\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 3U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "RTCM1033 COM2 10\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 4U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "RTCM1074 COM2 1\r\n"));

    // kBaseSurvey：MODE BASE TIME <s>。
    req.action = gnss::ModeAction::kBaseSurvey;
    req.surveySeconds = 25U;
    TEST_ASSERT_TRUE(driver.modeStep(req, 0U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "MODE BASE TIME 25\r\n"));
    TEST_ASSERT_FALSE(driver.modeStep(req, 1U, step));

    // kBaseFixed：定点经纬度与高度无浮点精确输出 + RTCM 开关。
    req.action = gnss::ModeAction::kBaseFixed;
    req.fixLatE9 = -31234567890LL;
    req.fixLonE9 = 121005000000LL;
    req.fixHeightMm = -1234;
    TEST_ASSERT_TRUE(driver.modeStep(req, 0U, step));
    TEST_ASSERT_TRUE(
        textEquals(step.cmd.view(), "MODE BASE -31.234567890 121.005000000 -1.234\r\n"));
    TEST_ASSERT_TRUE(driver.modeStep(req, 1U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "RTCM1005 COM2 1\r\n"));
}

/**
 * @brief 验证 PPP 开关的串口命令顺序、回显匹配与失败即停。
 * @note 单线程主机测试。
 */
void testPppSequence()
{
    const auto &driver = unicoreDrv().forModel(gnss::Model::kCm680);
    const auto *decoder = &gnss::unicore::kResponseDecoder;
    gnss::PppSequence sequence;
    std::span<const uint8_t> tx;
    uint32_t now = 100U;
    constexpr std::array<std::string_view, 5U> kEnable{
        "CONFIG SBAS DISABLE", "CONFIG SIGNALGROUP 2", "CONFIG PPP ENABLE E6-HAS",
        "CONFIG PPP CONVERGE 15 25", "SAVECONFIG"};
    const auto ackAll = [&](bool enabled, std::span<const std::string_view> steps) {
        sequence.start(enabled);
        for (const auto step : steps) {
            TEST_ASSERT_FALSE(sequence.poll(now++, tx).has_value());
            TEST_ASSERT_TRUE(textEquals(tx, std::string{step} + "\r\n"));
            const auto unrelated = commandReply("VERSIONA", "OK");
            sequence.onFrame(driver, *decoder, bytesOf(unrelated), now);
            TEST_ASSERT_FALSE(sequence.poll(now++, tx).has_value());
            TEST_ASSERT_TRUE(tx.empty());
            const auto response = commandReply(std::string{step}, "OK");
            sequence.onFrame(driver, *decoder, bytesOf(response), now);
        }
        TEST_ASSERT_TRUE(sequence.poll(now++, tx) == fw::Result::kOk);
        TEST_ASSERT_TRUE(tx.empty());
    };
    ackAll(true, kEnable);
    constexpr std::array<std::string_view, 2U> kDisable{"CONFIG PPP DISABLE", "SAVECONFIG"};
    ackAll(false, kDisable);

    sequence.start(true);
    TEST_ASSERT_FALSE(sequence.poll(now++, tx).has_value());
    const auto nack = commandReply("CONFIG SBAS DISABLE", "ERROR");
    sequence.onFrame(driver, *decoder, bytesOf(nack), now);
    TEST_ASSERT_TRUE(sequence.poll(now++, tx) == fw::Result::kIoError);
    TEST_ASSERT_TRUE(tx.empty() && !sequence.active());

    sequence.start(false);
    TEST_ASSERT_FALSE(sequence.poll(now, tx).has_value());
    now += gnss::PppSequence::kReplyMs;
    TEST_ASSERT_TRUE(sequence.poll(now, tx) == fw::Result::kTimeout);
    TEST_ASSERT_TRUE(tx.empty() && !sequence.active());
}

/** @brief 验证 PPP 型号支持判定（UM98x 支持，CM680 及其余型号不支持）。 */
static void testSupportsPpp()
{
    TEST_ASSERT_TRUE(gnss::supportsPpp(gnss::Model::kUm980));
    TEST_ASSERT_TRUE(gnss::supportsPpp(gnss::Model::kUm981));
    TEST_ASSERT_TRUE(gnss::supportsPpp(gnss::Model::kUm982));
    TEST_ASSERT_FALSE(gnss::supportsPpp(gnss::Model::kCm680));
    TEST_ASSERT_FALSE(gnss::supportsPpp(gnss::Model::kZedF9p));
    TEST_ASSERT_FALSE(gnss::supportsPpp(gnss::Model::kK803));
    TEST_ASSERT_FALSE(gnss::supportsPpp(gnss::Model::kUnknown));
}

/** @brief 验证 0x06 截止角/星座脚本顺序、NACK 中断、超时与非法截止角拒绝。 */
static void testSatMaskSequence()
{
    const auto &driver = unicoreDrv().forModel(gnss::Model::kCm680);
    const auto *decoder = &gnss::unicore::kResponseDecoder;
    gnss::SatMaskSequence sequence{};
    std::span<const uint8_t> tx{};
    uint32_t now = 200U;

    const gnss::SatMaskConfig invalidCfg{.cutoffDeg = 91U};
    TEST_ASSERT_FALSE(sequence.start(invalidCfg));
    TEST_ASSERT_FALSE(sequence.active());

    const gnss::SatMaskConfig cfg{
        .cutoffDeg = 12U, .gps = true, .glo = false, .bds = true, .gal = false, .sbas = true};
    constexpr std::array<std::string_view, 6U> kExpected{
        "MASK 12", "UNMASK GPS", "MASK GLO", "UNMASK BDS", "MASK GAL", "CONFIG SBAS ENABLE AUTO"};
    TEST_ASSERT_TRUE(sequence.start(cfg));
    for (const auto step : kExpected) {
        TEST_ASSERT_FALSE(sequence.poll(now++, tx).has_value());
        TEST_ASSERT_TRUE(textEquals(tx, std::string{step} + "\r\n"));
        const auto response = commandReply(std::string{step}, "OK");
        sequence.onFrame(driver, *decoder, bytesOf(response), now);
    }
    TEST_ASSERT_TRUE(sequence.poll(now++, tx) == fw::Result::kOk);
    TEST_ASSERT_TRUE(tx.empty() && !sequence.active());

    // NACK 在中途立即中止，不再发出后续步骤
    const gnss::SatMaskConfig disableAll{
        .cutoffDeg = 5U, .gps = false, .glo = false, .bds = false, .gal = false, .sbas = false};
    TEST_ASSERT_TRUE(sequence.start(disableAll));
    TEST_ASSERT_FALSE(sequence.poll(now++, tx).has_value());
    TEST_ASSERT_TRUE(textEquals(tx, "MASK 5\r\n"));
    const auto okStep0 = commandReply("MASK 5", "OK");
    sequence.onFrame(driver, *decoder, bytesOf(okStep0), now);
    TEST_ASSERT_FALSE(sequence.poll(now++, tx).has_value());
    TEST_ASSERT_TRUE(textEquals(tx, "MASK GPS\r\n"));
    const auto nackStep1 = commandReply("MASK GPS", "ERROR");
    sequence.onFrame(driver, *decoder, bytesOf(nackStep1), now);
    TEST_ASSERT_TRUE(sequence.poll(now++, tx) == fw::Result::kIoError);
    TEST_ASSERT_TRUE(tx.empty() && !sequence.active());

    // 单步超时中止
    TEST_ASSERT_TRUE(sequence.start(disableAll));
    TEST_ASSERT_FALSE(sequence.poll(now, tx).has_value());
    now += gnss::SatMaskSequence::kReplyMs;
    TEST_ASSERT_TRUE(sequence.poll(now, tx) == fw::Result::kTimeout);
    TEST_ASSERT_TRUE(tx.empty() && !sequence.active());
}

/** @brief 验证 CM680 的默认脚本与 460800 切速步不含 SAVECONFIG。 */
static void testCm680BaudScriptNoSave()
{
    const auto &driver = unicoreDrv().forModel(gnss::Model::kCm680);
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(driver.baudStep(1U, 460800U, step));
    TEST_ASSERT_TRUE(textEquals(step.cmd.view(), "CONFIG COM1 460800\r\n"));
    TEST_ASSERT_FALSE(step.expectReply);
    TEST_ASSERT_EQUAL_UINT32(460800U, step.switchBaud);

    gnss::RoverConfig cfg{};
    cfg.port = 1U;
    cfg.portBaud = 460800U;
    const std::array<std::string_view, 3U> expected{
        "UNLOGALL COM1\r\n",
        "MODE ROVER\r\n",
        "CONFIG COM1 460800\r\n",
    };
    for (size_t i = 0U; i < expected.size(); ++i) {
        TEST_ASSERT_TRUE(driver.roverStep(i, gnss::Model::kCm680, cfg, step));
        TEST_ASSERT_TRUE(textEquals(step.cmd.view(), expected[i]));
        TEST_ASSERT_FALSE(textEquals(step.cmd.view(), "SAVECONFIG\r\n"));
    }
    TEST_ASSERT_FALSE(driver.roverStep(expected.size(), gnss::Model::kCm680, cfg, step));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testPppSequence);
    RUN_TEST(testSupportsPpp);
    RUN_TEST(testSatMaskSequence);
    RUN_TEST(testMaintenanceCommands);
    RUN_TEST(testCm680BaudScriptNoSave);
    RUN_TEST(testWorkModeSteps);
    RUN_TEST(testGoldenProbe);
    RUN_TEST(testGoldenOutputPlan);
    RUN_TEST(testGoldenProbeMatch);
    RUN_TEST(testGoldenReplies);
    RUN_TEST(testGoldenCaps);
    RUN_TEST(testGoldenHeadingThroughModelDriver);
    RUN_TEST(testResponseStreaming);
    RUN_TEST(testResponseRejectsAndWindow);
    RUN_TEST(testBuildProbe);
    RUN_TEST(testMatchProbe);
    RUN_TEST(testMatchCm680);
    RUN_TEST(testRoverScript);
    RUN_TEST(testCm680Configuration);
    RUN_TEST(testCm680GsaPeriod);
    RUN_TEST(testBuildOutput);
    RUN_TEST(testMatchReply);
    RUN_TEST(testCoexistsWithNmea);
    RUN_TEST(testRealUm980CaptureThroughReceiverDecoders);
    RUN_TEST(testCaps);
    RUN_TEST(testForModelSplitsModels);
    RUN_TEST(testControllerDetectsModelDriver);
    return UNITY_END();
}
