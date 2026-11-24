// u-blox 驱动 host 测试：UBX-MON-VER 探测、CFG-VALSET 脚本字节、Fletcher 校验、ACK/NAK（ADR-0010 §2.2/§2.4）。
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "demux/decoders/Ubx.hpp"
#include "gnss/Controller.hpp"
#include "gnss/NmeaParser.hpp"
#include "gnss/drivers/UbloxReceiver.hpp"
#include "PppSequence.hpp"
#include "UbloxExt.hpp"
#include "unity.h"

// 驱动层次约束（ADR-0012 §2.8、CPP-011）：家族基类不可直接实例化；型号只实例化平凡析构的 final 叶子类。
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

/**
 * @brief 构造 4 字节头 UBX-CFG-VALSET 载荷并追加小端键/值项的测试辅助。
 * @param layer: 配置层位图。
 * @param items: 键值字节片段。
 * @retval 完整 UBX-CFG-VALSET 帧字节（含 Fletcher 校验）。
 */
Bytes makeValsetFrame(uint8_t layer, const Bytes &items)
{
    Bytes payload{0x00U, layer, 0x00U, 0x00U};
    payload.insert(payload.end(), items.begin(), items.end());
    return makeFrame(0x06U, 0x8AU, payload);
}

/**
 * @brief 向字节数组追加小端 U32。
 * @param dst: 目标字节向量。
 * @param value: 32 位值。
 */
void pushU32(Bytes &dst, uint32_t value)
{
    dst.push_back(static_cast<uint8_t>(value & 0xFFU));
    dst.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
    dst.push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
    dst.push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
}

/**
 * @brief 向字节数组追加小端 U16。
 * @param dst: 目标字节向量。
 * @param value: 16 位值。
 */
void pushU16(Bytes &dst, uint16_t value)
{
    dst.push_back(static_cast<uint8_t>(value & 0xFFU));
    dst.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
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

/**
 * @brief 验证流动站基础配置脚本（配置、12 句 NMEA 静默与可选波特率切换）。
 */
static void testRoverScript()
{
    gnss::RoverConfig cfg{};
    cfg.rateHz = 5;
    cfg.gsv = true;
    cfg.portBaud = kTestBaud;

    // U1 流动站基础脚本共 3 步：基础配置 -> 静默 12 条标准 NMEA -> 可选切波特率。
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(ubloxDrv().roverStep(0, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_TRUE(step.expectReply);
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x30210001U, 200U, 2));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10730004U, 1U, 1));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10740002U, 1U, 1));

    TEST_ASSERT_TRUE(ubloxDrv().roverStep(1, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_TRUE(step.expectReply);
    TEST_ASSERT_EQUAL_UINT32(0U, step.switchBaud);
    TEST_ASSERT_TRUE(step.timeoutMs <= 1000U);
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209100CAU, 0U, 1)); // GLL UART1 = 0
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209100B1U, 0U, 1)); // VTG UART1 = 0
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209100C5U, 0U, 1)); // GSV UART1 = 0
    TEST_ASSERT_TRUE(validFletcher(Bytes(step.cmd.view().begin(), step.cmd.view().end())));

    TEST_ASSERT_TRUE(ubloxDrv().roverStep(2, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_FALSE(step.expectReply);
    TEST_ASSERT_EQUAL_UINT32(kTestBaud, step.switchBaud);
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x40520001U, kTestBaud, 4));
    TEST_ASSERT_TRUE(validFletcher(Bytes(step.cmd.view().begin(), step.cmd.view().end())));

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(ubloxDrv().roverStep(3, gnss::Model::kZedF9p, cfg, none));

    // 不改波特率时脚本共两步（配置 + 静默 12 句 NMEA）。
    cfg.portBaud = 0;
    TEST_ASSERT_TRUE(ubloxDrv().roverStep(1, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_FALSE(ubloxDrv().roverStep(2, gnss::Model::kZedF9p, cfg, none));
}

/**
 * @brief buildOutput：CFG-MSGOUT 单键 4 字节头 VALSET，值 = 每历元次数（周期 × 频率）。
 */
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
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100C5U, 1U, 1));

    // GSA UART1 键修正为 0x209100C0（HPG 1.32 §6.9.14）。
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(r.buildOutput(gnss::OutputMsg::kGsa, 1000,
                                                     gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100C0U, 20U, 1));

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

/** @brief 验证 ZED-F9P 能力位（含 kRtcmIn|kBase，移除与 4 星座 RTK 上限不符的 kRate20Hz）。 */
static void testCaps()
{
    TEST_ASSERT_BITS(gnss::cap::kRtcmIn | gnss::cap::kBase, gnss::cap::kRtcmIn | gnss::cap::kBase,
                     ubloxDrv().caps(gnss::Model::kZedF9p));
    TEST_ASSERT_BITS(gnss::cap::kRate20Hz | gnss::cap::kHeading, 0U,
                     ubloxDrv().caps(gnss::Model::kZedF9p));
    // 单型号驱动：forModel 对任何型号都返回自身（ADR-0012 §2.8）。
    TEST_ASSERT_TRUE(&ubloxDrv().forModel(gnss::Model::kZedF9p) == &ubloxDrv());
    TEST_ASSERT_TRUE(&ubloxDrv().forModel(gnss::Model::kUm982) == &ubloxDrv());
}

// ---- 黄金向量比对（ADR-0012 §2.4）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenUblox.inc"
#include "PlanCheck.hpp"

static void testGoldenProbe()
{
    TEST_ASSERT_TRUE(golden_check::probeMatches(ubloxDrv(), golden_Ublox::kProbe));
}

/** 基础脚本 + buildOutput 逐项下发，合起来必须与基线脚本一致（按 VALSET 键值对比较）。 */
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

/** @brief 黄金能力位比对：保留 GoldenUblox.inc 不变，声明 U2 刻意去除 ZED-F9P 的 kRate20Hz。 */
static void testGoldenCaps()
{
    for (const auto &item : golden_Ublox::kCaps) {
        const auto model = static_cast<gnss::Model>(item.model);
        const uint32_t expected =
            (model == gnss::Model::kZedF9p) ? (item.caps & ~gnss::cap::kRate20Hz) : item.caps;
        TEST_ASSERT_EQUAL_UINT32(expected, ubloxDrv().forModel(model).caps(model));
    }
}

