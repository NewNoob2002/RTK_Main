// u-blox 驱动 host 测试：UBX-MON-VER 探测、CFG-VALSET 脚本字节、Fletcher 校验、ACK/NAK（ADR-0010 §2.2/§2.4）。
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "demux/decoders/Ubx.hpp"
#include "gnss/drivers/UbloxReceiver.hpp"
#include "unity.h"

namespace {

using Bytes = std::vector<uint8_t>;

Bytes make_frame(uint8_t cls, uint8_t id, const Bytes &payload)
{
    Bytes frame{0xB5,
                0x62,
                cls,
                id,
                static_cast<uint8_t>(payload.size() & 0xFF),
                static_cast<uint8_t>((payload.size() >> 8) & 0xFF)};
    frame.insert(frame.end(), payload.begin(), payload.end());
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    for (size_t i = 2; i < frame.size(); ++i) {
        ck_a = static_cast<uint8_t>(ck_a + frame[i]);
        ck_b = static_cast<uint8_t>(ck_b + ck_a);
    }
    frame.push_back(ck_a);
    frame.push_back(ck_b);
    return frame;
}

void append_text(Bytes &payload, const char *text, size_t width)
{
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    for (size_t i = 0; i < width; ++i) {
        payload.push_back(i < len ? static_cast<uint8_t>(text[i]) : 0U);
    }
}

Bytes mon_ver(const char *model, const char *fw)
{
    Bytes payload;
    append_text(payload, "EXT CORE 1.00 (61b2dd)", 30);
    append_text(payload, "00180000", 10);
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
    return make_frame(0x0A, 0x04, payload);
}

bool cmd_equals(const gnss::CmdBuf &cmd, const Bytes &expected)
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

bool has_item(const gnss::CmdBuf &cmd, uint32_t key, uint32_t value, size_t width)
{
    const std::span<const uint8_t> view = cmd.view();
    for (size_t i = 0; i + 5 + width <= view.size(); ++i) {
        const uint32_t k = static_cast<uint32_t>(view[i]) | (static_cast<uint32_t>(view[i + 1]) << 8) |
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

bool valid_fletcher(const Bytes &frame)
{
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    for (size_t i = 2; i + 2 < frame.size(); ++i) {
        ck_a = static_cast<uint8_t>(ck_a + frame[i]);
        ck_b = static_cast<uint8_t>(ck_b + ck_a);
    }
    return frame[frame.size() - 2] == ck_a && frame[frame.size() - 1] == ck_b;
}

} // namespace

void setUp() {}
void tearDown() {}

static void test_build_probe()
{
    gnss::CmdBuf cmd;
    TEST_ASSERT_TRUE(gnss::ublox_receiver().build_probe(cmd));
    TEST_ASSERT_TRUE(cmd_equals(cmd, Bytes{0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34}));
}

static void test_match_probe()
{
    const Bytes frame = mon_ver("ZED-F9P", "HPG 1.32");
    std::span<const uint8_t> span(frame.data(), frame.size());
    gnss::ProbeResult result{};
    TEST_ASSERT_TRUE(gnss::ublox_receiver().match_probe(demux::ubx::kDecoder, span, result));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kZedF9p), static_cast<int>(result.model));
    TEST_ASSERT_EQUAL_STRING("HPG 1.32", result.fw_version.data());

    const Bytes other = mon_ver("ZED-F9R", "HPG 1.32");
    std::span<const uint8_t> other_span(other.data(), other.size());
    TEST_ASSERT_FALSE(gnss::ublox_receiver().match_probe(demux::ubx::kDecoder, other_span, result));
}

