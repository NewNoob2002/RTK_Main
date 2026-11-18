// GNSS 配置读回 host 测试：$CONFIG 行解码、与 NMEA 共存、驱动读回命令与正文提取、控制器读回窗口（ADR-0012 §2.7）。
// 只用 host 可编的纯组件（gnss/streamDemux）与本地构造的协议字节，不碰硬件。
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "demux/Demux.hpp"
#include "demux/StaticDemux.hpp"
#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Ubx.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "gnss/Controller.hpp"
#include "gnss/NullReceiver.hpp"
#include "gnss/drivers/UnicoreReceiver.hpp"
#include "unity.h"

#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
#include "gnss/drivers/UbloxReceiver.hpp"
#endif
#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
#include "gnss/drivers/ComnavReceiver.hpp"
#endif

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
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
const gnss::Receiver &ubloxDrv() noexcept
{
    return *gnss::receiverFor(gnss::Family::kUblox);
}
#endif
#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
const gnss::Receiver &comnavDrv() noexcept
{
    return *gnss::receiverFor(gnss::Family::kComnav);
}
#endif

constexpr unsigned kNibbleBits = 4U;
constexpr unsigned kNibbleMask = 0x0FU;
constexpr uint32_t kByteMask = 0xFFU;
/** 截断用例的小缓冲容量。 */
constexpr size_t kSmallCapacity = 4U;
constexpr size_t kReadbackCapacity = 64U;
/** 读回窗口的期望值（与 Controller.cpp 的常量一致，ADR-0012 §2.7）。 */
constexpr uint32_t kIdleMs = 1000U;
constexpr uint32_t kTotalMs = 5000U;
/** 测试用快速时序（与真实 timing 无关，只为让状态机尽快跑完）。 */
constexpr uint32_t kFastMs = 1U;
constexpr uint32_t kProbeTimeoutMs = 5U;
constexpr uint32_t kSilenceMs = 100U;
constexpr uint32_t kRetryBackoffMs = 10U;
constexpr uint32_t kDriveGuard = 256U;
/** 发帧步长与等待 NMEA 空档前的时间。 */
constexpr uint32_t kLineStepMs = 400U;
constexpr uint32_t kLineGapMs = 700U;
constexpr uint32_t kNmeaOnlyMs = 500U;
constexpr uint32_t kTestBaud = 115200U;
constexpr size_t kRealisticConfigLines = 24U;
/** ACK-ACK 载荷里的 class/id 偏移（对应被确认帧的 cls/id）。 */
constexpr size_t kSentClassPos = 2U;
constexpr size_t kSentIdPos = 3U;
/** UBX 帧头与校验用的字节常量。 */
constexpr uint8_t kUbxSync1 = 0xB5U;
constexpr uint8_t kUbxSync2 = 0x62U;
constexpr uint8_t kUbxClsAck = 0x05U;
constexpr uint8_t kUbxIdAck = 0x01U;
constexpr uint8_t kUbxClsMonVer = 0x0AU;
constexpr uint8_t kUbxIdMonVer = 0x04U;
/** MON-VER 文本块宽度：标准 30 B、扩展 10 B。 */
constexpr size_t kUbxTextWidth = 30U;
constexpr size_t kUbxExtWidth = 10U;
/** 读回命令字节。 */
constexpr std::string_view kReadbackCommand = "CONFIG\r\n";
/** '$' 的字节值：拼校验和时要按需并入。 */
constexpr uint8_t kDollarSign = 0x24U;

/**
 * @brief 拼一条带 XOR 校验的 `$...` 行
 * @param body: '$' 与 '*' 之间的全部字符
 * @param withDollar: true 表示把 '$' 也计入校验（UM980 R4.10 的实测行为）
 * @retval line: 完整行，以 "\r\n" 结尾
 */