/** @brief C2 一次性命令的字节级合约。 */
static void testMaintenanceCommands()
{
    const auto &driver = ubloxDrv();
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(driver.saveStep(step));
    {
        constexpr std::array<uint8_t, 21U> kExpected{0xb5, 0x62, 0x6, 0x9, 0xd, 0x0,  0x0,
                                                     0x0,  0x0,  0x0, 0x1, 0x0, 0x0,  0x0,
                                                     0x0,  0x0,  0x0, 0x0, 0x3, 0x20, 0xc5};
        TEST_ASSERT_EQUAL_UINT32(kExpected.size(), step.cmd.len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpected.data(), step.cmd.data.data(), kExpected.size());
    }
    TEST_ASSERT_TRUE(driver.factoryResetStep(1U, {}, step));
    {
        constexpr std::array<uint8_t, 21U> kExpected{0xb5, 0x62, 0x6, 0x9, 0xd, 0x0,  0x1,
                                                     0x0,  0x0,  0x0, 0x0, 0x0, 0x0,  0x0,
                                                     0x1,  0x0,  0x0, 0x0, 0x3, 0x21, 0xce};
        TEST_ASSERT_EQUAL_UINT32(kExpected.size(), step.cmd.len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpected.data(), step.cmd.data.data(), kExpected.size());
    }
    TEST_ASSERT_TRUE(driver.baudStep(2U, 460800U, step));
    {
        constexpr std::array<uint8_t, 20U> kExpected{0xb5, 0x62, 0x6, 0x8a, 0xc,  0x0,  0x0,
                                                     0x1,  0x0,  0x0, 0x1,  0x0,  0x53, 0x40,
                                                     0x0,  0x8,  0x7, 0x0,  0x40, 0x89};
        TEST_ASSERT_EQUAL_UINT32(kExpected.size(), step.cmd.len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpected.data(), step.cmd.data.data(), kExpected.size());
    }
    TEST_ASSERT_FALSE(driver.factoryResetStep(2U, {}, step));
    const auto frame = monVer("ZED-F9P", "HPG 1.32");
    gnss::ProbeResult detected{};
    TEST_ASSERT_TRUE(driver.matchProbe(demux::ubx::kDecoder, frame, detected));
    TEST_ASSERT_TRUE(driver.factoryResetStep(2U, detected, step));
    TEST_ASSERT_EQUAL_UINT32(38400U, step.switchBaud);
    gnss::CmdBuf recovery{};
    TEST_ASSERT_TRUE(driver.buildFactoryResetProbe(2U, recovery));
    constexpr std::array<uint8_t, 25U> kRecovery{
        0xb5, 0x62, 0x06, 0x8a, 0x09, 0x00, 0x00, 0x03, 0x00, 0x00, 0x01, 0x00, 0x76,
        0x10, 0x01, 0x24, 0xc9, 0xb5, 0x62, 0x0a, 0x04, 0x00, 0x00, 0x0e, 0x34};
    TEST_ASSERT_EQUAL_UINT32(kRecovery.size(), recovery.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kRecovery.data(), recovery.data.data(), kRecovery.size());
}

/** @brief 默认接线口在 UART2 时，基础与输出脚本都使用 UART2 键，且含 NMEA 高精度两键。 */
static void testConfiguredOutputPort()
{
    const auto &driver = ubloxDrv();
    gnss::RoverConfig cfg{};
    cfg.useConfiguredPort = true;
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(driver.roverStep(0U, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10750004U, 1U, 1));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10760002U, 1U, 1));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10930006U, 1U, 1)); // CFG-NMEA-HIGHPREC = 1
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x20930007U, 1U, 1)); // CFG-NMEA-SVNUMBERING = 1
    TEST_ASSERT_EQUAL_HEX8(0x01U, step.cmd.view()[7U]);      // RAM only.
    TEST_ASSERT_FALSE(hasItem(step.cmd, 0x10740002U, 1U, 1));

    // 第 1 步静默同样落到 UART2 + RAM 层。
    TEST_ASSERT_TRUE(driver.roverStep(1U, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_EQUAL_HEX8(0x01U, step.cmd.view()[7U]);
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209100C6U, 0U, 1)); // GSV UART2 = 0
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209100CBU, 0U, 1)); // GLL UART2 = 0

    gnss::OutputPlan plan{};
    plan.useConfiguredPort = true;
    gnss::CmdBuf cmd{};
    TEST_ASSERT_TRUE(fw::isOk(
        driver.buildOutput(gnss::OutputMsg::kGsv, 5000U, gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100C6U, 5U, 1));
    TEST_ASSERT_EQUAL_HEX8(0x01U, cmd.view()[7U]);
}

/** @brief 检查 VALSET 帧内是否包含指定键与 U4 值。 */
static bool hasItemU4(const gnss::CmdBuf &cmd, uint32_t key, uint32_t value)
{
    const auto view = cmd.view();
    if (view.size() < 18U) {
        return false;
    }
    for (size_t i = 10U; i + 8U <= view.size() - 2U; ++i) {
        const uint32_t k = static_cast<uint32_t>(view[i]) |
                           (static_cast<uint32_t>(view[i + 1U]) << 8U) |
                           (static_cast<uint32_t>(view[i + 2U]) << 16U) |
                           (static_cast<uint32_t>(view[i + 3U]) << 24U);
        const uint32_t v = static_cast<uint32_t>(view[i + 4U]) |
                           (static_cast<uint32_t>(view[i + 5U]) << 8U) |
                           (static_cast<uint32_t>(view[i + 6U]) << 16U) |
                           (static_cast<uint32_t>(view[i + 7U]) << 24U);
        if (k == key && v == value) {
            return true;
        }
    }
    return false;
}

/** @brief C5a 模式切换（TMODE-MODE / Survey / Fixed / RTCM MSGOUT）编码。 */
static void testWorkModeSteps()
{
    const auto &driver = ubloxDrv();
    gnss::ModeRequest req{};
    req.port = 2U;
    req.navRateHz = 1U;
    req.useConfiguredPort = true;
    req.surveySeconds = 20U;
    req.surveyAccCm = 150U;
    req.fixLatE9 = -31234567899LL;
    req.fixLonE9 = 121473700012LL;
    req.fixHeightMm = -1234;
    req.rtcmPeriodMs = {1000U, 1000U, 10000U, 10000U, 1000U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};

    gnss::ConfigStep step{};
    req.action = gnss::ModeAction::kRover;
    TEST_ASSERT_TRUE(driver.modeStep(req, 0U, step));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x20030001U, 0U, 1));
    TEST_ASSERT_TRUE(driver.modeStep(req, 1U, step));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10760004U, 0U, 1)); // UART2 OUTPROT RTCM3X=0
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209102BFU, 0U, 1)); // 1005 UART2=0
    TEST_ASSERT_FALSE(driver.modeStep(req, 2U, step));

    req.action = gnss::ModeAction::kBasePrep;
    TEST_ASSERT_TRUE(driver.modeStep(req, 1U, step));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x10760004U, 1U, 1));  // UART2 OUTPROT RTCM3X=1
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209102BFU, 1U, 1));  // 1005 UART2=1
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x20910305U, 10U, 1)); // 1230 UART2=10

    req.action = gnss::ModeAction::kBaseSurvey;
    TEST_ASSERT_TRUE(driver.modeStep(req, 0U, step));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x20030001U, 1U, 1));
    TEST_ASSERT_TRUE(hasItemU4(step.cmd, 0x40030010U, 20U));
    TEST_ASSERT_TRUE(hasItemU4(step.cmd, 0x40030011U, 15000U));
    TEST_ASSERT_FALSE(driver.modeStep(req, 1U, step));

    req.action = gnss::ModeAction::kBaseFixed;
    TEST_ASSERT_TRUE(driver.modeStep(req, 0U, step));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x209102BFU, 1U, 1));
    TEST_ASSERT_TRUE(driver.modeStep(req, 1U, step));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x20030001U, 2U, 1)); // Fixed
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x20030002U, 1U, 1)); // LLH
    TEST_ASSERT_TRUE(hasItemU4(step.cmd, 0x40030009U, static_cast<uint32_t>(-312345678)));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x2003000CU, static_cast<uint8_t>(int8_t{-99}), 1));
    TEST_ASSERT_TRUE(hasItemU4(step.cmd, 0x4003000BU, static_cast<uint32_t>(-123)));
    TEST_ASSERT_TRUE(hasItem(step.cmd, 0x2003000EU, static_cast<uint8_t>(int8_t{-40}), 1));
    TEST_ASSERT_FALSE(driver.modeStep(req, 2U, step));
}

/**
 * @brief 字节级比对 4 字节头 VALSET 完整帧（含同步字、长度、reserved[2]=0 与 Fletcher 校验）。
 */
