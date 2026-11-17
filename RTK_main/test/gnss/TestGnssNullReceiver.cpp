// NullReceiver host 测试（ADR-0012 §2.2/§2.3）：探测全表失败时的占位驱动必须不产生字节、不认任何帧，
// 让 main 无需判空即可安全调用全部接口。
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/decoders/Nmea.hpp"
#include "gnss/NullReceiver.hpp"
#include "unity.h"

void setUp() {}
void tearDown() {}

static void testIdentity()
{
    const gnss::Receiver &nullReceiver = gnss::nullReceiver();
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnknown),
                      static_cast<int>(nullReceiver.family()));
    TEST_ASSERT_EQUAL_STRING("null", nullReceiver.name().data());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(nullReceiver.decoders().size()));
    TEST_ASSERT_EQUAL_UINT32(0, nullReceiver.caps(gnss::Model::kUm982));
    TEST_ASSERT_EQUAL_UINT32(0, nullReceiver.outputMask(gnss::Model::kUm982));
    // 同一实例：main 按地址比较驱动身份，nullReceiver() 必须稳定。
    TEST_ASSERT_EQUAL_PTR(&gnss::nullReceiver(), &nullReceiver);
}

static void testNoBytesAndNoFrames()
{
    const gnss::Receiver &nullReceiver = gnss::nullReceiver();

    gnss::CmdBuf cmd;
    TEST_ASSERT_FALSE(nullReceiver.buildProbe(cmd));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cmd.len));

    gnss::ProbeResult probe{};
    const uint8_t version[] = "#VERSIONA,96,GPS*12";
    TEST_ASSERT_FALSE(nullReceiver.matchProbe(demux::nmea::kDecoder, version, probe));

    gnss::RoverConfig cfg{};
    gnss::ConfigStep step{};
    TEST_ASSERT_FALSE(nullReceiver.roverStep(0, gnss::Model::kUm982, cfg, step));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(step.cmd.len));

    const uint8_t sent[] = "MODE ROVER\r\n";
    const uint8_t reply[] = "$command,MODE ROVER,response: OK*1F\r\n";
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Ack::kNone),
        static_cast<int>(nullReceiver.classifyReply(demux::nmea::kDecoder, reply, sent)));

    gnss::OutputPlan plan{};
    TEST_ASSERT_EQUAL(static_cast<int>(fw::Status::kNotSupported),
                      static_cast<int>(nullReceiver.buildOutput(gnss::OutputMsg::kGga, 1000U,
                                                                gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cmd.len));

    TEST_ASSERT_EQUAL(static_cast<int>(gnss::OutputMsg::kNone),
                      static_cast<int>(nullReceiver.classifyFrame(demux::nmea::kDecoder, reply)));

    char tag[32] = {};
    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(nullReceiver.frameTag(demux::nmea::kDecoder, reply, tag)));
    TEST_ASSERT_FALSE(nullReceiver.buildReadback(cmd));
    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(nullReceiver.matchReadback(demux::nmea::kDecoder, reply, tag)));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testIdentity);
    RUN_TEST(testNoBytesAndNoFrames);
    return UNITY_END();
}