std::string checksumLine(const std::string &body, bool withDollar)
{
    unsigned checksum = withDollar ? static_cast<unsigned>(kDollarSign) : 0U;
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

/**
 * @brief 一条 `$CONFIG,<port>,<config>*hh\r\n` 读回行（标准校验和）
 * @param body: 端口与配置项（如 "COM1,GPGGA,1"）
 * @retval line: 完整读回行
 */
std::string configLine(const std::string &body)
{
    return checksumLine("CONFIG," + body, false);
}

/**
 * @brief 硬件抓包里那样的读回行：校验和把 '$' 也算进去
 *
 * 这决定了分流器归属：NMEA 解码器只认标准 XOR，会拒掉这种行，随后重扫把整段交给下一个候选
 * （即 UnicoreResp）。反过来，用标准校验和拼行时 NMEA 会把它当成一条合法句交付，UnicoreResp 收不到——
 * 见 testStandardChecksumConfigLineIsClaimedByNmea。
 * @param body: 端口与配置项（如 "COM1,GPGGA,1"）
 * @retval line: 完整读回行（'$' 计入校验）
 */
std::string capturedConfigLine(const std::string &body)
{
    return checksumLine("CONFIG," + body, true);
}

/**
 * @brief 一条 `$command,<echo>,response: <text>*hh\r\n` 命令应答
 * @param echo: 被回显的命令文本
 * @param text: 应答文本（OK / ERROR ...）
 * @retval line: 完整应答行
 */
std::string commandReply(const std::string &echo, const std::string &text)
{
    return checksumLine("command," + echo + ",response: " + text, false);
}

/** 测试里把字符串当只读字节看；span 无法引用字符数据，必须做一次重解释（见下方 NOLINT）。 */
std::span<const uint8_t> bytesOf(std::string_view text)
{
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

bool bytesEqual(std::span<const uint8_t> data, std::string_view text)
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

bool charsEqual(std::span<const char> data, std::string_view text)
{
    if (data.size() != text.size()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (data[i] != text[i]) {
            return false;
        }
    }
    return true;
}

/** 统计每个解码器交付的帧数，用于共存与分片不变量断言。 */
class CountingSink final : public demux::Sink {
public:
    /** 帧计数上限：两个解码器用例只需前两项，多留两位避免越界。 */
    static constexpr size_t kCounters = 4U;

    void onFrame(const demux::Decoder &, uint8_t index, std::span<const uint8_t>) noexcept override
    {
        if (index < counters.size()) {
            ++counters[index];
        }
        ++total;
    }
    void onReject(const demux::Decoder &, uint8_t, std::span<const uint8_t>,
                  demux::RejectCause cause, bool last) noexcept override
    {
        ++rejects;
        if (last) {
            ++rejectBursts;
        }
        lastCause = cause;
    }
    void onUnknown(std::span<const uint8_t> bytes) noexcept override
    {
        unknown += bytes.size();
    }

    std::array<uint32_t, kCounters> counters{};
    uint32_t total = 0U;
    uint32_t rejects = 0U;
    uint32_t rejectBursts = 0U;
    size_t unknown = 0U;
    demux::RejectCause lastCause = demux::RejectCause::kStep;
};

/** 典型 UM980 读回现场：$CONFIG 行与 GNGGA 交错，末尾跟一条命令应答。 */
std::string realisticRuntimeStream(size_t configLines)
{
    std::string stream;
    static constexpr std::array<std::string_view, 3U> kPeriods{"1", "2", "3"};
    for (size_t i = 0; i < configLines; ++i) {
        stream += capturedConfigLine("COM1,GPGGA," + std::string{kPeriods[i % kPeriods.size()]});
        stream += checksumLine(
            "GNGGA,114533.00,2232.12345,N,11356.54321,E,1,12,1.0,10.0,M,-1.0,M,,", false);
    }
    stream += commandReply("CONFIG", "OK");
    return stream;
}

// ---------------------------------------------------------------- 解码器

static void testConfigLineIsDecoded()
{
    const std::string line = configLine("COM1,GPGGA,1");
    size_t need = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kFrame),
                      static_cast<int>(gnss::unicore::responseStep(bytesOf(line), need)));
    std::array<char, 64U> why{};
    // 帧本身没问题时诊断文本就是 "not rejected"（与 explain_reject 的约定一致）。
    const size_t whyLen = gnss::unicore::explainResponse(bytesOf(line), why);
    TEST_ASSERT_TRUE(charsEqual(std::span<const char>(why.data(), whyLen), "not rejected"));
    TEST_ASSERT_TRUE(gnss::unicore::responseFirstByte(static_cast<uint8_t>('$')));
}

static void testDecoderStillTakesCommandReplies()
{
    const std::string reply = commandReply("UNLOG COM2", "OK");
    size_t need = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kFrame),
                      static_cast<int>(gnss::unicore::responseStep(bytesOf(reply), need)));
}