static void testValsetByteExact()
{
    const auto &driver = ubloxDrv();
    gnss::RoverConfig cfg{};
    cfg.rateHz = 5U;
    cfg.portBaud = 115200U;
    gnss::ConfigStep step{};

    // Step 0：RAM|BBR 基础配置（4 字节头 + 3 个键）
    TEST_ASSERT_TRUE(driver.roverStep(0U, gnss::Model::kZedF9p, cfg, step));
    Bytes items{};
    pushU32(items, 0x30210001U);
    pushU16(items, 200U);
    pushU32(items, 0x10730004U);
    items.push_back(1U);
    pushU32(items, 0x10740002U);
    items.push_back(1U);
    TEST_ASSERT_TRUE(cmdEquals(step.cmd, makeValsetFrame(0x03U, items)));

    // Step 2：RAM|BBR 切波特率 115200（4 字节头 + 1 个 U4 键）
    TEST_ASSERT_TRUE(driver.roverStep(2U, gnss::Model::kZedF9p, cfg, step));
    items.clear();
    pushU32(items, 0x40520001U);
    pushU32(items, 115200U);
    TEST_ASSERT_TRUE(cmdEquals(step.cmd, makeValsetFrame(0x03U, items)));

    // buildOutput(GGA, 200 ms @ 5 Hz -> 1 epoch)：单键 4 字节头 RAM|BBR
    gnss::OutputPlan plan{};
    plan.rateHz = 5U;
    gnss::CmdBuf cmd{};
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGga, 200U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    items.clear();
    pushU32(items, 0x209100BBU);
    items.push_back(1U);
    TEST_ASSERT_TRUE(cmdEquals(cmd, makeValsetFrame(0x03U, items)));
}

/**
 * @brief 校验 12 条标准 NMEA 静默 VALSET 帧的字节集合与端口合法性检查。
 */
static void testNmeaSilenceFrame()
{
    constexpr std::array<uint32_t, 12U> kExpectedUart1{
        0x209100BBU, 0x209100CAU, 0x209100C0U, 0x209100C5U, 0x209100ACU, 0x209100B1U,
        0x209100D4U, 0x209100D9U, 0x209100B6U, 0x209100A7U, 0x209100CFU, 0x209100DEU,
    };
    gnss::CmdBuf cmd{};
    TEST_ASSERT_TRUE(gnss::ublox::buildSilenceNmeaValset(cmd, 0x03U, 1U));
    Bytes items{};
    for (const uint32_t key : kExpectedUart1) {
        pushU32(items, key);
        items.push_back(0U);
    }
    TEST_ASSERT_TRUE(cmdEquals(cmd, makeValsetFrame(0x03U, items)));

    TEST_ASSERT_TRUE(gnss::ublox::buildSilenceNmeaValset(cmd, 0x01U, 2U));
    items.clear();
    for (const uint32_t key : kExpectedUart1) {
        pushU32(items, key + 1U);
        items.push_back(0U);
    }
    TEST_ASSERT_TRUE(cmdEquals(cmd, makeValsetFrame(0x01U, items)));

    TEST_ASSERT_FALSE(gnss::ublox::buildSilenceNmeaValset(cmd, 0x03U, 0U));
    TEST_ASSERT_FALSE(gnss::ublox::buildSilenceNmeaValset(cmd, 0x03U, 3U));
}

/**
 * @brief 校验 F9P 单帧原子截止角+星座 VALSET（RU1-02/03/06/10）：GPS 必开、L1C/A 显式下发与按固件开关 SBAS。
 */
static void testSatMaskConstellationsAndCutoff()
{
    gnss::CmdBuf cmd{};
    // 非法截止角与 GPS 关闭（含全关）均在编码前拒绝
    const gnss::SatMaskConfig gpsOff{.cutoffDeg = 15U, .gps = false, .glo = true, .bds = true};
    const gnss::SatMaskConfig allOff{.cutoffDeg = 15U, .gps = false};
    const gnss::SatMaskConfig badCutoff91{.cutoffDeg = 91U, .gps = true};
    const gnss::SatMaskConfig badCutoff255{.cutoffDeg = 255U, .gps = true};
    TEST_ASSERT_FALSE(gnss::ublox::buildSatTrackValset(cmd, gpsOff, true));
    TEST_ASSERT_FALSE(gnss::ublox::buildSatTrackValset(cmd, allOff, false));
    TEST_ASSERT_FALSE(gnss::ublox::buildSatTrackValset(cmd, badCutoff91, true));
    TEST_ASSERT_FALSE(gnss::ublox::buildSatTrackValset(cmd, badCutoff255, false));

    // 固件版本 SBAS 能力判定（SparkFun GNSS_ZED.h:36：仅 HPG >= 1.13 支持）
    TEST_ASSERT_FALSE(gnss::ublox::f9pSupportsSbas(""));
    TEST_ASSERT_FALSE(gnss::ublox::f9pSupportsSbas("EXT CORE 1.00"));
    TEST_ASSERT_FALSE(gnss::ublox::f9pSupportsSbas("HPG 1.12"));
    TEST_ASSERT_TRUE(gnss::ublox::f9pSupportsSbas("HPG 1.13"));
    TEST_ASSERT_TRUE(gnss::ublox::f9pSupportsSbas("HPG 1.32"));
    TEST_ASSERT_TRUE(gnss::ublox::f9pSupportsSbas("HPG 1.51"));

    struct Case {
        gnss::SatMaskConfig cfg;
        bool includeSbas;
    };
    constexpr std::array<Case, 4U> kCases{{
        {{.cutoffDeg = 0U, .gps = true, .glo = true, .bds = true, .gal = true, .sbas = true}, true},
        {{.cutoffDeg = 90U, .gps = true, .glo = false, .bds = false, .gal = false, .sbas = false},
         false},
        {{.cutoffDeg = 15U, .gps = true, .glo = false, .bds = true, .gal = true, .sbas = true},
         false},
        {{.cutoffDeg = 20U, .gps = true, .glo = true, .bds = false, .gal = true, .sbas = false},
         true},
    }};
    for (const auto &entry : kCases) {
        TEST_ASSERT_TRUE(gnss::ublox::buildSatTrackValset(cmd, entry.cfg, entry.includeSbas));
        Bytes items{};
        pushU32(items, 0x201100A4U);
        items.push_back(entry.cfg.cutoffDeg);
        pushU32(items, 0x1031001FU);
        items.push_back(1U);
        pushU32(items, 0x10310001U);
        items.push_back(1U);
        pushU32(items, 0x10310024U);
        items.push_back(1U);
        pushU32(items, 0x10310012U);
        items.push_back(1U);
        pushU32(items, 0x10310022U);
        items.push_back(entry.cfg.bds ? 1U : 0U);
        pushU32(items, 0x10310025U);
        items.push_back(entry.cfg.glo ? 1U : 0U);
        pushU32(items, 0x10310021U);
        items.push_back(entry.cfg.gal ? 1U : 0U);
        if (entry.includeSbas) {
            pushU32(items, 0x10310020U);
            items.push_back(entry.cfg.sbas ? 1U : 0U);
        }
        TEST_ASSERT_TRUE(cmdEquals(cmd, makeValsetFrame(0x03U, items)));
    }
}

/**
 * @brief 校验 F9P 单步原子卫星序列的 600 ms 稳定期、超时 300 ms 重同步与 NAK 处理（RU1-04/05/06）。
 */
