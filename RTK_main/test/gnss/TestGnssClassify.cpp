// 帧归类与帧标签的等价性测试（ADR-0012 §2.2）：classify_frame() 必须与 P3a 之前 main 的
// classify_gnss_sentence() 判定一致，frame_tag() 必须与 main 原 describe_frame() 的文本一致。
// 逐条用例的内联期望值来自基线 main/data/GnssOutput.cpp 与 main/modules/gnss/GnssModule.cpp 的实现。
// 线程/ISR 安全：本文件只调用纯函数。
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/decoders/Ubx.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "gnss/NullReceiver.hpp"
#include "gnss/Receiver.hpp"
#include "unity.h"

#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
#include "gnss/drivers/UnicoreReceiver.hpp"
constexpr bool kHasUnicore = true;
#else
constexpr bool kHasUnicore = false;
#endif

#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
#include "gnss/drivers/UbloxReceiver.hpp"
constexpr bool kHasUblox = true;
#else
constexpr bool kHasUblox = false;
#endif

#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
#include "gnss/drivers/ComnavReceiver.hpp"
#endif

namespace {

std::span<const uint8_t> bytes_of(std::string_view text)
{
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

/** 用指定驱动归类的期望值比较。 */
bool classifies_as(const gnss::Receiver &receiver, const demux::Decoder &dec, std::string_view frame,
                   gnss::OutputMsg expect)
{
    return receiver.classify_frame(dec, bytes_of(frame)) == expect;
}

/** 帧标签文本比较（按写入长度比较，避免依赖缓冲尾部）。 */
bool tag_is(const gnss::Receiver &receiver, const demux::Decoder &dec, std::string_view frame, const char *expect)
{
    char tag[32] = {};
    const size_t len = receiver.frame_tag(dec, bytes_of(frame), tag);
    const std::string_view got(tag, len);
    return got == expect;
}

} // namespace

void setUp() {}
void tearDown() {}

// 通用 NMEA：忽略 talker，只认 5 类 App 报文；其余（ZDA/VTG/GLL…）为 kNone。
static void test_nmea_classification()
{
    struct Case {
        const char *frame;
        gnss::OutputMsg expect;
    };
    // '$' + talker(2) + 名称(3) + ','：位置 3..5 是语句名。
    const Case cases[] = {
        {"$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", gnss::OutputMsg::kGga},
        {"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", gnss::OutputMsg::kGga},
        {"$GNGST,123519,2.3,1.1,0.9,45.2,1.0,0.7,1.1*6F\r\n", gnss::OutputMsg::kGst},
        {"$GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n", gnss::OutputMsg::kRmc},
        {"$GNGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39\r\n", gnss::OutputMsg::kGsa},
        {"$GPGSV,3,1,11,03,03,111,00,04,15,270,00*72\r\n", gnss::OutputMsg::kGsv},
        {"$GNZDA,123519.00,23,03,1994,00,00*4F\r\n", gnss::OutputMsg::kNone},
        {"$GNVTG,054.7,T,034.4,M,005.5,N,010.2,K*48\r\n", gnss::OutputMsg::kNone},
        {"not a sentence at all", gnss::OutputMsg::kNone},
        {"", gnss::OutputMsg::kNone},
    };
    // 每个启用的驱动都必须给出同样的通用 NMEA 判定（基类默认实现）。
    const gnss::Receiver *drivers[3] = {&gnss::null_receiver(), &gnss::null_receiver(), &gnss::null_receiver()};
    size_t count = 0;
#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
    drivers[count++] = &gnss::unicore_receiver();
#endif
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    drivers[count++] = &gnss::ublox_receiver();
#endif
#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
    drivers[count++] = &gnss::comnav_receiver();
#endif
    TEST_ASSERT_GREATER_THAN_UINT32(0, static_cast<uint32_t>(count));
    for (size_t d = 0; d < count; ++d) {
        for (const Case &item : cases) {
            TEST_ASSERT_TRUE(classifies_as(*drivers[d], demux::nmea::kDecoder, item.frame, item.expect));
        }
    }
}

// 厂家私有帧：只有 Unicore 认 #SATSINFOA / #HEADINGA，其余驱动一律 kNone。
static void test_vendor_frames()
{
    const std::string_view sats = "#SATSINFOA,97,GPS,FINE,2281,1,0,0,18,10;10,20,30*12345678";
    const std::string_view heading = "#HEADINGA,97,GPS,FINE,2281,1,0,0,18,10;SOL_COMPUTED,12*12345678";
    const std::string_view other = "#VERSIONA,96,GPS*12345678";

    if (kHasUnicore) {
        TEST_ASSERT_TRUE(
            classifies_as(gnss::unicore_receiver(), demux::unicore_hash::kDecoder, sats, gnss::OutputMsg::kSatsInfo));
        TEST_ASSERT_TRUE(
            classifies_as(gnss::unicore_receiver(), demux::unicore_hash::kDecoder, heading, gnss::OutputMsg::kHeading));
        TEST_ASSERT_TRUE(
            classifies_as(gnss::unicore_receiver(), demux::unicore_hash::kDecoder, other, gnss::OutputMsg::kNone));
    }
    if (kHasUblox) {
        TEST_ASSERT_TRUE(
            classifies_as(gnss::ublox_receiver(), demux::unicore_hash::kDecoder, sats, gnss::OutputMsg::kNone));
    }
    // 未识别驱动（NullReceiver）对所有帧都是 kNone，保证探测失败后不会误投数据。
    TEST_ASSERT_TRUE(classifies_as(gnss::null_receiver(), demux::nmea::kDecoder, sats, gnss::OutputMsg::kNone));
}

// frame_tag：与 main 原 describe_frame() 文本一致（NMEA 五位名 / RTCM3 消息号 / UBX class-id / 未知名）。
static void test_frame_tags()
{
    const std::string_view gga = "$GNGGA,123519,4807.038,N*47\r\n";
    const std::string_view unknown_nmea = "$GNZDA,123519.00,23,03,1994,00,00*4F\r\n";
    // 0xD3 0x00 0x05 → 长度 5，消息号 1005（0x3ED）。
    const uint8_t rtcm[] = {0xD3, 0x00, 0x05, 0x3E, 0xD0, 0x00, 0x00, 0x00, 0x00};
    // UBX 同步字 0xB5 0x62 + class 0x06 + id 0x8A。
    const uint8_t ubx[] = {0xB5, 0x62, 0x06, 0x8A, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};

    const gnss::Receiver &base = gnss::null_receiver();
    if (kHasUnicore) {
        TEST_ASSERT_TRUE(tag_is(gnss::unicore_receiver(), demux::nmea::kDecoder, gga, "NMEA GNGGA"));
        TEST_ASSERT_TRUE(tag_is(gnss::unicore_receiver(), demux::nmea::kDecoder, unknown_nmea, "NMEA GNZDA"));
        TEST_ASSERT_TRUE(tag_is(gnss::unicore_receiver(), demux::rtcm3::kDecoder,
                                std::string_view(reinterpret_cast<const char *>(rtcm), sizeof(rtcm)), "RTCM3 1005"));
        TEST_ASSERT_TRUE(tag_is(gnss::unicore_receiver(), demux::ubx::kDecoder,
                                std::string_view(reinterpret_cast<const char *>(ubx), sizeof(ubx)), "UBX 0x06/0x8a"));
        const std::string_view sats = "#SATSINFOA,97,GPS*12345678";
        TEST_ASSERT_TRUE(tag_is(gnss::unicore_receiver(), demux::unicore_hash::kDecoder, sats, "UNICORE SATSINFOA"));
        const std::string_view reply = "$command,UNLOG COM2,response: OK*1F\r\n";
        TEST_ASSERT_TRUE(
            tag_is(gnss::unicore_receiver(), gnss::unicore::kResponseDecoder, reply, "UNICORE_CMD UNLOG COM2"));
    }
    if (kHasUblox) {
        TEST_ASSERT_TRUE(tag_is(gnss::ublox_receiver(), demux::nmea::kDecoder, gga, "NMEA GNGGA"));
    }
    // NullReceiver 不产标签（main 在未探测到时不打印帧内容）。
    char tag[32] = {};
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(base.frame_tag(demux::nmea::kDecoder, bytes_of(gga), tag)));
}