/**
 * @brief 把 '*' 后的两个校验字符改成一个两种变体都不接受的值
 * @param frame: 合法帧
 * @retval bad: 只改了校验和字段的副本
 */
std::string corruptChecksum(const std::string &frame)
{
    static constexpr std::array<std::string_view, 3U> kCandidates{"00", "FF", "A5"};
    std::string standard = checksumLine(frame.substr(1U, frame.find('*') - 1U), false);
    const std::string dollar = checksumLine(frame.substr(1U, frame.find('*') - 1U), true);
    const size_t tail = frame.find('*') + 1U;
    for (const std::string_view candidate : kCandidates) {
        if ((standard.substr(tail, 2U) == candidate) || (dollar.substr(tail, 2U) == candidate)) {
            continue;
        }
        std::string bad = standard;
        bad.replace(tail, 2U, candidate);
        return bad;
    }
    return standard; // 不可达（三个候选不可能与被排除的两个值同时重复）
}

static void testDecoderRejectsBadConfigFrames()
{
    size_t need = 0;
    // 校验和错：两种接受的变体都不匹配。
    const std::string bad = corruptChecksum(configLine("COM1,GPGGA,1"));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::unicore::responseStep(bytesOf(bad), need)));

    // 两个前缀都不成立。
    const std::string wrong = checksumLine("CONFIX,COM1,GPGGA,1", false);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::unicore::responseStep(bytesOf(wrong), need)));

    // 两种前缀都还可能成立时只报“还需要更多字节”。
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kNeedMore),
                      static_cast<int>(gnss::unicore::responseStep(bytesOf("$CONF"), need)));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, need);

    // 正文里出现 '$' 一律拒绝（与 $command 同一套规则）。
    const std::string inner = checksumLine("CONFIG,COM1,$GPGGA,1", false);
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Step::kReject),
                      static_cast<int>(gnss::unicore::responseStep(bytesOf(inner), need)));
}

/**
 * @brief 超过单帧上限的读回行不得成帧（长度上限由分流器容量强制，见 Demux::Stats::overflows）
 */
static void testOversizedConfigLineIsDropped()
{
    std::string body = "CONFIG,";
    body.append(gnss::unicore::kResponseMaxFrame, '1');
    const std::string line = checksumLine(body, true);
    demux::StaticDemux<demux::nmea::kDecoder, gnss::unicore::kResponseDecoder> demuxEngine;
    CountingSink sink;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                      static_cast<int>(demuxEngine.init(sink)));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                      static_cast<int>(demuxEngine.feed(bytesOf(line))));
    TEST_ASSERT_EQUAL_UINT32(0U, sink.counters[1]);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, demuxEngine.stats().overflows);
}

static void testConfigLinesCoexistWithNmea()
{
    const std::string stream = realisticRuntimeStream(kRealisticConfigLines);
    demux::StaticDemux<demux::nmea::kDecoder, gnss::unicore::kResponseDecoder> oneShot;
    CountingSink whole;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk), static_cast<int>(oneShot.init(whole)));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                      static_cast<int>(oneShot.feed(bytesOf(stream))));
    TEST_ASSERT_EQUAL_UINT32(kRealisticConfigLines, whole.counters[0]); // GNGGA 归 nmea
    TEST_ASSERT_EQUAL_UINT32(kRealisticConfigLines + 1U,
                             whole.counters[1]); // $CONFIG 行 + 命令应答归 UnicoreResp
    // 每条 $CONFIG 行与 $command 应答都先被 NMEA 拒一次（'$' 起点优先给表内靠前的候选），
    // 重扫后才落到 UnicoreResp——这正是本卡要保证的接手路径，不是丢字节。
    TEST_ASSERT_EQUAL_UINT32(kRealisticConfigLines + 1U, whole.rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(whole.unknown));
    TEST_ASSERT_EQUAL_UINT32((2U * kRealisticConfigLines) + 1U, whole.total);

    // 逐字节喂：每个切点都必须给出同样的结果（解码器不得依赖一次拿到整帧）。
    demux::StaticDemux<demux::nmea::kDecoder, gnss::unicore::kResponseDecoder> dribble;
    CountingSink split;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk), static_cast<int>(dribble.init(split)));
    const std::span<const uint8_t> all = bytesOf(stream);
    for (size_t i = 0; i < all.size(); ++i) {
        TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                          static_cast<int>(dribble.feed(all.subspan(i, 1U))));
    }
    TEST_ASSERT_EQUAL_UINT32(whole.counters[0], split.counters[0]);
    TEST_ASSERT_EQUAL_UINT32(whole.counters[1], split.counters[1]);
    TEST_ASSERT_EQUAL_UINT32(whole.rejects, split.rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(split.unknown));
}