static void testF9pSatMaskStateMachineAndValsetKeys()
{
    const auto &driver = ubloxDrv();
    const Bytes ack = makeFrame(0x05U, 0x01U, Bytes{0x06U, 0x8AU});
    const Bytes nak = makeFrame(0x05U, 0x00U, Bytes{0x06U, 0x8AU});
    TEST_ASSERT_TRUE(gnss::ublox::isValsetAck(ack));
    TEST_ASSERT_FALSE(gnss::ublox::isValsetNak(ack));
    TEST_ASSERT_TRUE(gnss::ublox::isValsetNak(nak));
    TEST_ASSERT_FALSE(gnss::ublox::isValsetAck(nak));

    gnss::SatMaskSequence seq{};
    std::span<const uint8_t> tx{};
    uint32_t now = 100U;
    const gnss::SatMaskConfig cfg{
        .cutoffDeg = 15U, .gps = true, .glo = false, .bds = true, .gal = true, .sbas = false};
    TEST_ASSERT_FALSE(seq.start({.cutoffDeg = 91U, .gps = true}, gnss::Model::kZedF9p, true));
    TEST_ASSERT_FALSE(seq.start({.cutoffDeg = 15U, .gps = false}, gnss::Model::kZedF9p, true));
    TEST_ASSERT_FALSE(seq.start(cfg, gnss::Model::kK803, false));

    // 场景 1：单步 VALSET（无 SBAS 时 8 键），ACK 后进入 600 ms 非阻塞稳定期，期间保持占用
    TEST_ASSERT_TRUE(seq.start(cfg, gnss::Model::kZedF9p, false));
    TEST_ASSERT_EQUAL_size_t(1U, seq.totalSteps());
    TEST_ASSERT_TRUE(seq.needsTx());
    TEST_ASSERT_FALSE(seq.poll(now, tx).has_value());
    TEST_ASSERT_FALSE(seq.needsTx());
    std::array<uint32_t, gnss::ublox::kMaxExtractedValsetKeys> keys{};
    TEST_ASSERT_EQUAL_size_t(8U, gnss::ublox::extractValsetKeys(tx, keys));
    TEST_ASSERT_EQUAL_HEX32(0x201100A4U, keys[0]);
    TEST_ASSERT_EQUAL_HEX32(0x1031001FU, keys[1]);
    std::array<char, 96U> text{};
    TEST_ASSERT_GREATER_THAN(0U, gnss::ublox::formatValsetKeys(std::span{keys}.first(2U), text));
    TEST_ASSERT_EQUAL_STRING("0x201100A4,0x1031001F", text.data());
    seq.onFrame(driver, demux::ubx::kDecoder, ack, now + 10U);
    TEST_ASSERT_FALSE(seq.poll(now + 20U, tx).has_value());
    TEST_ASSERT_TRUE(seq.active());
    TEST_ASSERT_TRUE(tx.empty());
    // 稳定期未满 600 ms（20 + 599）时仍占用且丢弃重复 ACK/NAK
    seq.onFrame(driver, demux::ubx::kDecoder, nak, now + 100U);
    TEST_ASSERT_FALSE(seq.poll(now + 619U, tx).has_value());
    TEST_ASSERT_TRUE(seq.active());
    TEST_ASSERT_TRUE(seq.poll(now + 620U, tx) == fw::Result::kOk);
    TEST_ASSERT_FALSE(seq.active());

    // 场景 2：NAK 立即返回 kIoError，不进入稳定期（含 SBAS 时 9 键）
    TEST_ASSERT_TRUE(seq.start(cfg, gnss::Model::kZedF9p, true));
    TEST_ASSERT_FALSE(seq.poll(now, tx).has_value());
    TEST_ASSERT_EQUAL_size_t(9U, gnss::ublox::extractValsetKeys(tx, keys));
    seq.onFrame(driver, demux::ubx::kDecoder, nak, now + 5U);
    TEST_ASSERT_TRUE(seq.poll(now + 10U, tx) == fw::Result::kIoError);
    TEST_ASSERT_FALSE(seq.active());

    // 场景 3：1000 ms 超时后进入 300 ms 重同步窗口，期间迟到 ACK/NAK 被丢弃
    TEST_ASSERT_TRUE(seq.start(cfg, gnss::Model::kZedF9p, false));
    TEST_ASSERT_FALSE(seq.poll(now, tx).has_value());
    TEST_ASSERT_FALSE(seq.poll(now + 999U, tx).has_value());
    TEST_ASSERT_FALSE(seq.poll(now + 1000U, tx).has_value());
    TEST_ASSERT_TRUE(seq.active());
    seq.onFrame(driver, demux::ubx::kDecoder, ack, now + 1100U);
    TEST_ASSERT_FALSE(seq.poll(now + 1299U, tx).has_value());
    TEST_ASSERT_TRUE(seq.poll(now + 1300U, tx) == fw::Result::kTimeout);
    TEST_ASSERT_FALSE(seq.active());

    // 空键表格式化为 "none"
    TEST_ASSERT_EQUAL_size_t(4U, gnss::ublox::formatValsetKeys({}, text));
    TEST_ASSERT_EQUAL_STRING("none", text.data());
}

/**
 * @brief 校验 ValsetTracker 单在途命令跟踪、超时仅报一次与 300 ms 重同步丢弃迟到/重复应答（RU1-05）。
 */
static void testValsetTrackerDelayedAndDuplicatedReplies()
{
    const Bytes ack = makeFrame(0x05U, 0x01U, Bytes{0x06U, 0x8AU});
    const Bytes nak = makeFrame(0x05U, 0x00U, Bytes{0x06U, 0x8AU});
    gnss::CmdBuf cmdA{};
    gnss::CmdBuf cmdB{};
    TEST_ASSERT_TRUE(gnss::ublox::buildSilenceNmeaValset(cmdA, 0x01U, 1U));
    TEST_ASSERT_TRUE(gnss::ublox::buildSatTrackValset(
        cmdB, {.cutoffDeg = 15U, .gps = true, .glo = false, .bds = true, .gal = true}, false));

    gnss::ublox::ValsetTracker tracker{};
    std::span<const uint32_t> keys{};
    tracker.onTx(cmdA.view(), 100U, 500U, true);
    TEST_ASSERT_TRUE(tracker.inFlight());
    TEST_ASSERT_FALSE(tracker.pollTimeout(599U, keys));
    // 超时仅触发一次并进入 300 ms 重同步窗口
    TEST_ASSERT_TRUE(tracker.pollTimeout(600U, keys));
    TEST_ASSERT_EQUAL_size_t(12U, keys.size());
    TEST_ASSERT_FALSE(tracker.inFlight());
    TEST_ASSERT_TRUE(tracker.resyncing(600U));
    TEST_ASSERT_FALSE(tracker.pollTimeout(610U, keys));
    // 重同步窗内迟到的 ACK 与 NAK 均归类为 kUnmatched
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::ublox::ValsetReplyKind::kUnmatched),
                      static_cast<int>(tracker.onFrame(ack, 700U, keys)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::ublox::ValsetReplyKind::kUnmatched),
                      static_cast<int>(tracker.onFrame(nak, 899U, keys)));
    TEST_ASSERT_FALSE(tracker.resyncing(900U));

    // 下一条在途命令精确匹配自身键列表，重复 ACK 归类为 kUnmatched
    tracker.onTx(cmdB.view(), 900U, 1000U, true);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::ublox::ValsetReplyKind::kAck),
                      static_cast<int>(tracker.onFrame(ack, 920U, keys)));
    TEST_ASSERT_EQUAL_size_t(8U, keys.size());
    TEST_ASSERT_EQUAL_HEX32(0x201100A4U, keys[0]);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::ublox::ValsetReplyKind::kUnmatched),
                      static_cast<int>(tracker.onFrame(ack, 925U, keys)));
}

/**
 * @brief 驱动真实 Controller 完成 F9P 探测直到进入 kConfiguring，返回当前时钟。
 * @param ctrl: 控制器实例。
 * @param now: 起始时钟（ms）。
 * @retval 进入 kConfiguring 后的时钟。
 */