// 报文掩码：默认 GGA|RMC|GSA|GSV；Unicore 追加 SATSINFOA，双天线型号再追加 HEADINGA。
static void test_output_masks()
{
    const uint32_t base_mask = (1U << 0) | (1U << 4) | (1U << 5) | (1U << 6); // GGA|RMC|GSA|GSV
    TEST_ASSERT_EQUAL_UINT32(0, gnss::null_receiver().output_mask(gnss::Model::kUm982));
    if (kHasUnicore) {
        const uint32_t single = gnss::unicore_receiver().output_mask(gnss::Model::kUm980);
        TEST_ASSERT_EQUAL_UINT32(base_mask | (1U << 1), single); // + kSatsInfo
        const uint32_t dual = gnss::unicore_receiver().output_mask(gnss::Model::kUm982);
        TEST_ASSERT_EQUAL_UINT32(base_mask | (1U << 1) | (1U << 2), dual); // + kHeading
    }
    if (kHasUblox) {
        TEST_ASSERT_EQUAL_UINT32(base_mask, gnss::ublox_receiver().output_mask(gnss::Model::kZedF9p));
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_nmea_classification);
    RUN_TEST(test_vendor_frames);
    RUN_TEST(test_frame_tags);
    RUN_TEST(test_output_masks);
    return UNITY_END();
}