/**
 * @brief 记录“标准校验和的 $CONFIG 行会被 NMEA 当成合法句”的事实
 *
 * 分流器把 '$' 起点优先交给表内靠前的候选，而 NMEA 解码器并不限制 talker：`$CONFIG,...` 的
 * talker/type 都合法，校验和也标准，于是它成了 NMEA 帧，UnicoreResp 收不到。UM980 实测的读回行带
 * '$' 入校验的变体，NMEA 校验失败后重扫才落到 UnicoreResp——这就是上一条用例用 capturedConfigLine 的原因。
 */
static void testStandardChecksumConfigLineIsClaimedByNmea()
{
    const std::string line = configLine("COM1,GPGGA,1");
    demux::StaticDemux<demux::nmea::kDecoder, gnss::unicore::kResponseDecoder> demuxEngine;
    CountingSink sink;
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                      static_cast<int>(demuxEngine.init(sink)));
    TEST_ASSERT_EQUAL(static_cast<int>(demux::Error::kOk),
                      static_cast<int>(demuxEngine.feed(bytesOf(line))));
    TEST_ASSERT_EQUAL_UINT32(1U, sink.counters[0]);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.counters[1]);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(sink.unknown));
}

// ---------------------------------------------------------------- 驱动

static void testBuildReadbackWritesConfig()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_TRUE(unicoreDrv().buildReadback(cmd));
    TEST_ASSERT_TRUE(bytesEqual(cmd.view(), kReadbackCommand));
    // Um982Receiver 继续同一条读回命令。
    TEST_ASSERT_TRUE(um982Drv().buildReadback(cmd));
    TEST_ASSERT_TRUE(bytesEqual(cmd.view(), kReadbackCommand));
}

static void testMatchReadbackExtractsBody()
{
    std::array<char, kReadbackCapacity> out{};
    const std::string line = configLine("COM1,GPGGA,1");
    const size_t len =
        unicoreDrv().matchReadback(gnss::unicore::kResponseDecoder, bytesOf(line), out);
    TEST_ASSERT_EQUAL_UINT32(12U, static_cast<uint32_t>(len)); // "COM1,GPGGA,1"
    TEST_ASSERT_TRUE(charsEqual(std::span<const char>(out.data(), len), "COM1,GPGGA,1"));

    // $command,CONFIG,response: OK*.. 是命令应答，不是配置行。
    const std::string reply = commandReply("CONFIG", "OK");
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(unicoreDrv().matchReadback(
                                     gnss::unicore::kResponseDecoder, bytesOf(reply), out)));
    // 别的解码器交付的帧一律不算。
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(unicoreDrv().matchReadback(
                                     demux::nmea::kDecoder, bytesOf(line), out)));
    // 截断：正文比缓冲长时只写缓冲容量，返回值仍 > 0（调用方据此计数）。
    std::array<char, kSmallCapacity> small{};
    TEST_ASSERT_EQUAL_UINT32(kSmallCapacity,
                             static_cast<uint32_t>(unicoreDrv().matchReadback(
                                 gnss::unicore::kResponseDecoder, bytesOf(line), small)));
}

static void testDriversWithoutReadback()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_FALSE(gnss::nullReceiver().buildReadback(cmd));
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    TEST_ASSERT_FALSE(ubloxDrv().buildReadback(cmd));
#endif
#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
    TEST_ASSERT_FALSE(comnavDrv().buildReadback(cmd));
#endif
}

// ---------------------------------------------------------------- 控制器

gnss::Timing fastTiming()
{
    gnss::Timing timing{};
    timing.powerOffMs = kFastMs;
    timing.bootMs = kFastMs;
    timing.probeTimeoutMs = static_cast<uint16_t>(kProbeTimeoutMs);
    timing.probeRounds = 2;
    timing.stepRetries = 1;
    timing.silenceMs = kSilenceMs;
    timing.retryBackoffMs = kRetryBackoffMs;
    return timing;
}