static uint32_t probeControllerToF9p(gnss::Controller &ctrl, uint32_t now)
{
    const Bytes ver = monVer("ZED-F9P", "HPG 1.32");
    ctrl.start(now);
    for (unsigned step = 0U; step < 8U && ctrl.state() != gnss::State::kConfiguring; ++step) {
        const gnss::Action act = ctrl.poll(now);
        if (act.kind == gnss::Action::Kind::kWrite) {
            ctrl.onFrame(demux::ubx::kDecoder, ver, now + 5U);
            now += 10U;
        } else {
            now += 100U;
        }
    }
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kConfiguring), static_cast<int>(ctrl.state()));
    return now;
}

/**
 * @brief 校验 F9P 静默步骤收 NAK 非致命降级：不重试、不进 kFailed，继续波特率切换与 GSV-off 输出计划（RU1-01/11）。
 */
static void testF9pControllerSilenceNak()
{
    const Bytes ack = makeFrame(0x05U, 0x01U, Bytes{0x06U, 0x8AU});
    const Bytes nak = makeFrame(0x05U, 0x00U, Bytes{0x06U, 0x8AU});
    const std::array<const gnss::Receiver *, 1U> drivers{&ubloxDrv()};
    const std::array<uint32_t, 1U> bauds{38400U};
    gnss::RoverConfig cfg{};
    cfg.port = 1U;
    cfg.portBaud = 921600U;
    cfg.useConfiguredPort = true;
    gnss::Timing timing{};
    timing.powerOffMs = 0U;
    timing.bootMs = 0U;
    timing.stepRetries = 2U;

    gnss::Controller ctrl{};
    gnss::OutputPlan planOff{};
    planOff.port = 1U;
    planOff.rateHz = 1U;
    planOff.useConfiguredPort = true;
    planOff.periodMs[static_cast<size_t>(gnss::OutputMsg::kGga)] = 1000U;
    planOff.periodMs[static_cast<size_t>(gnss::OutputMsg::kGsv)] = 0U;
    TEST_ASSERT_TRUE(ctrl.init(drivers, bauds, cfg, planOff, {}, timing));
    uint32_t now = probeControllerToF9p(ctrl, 0U);
    unsigned silenceSent = 0U;
    unsigned outputWrites = 0U;
    bool baudSwitched = false;
    bool sawAnyGsvEnable = false;
    for (unsigned iter = 0U; iter < 80U && ctrl.state() != gnss::State::kRunning; ++iter) {
        const gnss::Action act = ctrl.poll(now);
        if (act.kind == gnss::Action::Kind::kSetBaud) {
            baudSwitched = (act.baud == 921600U);
        } else if (act.kind == gnss::Action::Kind::kWrite) {
            if (ctrl.state() == gnss::State::kOutput) {
                ++outputWrites;
                std::array<uint32_t, gnss::ublox::kMaxExtractedValsetKeys> outKeys{};
                const size_t keyCount = gnss::ublox::extractValsetKeys(act.bytes, outKeys);
                if ((keyCount == 1U) && (outKeys[0] == 0x209100C5U)) {
                    sawAnyGsvEnable = true;
                }
                ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
            } else if (gnss::ublox::isSilenceNmeaValset(act.bytes)) {
                ++silenceSent;
                ctrl.onFrame(demux::ubx::kDecoder, nak, now + 5U);
            } else {
                ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
            }
        }
        now += 20U;
    }
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctrl.state()));
    TEST_ASSERT_EQUAL_UINT(1U, silenceSent);
    TEST_ASSERT_TRUE(baudSwitched);
    TEST_ASSERT_GREATER_THAN_UINT(0U, outputWrites);
    TEST_ASSERT_FALSE(sawAnyGsvEnable);
    TEST_ASSERT_BITS_HIGH(1U << static_cast<uint32_t>(gnss::OutputMsg::kGga),
                          ctrl.stats().appliedMask);
    TEST_ASSERT_BITS_LOW(1U << static_cast<uint32_t>(gnss::OutputMsg::kGsv),
                         ctrl.stats().appliedMask);
    TEST_ASSERT_EQUAL_UINT(1U, ctrl.stats().outputRuns);
    TEST_ASSERT_EQUAL_UINT(0U, ctrl.stats().failures);
}

/**
 * @brief 校验 F9P 基础步与静默步超时均经 300 ms 重同步丢弃迟到 ACK/NAK，并完成 GSV-on 输出计划（RU1-01/05/11）。
 */
static void testF9pControllerSilenceTimeoutAndResync()
{
    const Bytes ack = makeFrame(0x05U, 0x01U, Bytes{0x06U, 0x8AU});
    const Bytes nak = makeFrame(0x05U, 0x00U, Bytes{0x06U, 0x8AU});
    const std::array<const gnss::Receiver *, 1U> drivers{&ubloxDrv()};
    const std::array<uint32_t, 1U> bauds{38400U};
    gnss::RoverConfig cfg{};
    cfg.port = 1U;
    cfg.portBaud = 921600U;
    cfg.useConfiguredPort = true;
    gnss::Timing timing{};
    timing.powerOffMs = 0U;
    timing.bootMs = 0U;
    timing.stepRetries = 2U;

    gnss::Controller ctrl{};
    gnss::OutputPlan planOn{};
    planOn.port = 1U;
    planOn.rateHz = 1U;
    planOn.useConfiguredPort = true;
    planOn.periodMs[static_cast<size_t>(gnss::OutputMsg::kGga)] = 1000U;
    planOn.periodMs[static_cast<size_t>(gnss::OutputMsg::kGsv)] = 5000U;
    TEST_ASSERT_TRUE(ctrl.init(drivers, bauds, cfg, planOn, {}, timing));
    uint32_t now = probeControllerToF9p(ctrl, 0U);
    bool droppedFirstBasic = false;
    unsigned silenceSent = 0U;
    unsigned outputWrites = 0U;
    bool sawGsvOn = false;
    for (unsigned iter = 0U; iter < 80U && ctrl.state() != gnss::State::kRunning; ++iter) {
        const gnss::Action act = ctrl.poll(now);
        if (act.kind != gnss::Action::Kind::kWrite) {
            now += 20U;
            continue;
        }
        if (!droppedFirstBasic && ctrl.state() == gnss::State::kConfiguring &&
            !gnss::ublox::isSilenceNmeaValset(act.bytes)) {
            droppedFirstBasic = true;
            now += 500U;
            TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kNone),
                              static_cast<int>(ctrl.poll(now).kind));
            ctrl.onFrame(demux::ubx::kDecoder, ack, now + 100U);
            now += 300U;
            continue;
        }
        if (gnss::ublox::isSilenceNmeaValset(act.bytes)) {
            ++silenceSent;
            now += 500U;
            TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kNone),
                              static_cast<int>(ctrl.poll(now).kind));
            ctrl.onFrame(demux::ubx::kDecoder, ack, now + 50U);
            ctrl.onFrame(demux::ubx::kDecoder, nak, now + 150U);
            now += 300U;
            continue;
        }
        if (ctrl.state() == gnss::State::kOutput) {
            ++outputWrites;
            Bytes gsvOnItem{};
            pushU32(gsvOnItem, 0x209100C5U);
            gsvOnItem.push_back(5U);
            if (Bytes(act.bytes.begin(), act.bytes.end()) == makeValsetFrame(0x01U, gsvOnItem)) {
                sawGsvOn = true;
            }
        }
        ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
        now += 20U;
    }
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctrl.state()));
    TEST_ASSERT_EQUAL_UINT(1U, silenceSent);
    TEST_ASSERT_GREATER_THAN_UINT(0U, outputWrites);
    TEST_ASSERT_TRUE(sawGsvOn);
    TEST_ASSERT_EQUAL_UINT(1U, ctrl.stats().outputRuns);
    TEST_ASSERT_EQUAL_UINT(0U, ctrl.stats().failures);
}

/**
 * @brief 校验 F9P 工作模式 VALSET 超时重试必须经过 300 ms 重同步窗并丢弃迟到 ACK（NRU1-04）。
 */
