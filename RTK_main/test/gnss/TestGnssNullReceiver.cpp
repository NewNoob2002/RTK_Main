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

static void test_identity()
{
    const gnss::Receiver &null_receiver = gnss::null_receiver();
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnknown), static_cast<int>(null_receiver.family()));
    TEST_ASSERT_EQUAL_STRING("null", null_receiver.name().data());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(null_receiver.decoders().size()));
    TEST_ASSERT_EQUAL_UINT32(0, null_receiver.caps(gnss::Model::kUm982));
    TEST_ASSERT_EQUAL_UINT32(0, null_receiver.output_mask(gnss::Model::kUm982));
    // 同一实例：main 按地址比较驱动身份，null_receiver() 必须稳定。
    TEST_ASSERT_EQUAL_PTR(&gnss::null_receiver(), &null_receiver);
}

static void test_no_bytes_and_no_frames()
{
    const gnss::Receiver &null_receiver = gnss::null_receiver();

    gnss::CmdBuf cmd;
    TEST_ASSERT_FALSE(null_receiver.build_probe(cmd));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cmd.len));

    gnss::ProbeResult probe{};
    const uint8_t version[] = "#VERSIONA,96,GPS*12";
    TEST_ASSERT_FALSE(null_receiver.match_probe(demux::nmea::kDecoder, version, probe));

    gnss::RoverConfig cfg{};
    gnss::ConfigStep step{};
    TEST_ASSERT_FALSE(null_receiver.rover_step(0, gnss::Model::kUm982, cfg, step));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(step.cmd.len));

    const uint8_t sent[] = "MODE ROVER\r\n";
    const uint8_t reply[] = "$command,MODE ROVER,response: OK*1F\r\n";
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Ack::kNone),
                      static_cast<int>(null_receiver.classify_reply(demux::nmea::kDecoder, reply, sent)));

    gnss::OutputPlan plan{};
    TEST_ASSERT_EQUAL(
        static_cast<int>(fw::Status::kNotSupported),
        static_cast<int>(null_receiver.build_output(gnss::OutputMsg::kGga, 1000U, gnss::Model::kUm982, plan, cmd)));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cmd.len));

    TEST_ASSERT_EQUAL(static_cast<int>(gnss::OutputMsg::kNone),
                      static_cast<int>(null_receiver.classify_frame(demux::nmea::kDecoder, reply)));

    char tag[32] = {};
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(null_receiver.frame_tag(demux::nmea::kDecoder, reply, tag)));
    TEST_ASSERT_FALSE(null_receiver.build_readback(cmd));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(null_receiver.match_readback(demux::nmea::kDecoder, reply, tag)));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity);
    RUN_TEST(test_no_bytes_and_no_frames);
    return UNITY_END();
}