gnss::RoverConfig readbackRoverConfig()
{
    gnss::RoverConfig cfg{};
    cfg.rateHz = 1;
    cfg.gsv = true;
    cfg.port = 2;
    cfg.portBaud = 0; // 不切波特率：脚本只剩 UNLOG / MODE ROVER 两步
    return cfg;
}

void feed(gnss::Controller &ctl, const demux::Decoder &dec, const std::string &frame, uint32_t now)
{
    ctl.onFrame(dec, bytesOf(frame), now);
}

std::string sentText(std::span<const uint8_t> bytes)
{
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        out.push_back(static_cast<char>(bytes[i]));
    }
    while (!out.empty() && ((out.back() == '\r') || (out.back() == '\n'))) {
        out.pop_back();
    }
    return out;
}

#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
/** 按文本宽度补 '\0' 的定长字段（UBX-MON-VER 的文本块）。 */
void putField(std::string &payload, std::string_view text, size_t width)
{
    for (size_t i = 0; i < width; ++i) {
        payload.push_back(i < text.size() ? text[i] : '\0');
    }
}

/** 给 UBX 帧补 Fletcher 校验并返回整帧文本。 */
std::string ubxFrame(int8_t cls, int8_t id, const std::string &payload)
{
    std::string frame;
    frame.push_back(static_cast<char>(kUbxSync1));
    frame.push_back(static_cast<char>(kUbxSync2));
    frame.push_back(static_cast<char>(cls));
    frame.push_back(static_cast<char>(id));
    frame.push_back(static_cast<char>(payload.size() & kByteMask));
    frame.push_back(static_cast<char>((payload.size() >> 8U) & kByteMask));
    frame += payload;
    uint8_t ckA = 0;
    uint8_t ckB = 0;
    for (size_t i = 2U; i < frame.size(); ++i) {
        const auto byte = static_cast<uint8_t>(frame[i]);
        ckA = static_cast<uint8_t>(ckA + byte);
        ckB = static_cast<uint8_t>(ckB + ckA);
    }
    frame.push_back(static_cast<char>(ckA));
    frame.push_back(static_cast<char>(ckB));
    return frame;
}
#endif

/** 探测应答：按驱动家族回一条“型号已知”的帧。 */
void feedProbeReply(gnss::Controller &ctl, const gnss::Receiver &receiver, uint32_t now)
{
    if (receiver.family() == gnss::Family::kUnicore) {
        feed(
            ctl, demux::unicore_hash::kDecoder,
            "#VERSIONA,96,GPS,FINE,2281,45803,0,0,18,10;\"UM980\",\"R4.10Build11833\",\"HRPT00-S10C-P\","
            "\"2310415000123\",\"R4.10.0\",\"ff\"*12345678",
            now);
        return;
    }
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    if (receiver.family() == gnss::Family::kUblox) {
        std::string payload;
        putField(payload, "EXT CORE 1.00 (61b2dd)", kUbxTextWidth);
        putField(payload, "00180000", kUbxExtWidth);
        putField(payload, "MOD=ZED-F9P", kUbxTextWidth);
        putField(payload, "FWVER=HPG 1.32", kUbxTextWidth);
        feed(ctl, demux::ubx::kDecoder,
             ubxFrame(static_cast<int8_t>(kUbxClsMonVer), static_cast<int8_t>(kUbxIdMonVer),
                      payload),
             now);
    }
#endif
}

/** 命令应答：unicore 回 $command,<echo>,response: OK；u-blox 回 ACK-ACK（带被确认的 cls/id）。 */
void feedCommandReply(gnss::Controller &ctl, const gnss::Receiver &receiver,
                      std::span<const uint8_t> sent, uint32_t now)
{
    if (receiver.family() == gnss::Family::kUnicore) {
        feed(ctl, gnss::unicore::kResponseDecoder, commandReply(sentText(sent), "OK"), now);
        return;
    }
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    if ((receiver.family() == gnss::Family::kUblox) && (sent.size() >= 4U)) {
        std::string payload;
        payload.push_back(static_cast<char>(sent[kSentClassPos])); // 被确认消息的 class
        payload.push_back(static_cast<char>(sent[kSentIdPos]));    // 被确认消息的 id
        feed(ctl, demux::ubx::kDecoder,
             ubxFrame(static_cast<int8_t>(kUbxClsAck), static_cast<int8_t>(kUbxIdAck), payload),
             now);
    }
#endif
}