static void testF9pModeValsetRetryResync()
{
    const Bytes ack = makeFrame(0x05U, 0x01U, Bytes{0x06U, 0x8AU});
    const std::array<const gnss::Receiver *, 1U> drivers{&ubloxDrv()};
    const std::array<uint32_t, 1U> bauds{38400U};
    gnss::RoverConfig cfg{};
    cfg.port = 1U;
    cfg.portBaud = 38400U;
    cfg.useConfiguredPort = true;
    gnss::Timing timing{};
    timing.powerOffMs = 0U;
    timing.bootMs = 0U;
    timing.stepRetries = 1U;

    gnss::Controller ctrl{};
    gnss::OutputPlan plan{};
    plan.port = 1U;
    plan.rateHz = 1U;
    plan.useConfiguredPort = true;
    plan.periodMs[static_cast<size_t>(gnss::OutputMsg::kGga)] = 1000U;
    TEST_ASSERT_TRUE(ctrl.init(drivers, bauds, cfg, plan, {}, timing));
    uint32_t now = probeControllerToF9p(ctrl, 0U);
    for (unsigned iter = 0U; iter < 80U && ctrl.state() != gnss::State::kRunning; ++iter) {
        const gnss::Action act = ctrl.poll(now);
        if (act.kind == gnss::Action::Kind::kWrite) {
            ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
        }
        now += 20U;
    }
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctrl.state()));
    const uint32_t timeoutsBefore = ctrl.stats().stepTimeouts;

    gnss::ModeRequest req{};
    req.action = gnss::ModeAction::kBaseSurvey;
    req.port = 1U;
    req.useConfiguredPort = true;
    req.navRateHz = 1U;
    req.surveySeconds = 60U;
    req.surveyAccCm = 100U;
    TEST_ASSERT_TRUE(ctrl.requestMode(req));
    const gnss::Action first = ctrl.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kWrite), static_cast<int>(first.kind));
    TEST_ASSERT_TRUE(gnss::ublox::isValsetFrame(first.bytes));

    // 首次步超时：不得同轮重发，必须进入 300 ms 重同步窗并丢弃迟到 ACK。
    now += 600U;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kNone),
                      static_cast<int>(ctrl.poll(now).kind));
    ctrl.onFrame(demux::ubx::kDecoder, ack, now + 100U);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kNone),
                      static_cast<int>(ctrl.poll(now + 200U).kind));
    now += gnss::ublox::kValsetResyncWindowMs;
    const gnss::Action retry = ctrl.poll(now);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Action::Kind::kWrite), static_cast<int>(retry.kind));
    TEST_ASSERT_TRUE(gnss::ublox::isValsetFrame(retry.bytes));

    // 重试及后续步骤全部正常 ACK 后完成。
    ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
    now += 20U;
    for (unsigned iter = 0U; iter < 40U && ctrl.state() != gnss::State::kRunning; ++iter) {
        const gnss::Action act = ctrl.poll(now);
        if (act.kind == gnss::Action::Kind::kWrite) {
            ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
        }
        now += 20U;
    }
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctrl.state()));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::MaintenanceResult::kOk),
                      static_cast<int>(ctrl.stats().maintenanceResult));
    TEST_ASSERT_EQUAL_UINT(timeoutsBefore + 1U, ctrl.stats().stepTimeouts);
}

/**
 * @brief 校验 MON-VER 边界用例：未知扩展、缺少 FWVER 回退 swVersion、变体型号、截断与坏校验和。
 */
static void testMonVerEdgeCases()
{
    const auto &driver = ubloxDrv();
    gnss::ProbeResult result{};

    // 含未知扩展项与 ZED-F9P-04B 变体后缀 -> 正常识别
    Bytes payload{};
    appendText(payload, "EXT CORE 1.00 (61b2dd)", 30);
    appendText(payload, "00190000", 10);
    appendText(payload, "ROM BASE 0x118B2060", 30);
    appendText(payload, "MOD=ZED-F9P-04B", 30);
    appendText(payload, "PROTVER=27.31", 30);
    appendText(payload, "FWVER=HPG 1.32", 30);
    appendText(payload, "GPS;GLO;GAL;BDS", 30);
    const Bytes rich = makeFrame(0x0AU, 0x04U, payload);
    TEST_ASSERT_TRUE(driver.matchProbe(demux::ubx::kDecoder, rich, result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kZedF9p), static_cast<int>(result.model));
    TEST_ASSERT_EQUAL_STRING("HPG 1.32", result.fwVersion.data());

    // 缺少 FWVER= 时回退到 swVersion（且忽略尾部不足 30 B 的残缺扩展）
    payload.clear();
    appendText(payload, "EXT CORE 1.00 (61b2dd)", 30);
    appendText(payload, "00190000", 10);
    appendText(payload, "MOD=ZED-F9P", 30);
    payload.push_back('X');
    payload.push_back('Y');
    const Bytes fallback = makeFrame(0x0AU, 0x04U, payload);
    TEST_ASSERT_TRUE(driver.matchProbe(demux::ubx::kDecoder, fallback, result));
    TEST_ASSERT_EQUAL_STRING("EXT CORE 1.00 (61b2dd)", result.fwVersion.data());

    // ZED-F9P2 误匹配拒绝、坏校验和拒绝、载荷 < 40 B 拒绝
    const Bytes wrongModel = monVer("ZED-F9P2", "HPG 1.32");
    TEST_ASSERT_FALSE(driver.matchProbe(demux::ubx::kDecoder, wrongModel, result));
    Bytes badCrc = rich;
    badCrc.back() ^= 0xFFU;
    TEST_ASSERT_FALSE(driver.matchProbe(demux::ubx::kDecoder, badCrc, result));
    const Bytes shortFrame = makeFrame(0x0AU, 0x04U, Bytes(39U, 0x20U));
    TEST_ASSERT_FALSE(driver.matchProbe(demux::ubx::kDecoder, shortFrame, result));
}

/**
 * @brief 校验 UBX-SEC-UNIQID 轮询帧字节与解析边界（正常/截断/错误版本/错误校验/短缓冲）。
 */
static void testSecUniqIdPollAndParse()
{
    gnss::CmdBuf poll{};
    TEST_ASSERT_TRUE(gnss::ublox::buildPollSecUniqId(poll));
    TEST_ASSERT_TRUE(
        cmdEquals(poll, Bytes{0xB5U, 0x62U, 0x27U, 0x03U, 0x00U, 0x00U, 0x2AU, 0xA5U}));

    const Bytes valid = makeFrame(
        0x27U, 0x03U, Bytes{0x01U, 0x00U, 0x00U, 0x00U, 0xA1U, 0x0BU, 0xC2U, 0x3DU, 0x4EU});
    std::array<char, 16U> sn{};
    TEST_ASSERT_EQUAL_size_t(10U, gnss::ublox::parseSecUniqId(valid, sn));
    TEST_ASSERT_EQUAL_STRING("A10BC23D4E", sn.data());

    // 短输出缓冲拒绝
    std::array<char, 10U> shortBuf{};
    TEST_ASSERT_EQUAL_size_t(0U, gnss::ublox::parseSecUniqId(valid, shortBuf));

    // 错误版本、截断载荷、错误校验和、错误 class/id 全部拒绝并置空输出
    const Bytes badVer = makeFrame(
        0x27U, 0x03U, Bytes{0x02U, 0x00U, 0x00U, 0x00U, 0xA1U, 0x0BU, 0xC2U, 0x3DU, 0x4EU});
    TEST_ASSERT_EQUAL_size_t(0U, gnss::ublox::parseSecUniqId(badVer, sn));
    TEST_ASSERT_EQUAL_STRING("", sn.data());

    const Bytes truncated = makeFrame(0x27U, 0x03U, Bytes{0x01U, 0x00U, 0x00U, 0x00U, 0xA1U});
    TEST_ASSERT_EQUAL_size_t(0U, gnss::ublox::parseSecUniqId(truncated, sn));

    Bytes badCrc = valid;
    badCrc.back() ^= 0x01U;
    TEST_ASSERT_EQUAL_size_t(0U, gnss::ublox::parseSecUniqId(badCrc, sn));

    const Bytes wrongId = makeFrame(
        0x27U, 0x01U, Bytes{0x01U, 0x00U, 0x00U, 0x00U, 0xA1U, 0x0BU, 0xC2U, 0x3DU, 0x4EU});
    TEST_ASSERT_EQUAL_size_t(0U, gnss::ublox::parseSecUniqId(wrongId, sn));
}