static void test_rover_script()
{
    gnss::RoverConfig cfg{};
    cfg.rate_hz = 5;
    cfg.gsv = true;
    cfg.port_baud = 921600;

    gnss::ConfigStep step{};
    TEST_ASSERT_TRUE(gnss::ublox_receiver().rover_step(0, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_TRUE(step.expect_reply);
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x30210001U, 200U, 2));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x10730004U, 1U, 1));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x10740002U, 1U, 1));

    TEST_ASSERT_TRUE(gnss::ublox_receiver().rover_step(1, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x209100BBU, 1U, 1));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x209100ACU, 1U, 1));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x209100BFU, 1U, 1));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x209100D4U, 1U, 1));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x209100C3U, 1U, 1));
    TEST_ASSERT_TRUE(valid_fletcher(Bytes(step.cmd.view().begin(), step.cmd.view().end())));

    TEST_ASSERT_TRUE(gnss::ublox_receiver().rover_step(2, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_FALSE(step.expect_reply);
    TEST_ASSERT_EQUAL_UINT32(921600, step.switch_baud);
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x40520001U, 921600U, 4));

    gnss::ConfigStep none{};
    TEST_ASSERT_FALSE(gnss::ublox_receiver().rover_step(3, gnss::Model::kZedF9p, cfg, none));

    cfg.gsv = false;
    cfg.port_baud = 0;
    TEST_ASSERT_TRUE(gnss::ublox_receiver().rover_step(1, gnss::Model::kZedF9p, cfg, step));
    TEST_ASSERT_TRUE(has_item(step.cmd, 0x209100C3U, 0U, 1));
    TEST_ASSERT_FALSE(gnss::ublox_receiver().rover_step(2, gnss::Model::kZedF9p, cfg, step));
}

static void test_match_reply()
{
    const Bytes ack = make_frame(0x05, 0x01, Bytes{0x06, 0x8A});
    std::span<const uint8_t> ack_span(ack.data(), ack.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kAck),
                      static_cast<int>(gnss::ublox_receiver().classify_reply(demux::ubx::kDecoder, ack_span,
                                                                             std::span<const uint8_t>())));

    const Bytes nak = make_frame(0x05, 0x00, Bytes{0x06, 0x8A});
    std::span<const uint8_t> nak_span(nak.data(), nak.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNack),
                      static_cast<int>(gnss::ublox_receiver().classify_reply(demux::ubx::kDecoder, nak_span,
                                                                             std::span<const uint8_t>())));

    const Bytes other = make_frame(0x05, 0x01, Bytes{0x06, 0x01});
    std::span<const uint8_t> other_span(other.data(), other.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(gnss::ublox_receiver().classify_reply(demux::ubx::kDecoder, other_span,
                                                                             std::span<const uint8_t>())));

    const Bytes nmea{0x24, 0x47, 0x4E, 0x47, 0x47, 0x41};
    std::span<const uint8_t> nmea_span(nmea.data(), nmea.size());
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(gnss::ublox_receiver().classify_reply(demux::ubx::kDecoder, nmea_span,
                                                                             std::span<const uint8_t>())));
}

static void test_caps()
{
    TEST_ASSERT_BITS(gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kRate20Hz,
                     gnss::cap::kRtcmIn | gnss::cap::kBase | gnss::cap::kRate20Hz,
                     gnss::ublox_receiver().caps(gnss::Model::kZedF9p));
    TEST_ASSERT_BITS(gnss::cap::kHeading, 0U, gnss::ublox_receiver().caps(gnss::Model::kZedF9p));
}

// ---- 黄金向量比对（ADR-0012 P3a 第 3 步）：与基线 4cf1320 上未修改的方言实现逐字节一致 ----
#include "GoldenCheck.hpp"
#include "GoldenUblox.inc"

static void test_golden_probe()
{
    TEST_ASSERT_TRUE(golden_check::probe_matches(gnss::ublox_receiver(), golden_Ublox::kProbe));
}

static void test_golden_rover_steps()
{
    const size_t bad = golden_check::steps_mismatch(gnss::ublox_receiver(), golden_Ublox::kRover, golden_Ublox::kSteps);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_probe_match()
{
    const size_t bad = golden_check::probe_match_mismatch(gnss::ublox_receiver(), golden_Ublox::kProbeMatch, nullptr);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_replies()
{
    const size_t bad = golden_check::reply_mismatch(gnss::ublox_receiver(), golden_Ublox::kReply, nullptr);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(golden_check::kAllMatch), static_cast<uint32_t>(bad));
}

static void test_golden_caps()
{
    const size_t bad = golden_check::caps_mismatch(gnss::ublox_receiver(), golden_Ublox::kCaps);
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
    RUN_TEST(test_build_probe);
    RUN_TEST(test_match_probe);
    RUN_TEST(test_rover_script);
    RUN_TEST(test_match_reply);
    RUN_TEST(test_caps);
    return UNITY_END();
}