/**
 * @brief 推进控制器到 kRunning：把每次写动作都当成“模块已应答”
 * @param ctl: 控制器
 * @param receiver: 正在使用的驱动（决定应答帧格式）
 * @param now: 单调时间，函数内推进
 * @retval true: 到达 kRunning
 * @retval false: 步数耗尽仍未到达
 */
bool driveToRunning(gnss::Controller &ctl, const gnss::Receiver &receiver, uint32_t &now)
{
    for (uint32_t guard = 0U; guard < kDriveGuard; ++guard) {
        if (ctl.state() == gnss::State::kRunning) {
            return true;
        }
        const gnss::Action act = ctl.poll(now);
        ++now;
        if (act.kind != gnss::Action::Kind::kWrite) {
            continue;
        }
        if (ctl.state() == gnss::State::kProbing) {
            feedProbeReply(ctl, receiver, now);
        } else {
            feedCommandReply(ctl, receiver, act.bytes, now);
        }
    }
    return ctl.state() == gnss::State::kRunning;
}

/**
 * @brief 建一个已到 kRunning 的控制器（真驱动 + 真协议字节）
 * @param ctl: 控制器
 * @param receiver: 要用的驱动
 * @param now: 返回推进后的时间
 * @retval true: 已到 kRunning
 * @retval false: 初始化或推进失败
 */
bool initRunningController(gnss::Controller &ctl, const gnss::Receiver &receiver, uint32_t &now)
{
    static const std::array<uint32_t, 1U> bauds{kTestBaud};
    const std::array<const gnss::Receiver *, 1U> receivers{&receiver};
    const gnss::RoverConfig cfg = readbackRoverConfig();
    if (!ctl.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, fastTiming())) {
        return false;
    }
    now = 0U;
    ctl.start(now);
    return driveToRunning(ctl, receiver, now);
}

bool isWrite(const gnss::Action &action, std::string_view text)
{
    return (action.kind == gnss::Action::Kind::kWrite) && bytesEqual(action.bytes, text);
}

static void testReadbackRejectedBeforeRunning()
{
    gnss::Controller ctl;
    static const std::array<uint32_t, 1U> bauds{kTestBaud};
    const std::array<const gnss::Receiver *, 1U> receivers{&unicoreDrv()};
    const gnss::RoverConfig cfg = readbackRoverConfig();
    TEST_ASSERT_TRUE(ctl.init(receivers, bauds, cfg, gnss::OutputPlan{}, {}, fastTiming()));

    uint32_t now = 0U;
    TEST_ASSERT_FALSE(ctl.requestReadback(now));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kOff), static_cast<int>(ctl.state()));

    ctl.start(now);
    TEST_ASSERT_FALSE(ctl.requestReadback(now));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kPowerUp), static_cast<int>(ctl.state()));
}

static void testReadbackCollectsLinesUntilIdle()
{
    gnss::Controller ctl;
    uint32_t now = 0U;
    TEST_ASSERT_TRUE(initRunningController(ctl, unicoreDrv(), now));
    const uint32_t seqBefore = ctl.stats().readbackSeq;

    TEST_ASSERT_TRUE(ctl.requestReadback(now));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kReadback), static_cast<int>(ctl.state()));
    // 下一轮 poll 把 CONFIG 写出去（与 kSend 同一条通道）。
    TEST_ASSERT_TRUE(isWrite(ctl.poll(now), "CONFIG\r\n"));

    now += kLineGapMs;
    feed(ctl, gnss::unicore::kResponseDecoder, configLine("COM1,GPGGA,1"), now);
    now += kLineGapMs;
    feed(ctl, gnss::unicore::kResponseDecoder, configLine("COM1,GPRMC,1"), now);
    now += kLineGapMs;
    feed(ctl, gnss::unicore::kResponseDecoder, configLine("COM1,MODE,ROVER"), now);
    TEST_ASSERT_EQUAL_UINT32(3U, ctl.stats().readbackLines);

    // 行间静默未到 1 s：仍在读回。
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kReadback), static_cast<int>(ctl.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kNone),
                      static_cast<int>(ctl.poll(now + kIdleMs - 1U).kind));
    // 满 1 s：回到 kRunning，seq+1。
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kNone),
                      static_cast<int>(ctl.poll(now + kIdleMs).kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctl.state()));
    TEST_ASSERT_EQUAL_UINT32(seqBefore + 1U, ctl.stats().readbackSeq);
    TEST_ASSERT_EQUAL_UINT32(3U, ctl.stats().readbackLines);
}