/** @brief 验证 ZED-F9P 覆写的 navRatesHz 与 outputPeriodsMs 值集及非 F9P 空集。 */
static void testF9pNavRatesAndOutputPeriods()
{
    const auto &driver = ubloxDrv();
    const auto rates = driver.navRatesHz(gnss::Model::kZedF9p);
    TEST_ASSERT_EQUAL_size_t(3U, rates.size());
    TEST_ASSERT_EQUAL_UINT8(1U, rates[0]);
    TEST_ASSERT_EQUAL_UINT8(2U, rates[1]);
    TEST_ASSERT_EQUAL_UINT8(5U, rates[2]);
    TEST_ASSERT_TRUE(driver.navRatesHz(gnss::Model::kUnknown).empty());
    TEST_ASSERT_TRUE(driver.navRatesHz(gnss::Model::kUm980).empty());

    for (const auto msg : {gnss::OutputMsg::kGga, gnss::OutputMsg::kGst, gnss::OutputMsg::kRmc}) {
        const auto periods = driver.outputPeriodsMs(msg, gnss::Model::kZedF9p);
        TEST_ASSERT_EQUAL_size_t(4U, periods.size());
        TEST_ASSERT_EQUAL_UINT16(0U, periods[0]);
        TEST_ASSERT_EQUAL_UINT16(200U, periods[1]);
        TEST_ASSERT_EQUAL_UINT16(500U, periods[2]);
        TEST_ASSERT_EQUAL_UINT16(1000U, periods[3]);
    }
    const auto gsa = driver.outputPeriodsMs(gnss::OutputMsg::kGsa, gnss::Model::kZedF9p);
    TEST_ASSERT_EQUAL_size_t(2U, gsa.size());
    TEST_ASSERT_EQUAL_UINT16(0U, gsa[0]);
    TEST_ASSERT_EQUAL_UINT16(1000U, gsa[1]);

    const auto gsv = driver.outputPeriodsMs(gnss::OutputMsg::kGsv, gnss::Model::kZedF9p);
    TEST_ASSERT_EQUAL_size_t(3U, gsv.size());
    TEST_ASSERT_EQUAL_UINT16(0U, gsv[0]);
    TEST_ASSERT_EQUAL_UINT16(1000U, gsv[1]);
    TEST_ASSERT_EQUAL_UINT16(5000U, gsv[2]);

    TEST_ASSERT_TRUE(
        driver.outputPeriodsMs(gnss::OutputMsg::kSatsInfo, gnss::Model::kZedF9p).empty());
    TEST_ASSERT_TRUE(
        driver.outputPeriodsMs(gnss::OutputMsg::kHeading, gnss::Model::kZedF9p).empty());
    TEST_ASSERT_TRUE(driver.outputPeriodsMs(gnss::OutputMsg::kGga, gnss::Model::kUnknown).empty());
}

/** @brief 验证 buildOutput 严格执行整倍导航历元规则（1..255），拒绝静默取整。 */
static void testBuildOutputStrictMultiple()
{
    const auto &driver = ubloxDrv();
    gnss::OutputPlan plan{};
    plan.useConfiguredPort = true;
    plan.port = 1U;
    gnss::CmdBuf cmd{};

    // 5 Hz（Tnav=200 ms）下 500 ms 非整数倍，必须拒绝而非静默取整为 600 ms。
    plan.rateHz = 5U;
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGga, 500U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGga, 100U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGga, 51200U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGga, 51000U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100BBU, 255U, 1U));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGsv, 5000U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100C5U, 25U, 1U));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGga, 0U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100BBU, 0U, 1U));

    // 2 Hz（Tnav=500 ms）下 200 ms 非整数倍，1 Hz（Tnav=1000 ms）下 500 ms 非整数倍。
    plan.rateHz = 2U;
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kRmc, 200U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kOk),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kRmc, 500U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
    TEST_ASSERT_TRUE(hasItem(cmd, 0x209100ACU, 1U, 1U));

    plan.rateHz = 1U;
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kInvalidArg),
                      static_cast<int>(driver.buildOutput(gnss::OutputMsg::kGst, 500U,
                                                          gnss::Model::kZedF9p, plan, cmd)));
}

/** @brief 字节级验证 cmdConfigurePort 含 CFG-NMEA-HIGHPREC=1 与 CFG-NMEA-SVNUMBERING=1。 */
static void testConfiguredPortNmeaHighPrecBytes()
{
    const auto &driver = ubloxDrv();
    gnss::RoverConfig cfg{};
    cfg.useConfiguredPort = true;
    cfg.port = 1U;
    cfg.rateHz = 5U;
    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(driver.roverStep(0U, gnss::Model::kZedF9p, cfg, step));

    Bytes items{};
    pushU32(items, 0x30210001U);
    pushU16(items, 200U); // 5 Hz -> 200 ms
    pushU32(items, 0x10730004U);
    items.push_back(1U); // CFG-UART1INPROT-RTCM3X = 1
    pushU32(items, 0x10740002U);
    items.push_back(1U); // CFG-UART1OUTPROT-NMEA = 1
    pushU32(items, 0x10930006U);
    items.push_back(1U); // CFG-NMEA-HIGHPREC = 1
    pushU32(items, 0x20930007U);
    items.push_back(1U); // CFG-NMEA-SVNUMBERING = 1
    const Bytes expected = makeValsetFrame(0x01U, items);
    TEST_ASSERT_EQUAL_size_t(expected.size(), step.cmd.len);
    TEST_ASSERT_TRUE(cmdEquals(step.cmd, expected));
    TEST_ASSERT_TRUE(gnss::ublox::validUbxChecksum(step.cmd.view()));
}