static void testReadbackIgnoresNmeaLines()
{
    gnss::Controller ctl;
    uint32_t now = 0U;
    TEST_ASSERT_TRUE(initRunningController(ctl, unicoreDrv(), now));
    const uint32_t seqBefore = ctl.stats().readbackSeq;

    TEST_ASSERT_TRUE(ctl.requestReadback(now));
    TEST_ASSERT_TRUE(isWrite(ctl.poll(now), "CONFIG\r\n"));
    // NMEA 帧不算读回行，也不延长窗口：1 s 后照样收尾。
    feed(ctl, demux::nmea::kDecoder,
         checksumLine("GNGGA,114533.00,,,N,,E,1,12,1.0,10.0,M,-1.0,M,,", false), now + kNmeaOnlyMs);
    TEST_ASSERT_EQUAL_UINT32(0U, ctl.stats().readbackLines);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kNone),
                      static_cast<int>(ctl.poll(now + kIdleMs).kind));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctl.state()));
    TEST_ASSERT_EQUAL_UINT32(seqBefore + 1U, ctl.stats().readbackSeq);
}

static void testReadbackEndsOnTotalTimeout()
{
    gnss::Controller ctl;
    uint32_t now = 0U;
    TEST_ASSERT_TRUE(initRunningController(ctl, unicoreDrv(), now));
    const uint32_t start = now;
    TEST_ASSERT_TRUE(ctl.requestReadback(now));
    TEST_ASSERT_TRUE(isWrite(ctl.poll(now), "CONFIG\r\n"));

    // 每 400 ms 一行：静默窗口永远不触发，只能由总时长（5 s）收尾。
    uint32_t lines = 0U;
    while (((now - start) + kLineStepMs) < kTotalMs) {
        now += kLineStepMs;
        feed(ctl, gnss::unicore::kResponseDecoder, configLine("COM1,GPGGA,1"), now);
        ++lines;
        TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kReadback), static_cast<int>(ctl.state()));
        (void)ctl.poll(now);
    }
    TEST_ASSERT_EQUAL_UINT32(lines, ctl.stats().readbackLines);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kReadback), static_cast<int>(ctl.state()));
    (void)ctl.poll(start + kTotalMs + 1U);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctl.state()));
    TEST_ASSERT_EQUAL_UINT32(lines, ctl.stats().readbackLines);
}

#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
static void testUbloxRunStillRejectsReadback()
{
    gnss::Controller ctl;
    uint32_t now = 0U;
    // u-blox 脚本在 cfg.portBaud == 0 时只剩一条 VALSET，仍需真实 ACK 才能到 kRunning。
    TEST_ASSERT_TRUE(initRunningController(ctl, ubloxDrv(), now));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctl.state()));
    TEST_ASSERT_FALSE(ctl.requestReadback(now));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctl.state()));
    TEST_ASSERT_EQUAL_UINT32(0U, ctl.stats().readbackSeq);
}
#endif

} // namespace

void setUp() {}
void tearDown() {}

int main(int, char **)
{
    UNITY_BEGIN();
#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
    RUN_TEST(testConfigLineIsDecoded);
    RUN_TEST(testDecoderStillTakesCommandReplies);
    RUN_TEST(testDecoderRejectsBadConfigFrames);
    RUN_TEST(testConfigLinesCoexistWithNmea);
    RUN_TEST(testStandardChecksumConfigLineIsClaimedByNmea);
    RUN_TEST(testOversizedConfigLineIsDropped);
    RUN_TEST(testBuildReadbackWritesConfig);
    RUN_TEST(testMatchReadbackExtractsBody);
    RUN_TEST(testReadbackRejectedBeforeRunning);
    RUN_TEST(testReadbackCollectsLinesUntilIdle);
    RUN_TEST(testReadbackIgnoresNmeaLines);
    RUN_TEST(testReadbackEndsOnTotalTimeout);
#endif
    RUN_TEST(testDriversWithoutReadback);
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    RUN_TEST(testUbloxRunStillRejectsReadback);
#endif
    return UNITY_END();
}