/** @brief 验证 HPG 1.12 对含高精度键的串口配置 VALSET 返回 NAK 时降级为 3 键基础配置且非致命。 */
static void testF9pControllerPortConfigNakFallback()
{
    gnss::Controller ctrl{};
    const std::array<const gnss::Receiver *, 1> drv{&ubloxDrv()};
    const std::array<uint32_t, 1> bauds{115200U};
    gnss::RoverConfig cfg{};
    cfg.useConfiguredPort = true;
    cfg.port = 1U;
    cfg.rateHz = 5U;
    gnss::OutputPlan plan{};
    plan.useConfiguredPort = true;
    plan.port = 1U;
    plan.rateHz = 5U;
    plan.periodMs[static_cast<size_t>(gnss::OutputMsg::kGga)] = 200U;
    gnss::Timing timing{};
    timing.powerOffMs = 0U;
    timing.bootMs = 0U;
    timing.stepRetries = 1U;
    TEST_ASSERT_TRUE(ctrl.init(drv, bauds, cfg, plan, gnss::Hint{}, timing));
    uint32_t now = 10U;
    ctrl.start(now);

    const Bytes probeReply = monVer("ZED-F9P", "HPG 1.12");
    const Bytes ack = makeFrame(0x05U, 0x01U, Bytes{0x06U, 0x8AU});
    const Bytes nak = makeFrame(0x05U, 0x00U, Bytes{0x06U, 0x8AU});
    bool sawFiveKey = false;
    bool sawThreeKeyFallback = false;

    for (unsigned iter = 0U; iter < 60U && ctrl.state() != gnss::State::kRunning; ++iter) {
        const gnss::Action act = ctrl.poll(now);
        if (act.kind == gnss::Action::Kind::kWrite) {
            if (ctrl.state() == gnss::State::kProbing) {
                ctrl.onFrame(demux::ubx::kDecoder, probeReply, now + 5U);
            } else if (gnss::ublox::isValsetFrame(act.bytes)) {
                std::array<uint32_t, gnss::ublox::kMaxExtractedValsetKeys> keys{};
                const size_t n = gnss::ublox::extractValsetKeys(act.bytes, keys);
                bool hasHighPrec = false;
                for (size_t i = 0U; i < n; ++i) {
                    if (keys[i] == 0x10930006U || keys[i] == 0x20930007U) {
                        hasHighPrec = true;
                    }
                }
                if (hasHighPrec) {
                    sawFiveKey = true;
                    ctrl.onFrame(demux::ubx::kDecoder, nak, now + 5U);
                } else {
                    if (n == 3U && keys[0] == 0x30210001U) {
                        sawThreeKeyFallback = true;
                    }
                    ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
                }
            }
        }
        now += 20U;
    }
    TEST_ASSERT_TRUE(sawFiveKey);
    TEST_ASSERT_TRUE(sawThreeKeyFallback);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kRunning), static_cast<int>(ctrl.state()));
    TEST_ASSERT_EQUAL_UINT32(0U, ctrl.stats().failures);
}

/** @brief 5 键与降级 3 键端口 VALSET 都被 NAK 时，重试次数有界并进入 kFailed（不得无限循环）。 */
static void testF9pControllerPortConfigFallbackNakFails()
{
    gnss::Controller ctrl{};
    const std::array<const gnss::Receiver *, 1> drv{&ubloxDrv()};
    const std::array<uint32_t, 1> bauds{115200U};
    gnss::RoverConfig cfg{};
    cfg.useConfiguredPort = true;
    cfg.port = 1U;
    cfg.rateHz = 5U;
    gnss::OutputPlan plan{};
    plan.useConfiguredPort = true;
    plan.port = 1U;
    plan.rateHz = 5U;
    plan.periodMs[static_cast<size_t>(gnss::OutputMsg::kGga)] = 200U;
    gnss::Timing timing{};
    timing.powerOffMs = 0U;
    timing.bootMs = 0U;
    timing.stepRetries = 1U;
    TEST_ASSERT_TRUE(ctrl.init(drv, bauds, cfg, plan, gnss::Hint{}, timing));
    uint32_t now = 10U;
    ctrl.start(now);

    const Bytes probeReply = monVer("ZED-F9P", "HPG 1.12");
    const Bytes ack = makeFrame(0x05U, 0x01U, Bytes{0x06U, 0x8AU});
    const Bytes nak = makeFrame(0x05U, 0x00U, Bytes{0x06U, 0x8AU});
    unsigned portNaks = 0U;

    for (unsigned iter = 0U; iter < 400U && ctrl.state() != gnss::State::kFailed &&
                             ctrl.state() != gnss::State::kRunning;
         ++iter) {
        const gnss::Action act = ctrl.poll(now);
        if (act.kind == gnss::Action::Kind::kWrite) {
            if (ctrl.state() == gnss::State::kProbing) {
                ctrl.onFrame(demux::ubx::kDecoder, probeReply, now + 5U);
            } else if (gnss::ublox::isValsetFrame(act.bytes)) {
                std::array<uint32_t, gnss::ublox::kMaxExtractedValsetKeys> keys{};
                const size_t n = gnss::ublox::extractValsetKeys(act.bytes, keys);
                bool portCfg = false;
                for (size_t i = 0U; i < n; ++i) {
                    if (keys[i] == 0x30210001U || keys[i] == 0x10930006U) {
                        portCfg = true;
                    }
                }
                if (portCfg) {
                    ++portNaks;
                    ctrl.onFrame(demux::ubx::kDecoder, nak, now + 5U);
                } else {
                    ctrl.onFrame(demux::ubx::kDecoder, ack, now + 5U);
                }
            }
        }
        now += 20U;
    }
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::State::kFailed), static_cast<int>(ctrl.state()));
    TEST_ASSERT_TRUE(portNaks <= 4U);
}

/** @brief 验证 7 位小数高精度 NMEA GGA 帧长在解析器上限内且经纬度/高程精确解析。 */
static void testNmeaHighPrec7DecimalGgaParse()
{
    // 92 字节高精度 GGA（7 位小数经纬度 + 3 位小数高程），带正确 XOR 校验。
    constexpr std::string_view kBody =
        "GNGGA,123519.00,4807.0381234,N,12345.6789012,W,4,12,0.55,1234.567,M,-123.456,M,1.2,0000";
    uint8_t x = 0U;
    for (const char ch : kBody) {
        x ^= static_cast<uint8_t>(ch);
    }
    char sentence[128]{};
    const int len =
        std::snprintf(sentence, sizeof(sentence), "$%.*s*%02X\r\n", static_cast<int>(kBody.size()),
                      kBody.data(), static_cast<unsigned>(x));
    TEST_ASSERT_TRUE(len > 82 && len < 256);

    gnss::NmeaParser parser{};
    const auto span = std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(sentence),
                                               static_cast<size_t>(len)};
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kFix),
                      static_cast<int>(parser.feed(span, 1000U)));
    TEST_ASSERT_TRUE(parser.fix().valid);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::FixQuality::kRtkFixed),
                      static_cast<int>(parser.fix().quality));
    TEST_ASSERT_EQUAL_INT32(1234567, parser.fix().altMm);
    TEST_ASSERT_EQUAL_INT32(-123456, parser.fix().geoidMm);
    // 48 + 7.0381234/60 = 48.11730205666... -> 48117302057 (1e-9 deg)
    TEST_ASSERT_INT64_WITHIN(1LL, 48117302057LL, parser.fix().latE9);
    // -(123 + 45.6789012/60) = -123.76131502 -> -123761315020 (1e-9 deg)
    TEST_ASSERT_INT64_WITHIN(1LL, -123761315020LL, parser.fix().lonE9);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testMaintenanceCommands);
    RUN_TEST(testWorkModeSteps);
    RUN_TEST(testConfiguredOutputPort);
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
    RUN_TEST(testValsetByteExact);
    RUN_TEST(testNmeaSilenceFrame);
    RUN_TEST(testSatMaskConstellationsAndCutoff);
    RUN_TEST(testF9pSatMaskStateMachineAndValsetKeys);
    RUN_TEST(testValsetTrackerDelayedAndDuplicatedReplies);
    RUN_TEST(testF9pControllerSilenceNak);
    RUN_TEST(testF9pControllerSilenceTimeoutAndResync);
    RUN_TEST(testF9pModeValsetRetryResync);
    RUN_TEST(testMonVerEdgeCases);
    RUN_TEST(testSecUniqIdPollAndParse);
    RUN_TEST(testF9pNavRatesAndOutputPeriods);
    RUN_TEST(testBuildOutputStrictMultiple);
    RUN_TEST(testConfiguredPortNmeaHighPrecBytes);
    RUN_TEST(testF9pControllerPortConfigNakFallback);
    RUN_TEST(testF9pControllerPortConfigFallbackNakFails);
    RUN_TEST(testNmeaHighPrec7DecimalGgaParse);
    return UNITY_END();
}
