// 帧归类与帧标签的等价性测试（ADR-0012 §2.2）：classifyFrame() 必须与 旧版 main 的
// classify_gnss_sentence() 判定一致，frameTag() 必须与 main 原 describe_frame() 的文本一致。
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
#endif

#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
#include "gnss/drivers/UbloxReceiver.hpp"
#endif

#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
#include "gnss/drivers/ComnavReceiver.hpp"
#endif

namespace {

// 驱动访问器已按 ADR-0013 §2.3 删除；测试经注册表取基准叶子实例。
#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
const gnss::Receiver &unicoreDrv() noexcept
{
    return *gnss::receiverFor(gnss::Family::kUnicore);
}
#endif
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

std::span<const uint8_t> bytesOf(std::string_view text)
{
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

/** 用指定驱动归类的期望值比较。 */
bool classifiesAs(const gnss::Receiver &receiver, const demux::Decoder &dec, std::string_view frame,
                  gnss::OutputMsg expect)
{
    return receiver.classifyFrame(dec, bytesOf(frame)) == expect;
}

#if (defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE) ||                                     \
    (defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX)
/** 帧标签文本比较（按写入长度比较，避免依赖缓冲尾部）。 */
bool tagIs(const gnss::Receiver &receiver, const demux::Decoder &dec, std::string_view frame,
           const char *expect)
{
    char tag[32] = {};
    const size_t len = receiver.frameTag(dec, bytesOf(frame), tag);
    const std::string_view got(tag, len);
    return got == expect;
}
#endif

} // namespace

void setUp() {}
void tearDown() {}

// 通用 NMEA：忽略 talker，只认 5 类 App 报文；其余（ZDA/VTG/GLL…）为 kNone。
static void testNmeaClassification()
{
    struct Case {
        const char *frame;
        gnss::OutputMsg expect;
    };
    // '$' + talker(2) + 名称(3) + ','：位置 3..5 是语句名。
    const Case cases[] = {
        {"$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n",
         gnss::OutputMsg::kGga},
        {"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n",
         gnss::OutputMsg::kGga},
        {"$GNGST,123519,2.3,1.1,0.9,45.2,1.0,0.7,1.1*6F\r\n", gnss::OutputMsg::kGst},
        {"$GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n",
         gnss::OutputMsg::kRmc},
        {"$GNGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39\r\n", gnss::OutputMsg::kGsa},
        {"$GPGSV,3,1,11,03,03,111,00,04,15,270,00*72\r\n", gnss::OutputMsg::kGsv},
        {"$GNZDA,123519.00,23,03,1994,00,00*4F\r\n", gnss::OutputMsg::kNone},
        {"$GNVTG,054.7,T,034.4,M,005.5,N,010.2,K*48\r\n", gnss::OutputMsg::kNone},
        {"not a sentence at all", gnss::OutputMsg::kNone},
        {"", gnss::OutputMsg::kNone},
    };
    // 每个启用的驱动都必须给出同样的通用 NMEA 判定（基类默认实现）。
    const gnss::Receiver *drivers[3] = {&gnss::nullReceiver(), &gnss::nullReceiver(),
                                        &gnss::nullReceiver()};
    size_t count = 0;
#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
    drivers[count++] = &unicoreDrv();
#endif
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    drivers[count++] = &ubloxDrv();
#endif
#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
    drivers[count++] = &comnavDrv();
#endif
#if (defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE) ||                                     \
    (defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX) ||                                         \
    (defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV)
    TEST_ASSERT_GREATER_THAN_UINT32(0, static_cast<uint32_t>(count));
#else
    // 全部驱动关闭：没有驱动可测通用 NMEA 判定（nullReceiver 对任何报文都是 kNone）。
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(count));
#endif
    for (size_t d = 0; d < count; ++d) {
        for (const Case &item : cases) {
            TEST_ASSERT_TRUE(
                classifiesAs(*drivers[d], demux::nmea::kDecoder, item.frame, item.expect));
        }
    }
}

// 厂家私有帧：只有 Unicore 认 #SATSINFOA / #HEADINGA，其余驱动一律 kNone。
static void testVendorFrames()
{
    const std::string_view sats = "#SATSINFOA,97,GPS,FINE,2281,1,0,0,18,10;10,20,30*12345678";

#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
    const std::string_view heading =
        "#HEADINGA,97,GPS,FINE,2281,1,0,0,18,10;SOL_COMPUTED,12*12345678";
    const std::string_view other = "#VERSIONA,96,GPS*12345678";
    TEST_ASSERT_TRUE(classifiesAs(unicoreDrv(), demux::unicore_hash::kDecoder, sats,
                                  gnss::OutputMsg::kSatsInfo));
    TEST_ASSERT_TRUE(classifiesAs(unicoreDrv(), demux::unicore_hash::kDecoder, heading,
                                  gnss::OutputMsg::kHeading));
    TEST_ASSERT_TRUE(
        classifiesAs(unicoreDrv(), demux::unicore_hash::kDecoder, other, gnss::OutputMsg::kNone));
#endif
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    TEST_ASSERT_TRUE(
        classifiesAs(ubloxDrv(), demux::unicore_hash::kDecoder, sats, gnss::OutputMsg::kNone));
#endif
    // 未识别驱动（NullReceiver）对所有帧都是 kNone，保证探测失败后不会误投数据。
    TEST_ASSERT_TRUE(
        classifiesAs(gnss::nullReceiver(), demux::nmea::kDecoder, sats, gnss::OutputMsg::kNone));
}

// frameTag：与 main 原 describe_frame() 文本一致（NMEA 五位名 / RTCM3 消息号 / UBX class-id / 未知名）。
static void testFrameTags()
{
    const std::string_view gga = "$GNGGA,123519,4807.038,N*47\r\n";

    const gnss::Receiver &base = gnss::nullReceiver();
#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
    const std::string_view unknownNmea = "$GNZDA,123519.00,23,03,1994,00,00*4F\r\n";
    // 0xD3 0x00 0x05 → 长度 5，消息号 1005（0x3ED）。
    const uint8_t rtcm[] = {0xD3, 0x00, 0x05, 0x3E, 0xD0, 0x00, 0x00, 0x00, 0x00};
    // UBX 同步字 0xB5 0x62 + class 0x06 + id 0x8A。
    const uint8_t ubx[] = {0xB5, 0x62, 0x06, 0x8A, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_TRUE(tagIs(unicoreDrv(), demux::nmea::kDecoder, gga, "NMEA GNGGA"));
    TEST_ASSERT_TRUE(tagIs(unicoreDrv(), demux::nmea::kDecoder, unknownNmea, "NMEA GNZDA"));
    TEST_ASSERT_TRUE(tagIs(unicoreDrv(), demux::rtcm3::kDecoder,
                           std::string_view(reinterpret_cast<const char *>(rtcm), sizeof(rtcm)),
                           "RTCM3 1005"));
    TEST_ASSERT_TRUE(tagIs(unicoreDrv(), demux::ubx::kDecoder,
                           std::string_view(reinterpret_cast<const char *>(ubx), sizeof(ubx)),
                           "UBX 0x06/0x8a"));
    const std::string_view sats = "#SATSINFOA,97,GPS*12345678";
    TEST_ASSERT_TRUE(tagIs(unicoreDrv(), demux::unicore_hash::kDecoder, sats, "UNICORE SATSINFOA"));
    const std::string_view reply = "$command,UNLOG COM2,response: OK*1F\r\n";
    TEST_ASSERT_TRUE(
        tagIs(unicoreDrv(), gnss::unicore::kResponseDecoder, reply, "UNICORE_CMD UNLOG COM2"));
#endif
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    TEST_ASSERT_TRUE(tagIs(ubloxDrv(), demux::nmea::kDecoder, gga, "NMEA GNGGA"));
#endif
    // NullReceiver 不产标签（main 在未探测到时不打印帧内容）。
    char tag[32] = {};
    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(base.frameTag(demux::nmea::kDecoder, bytesOf(gga), tag)));
}

// 报文掩码：默认 GGA|RMC|GSA|GSV；Unicore 追加 SATSINFOA 与 GPGST，双天线型号再追加 HEADINGA。
static void testOutputMasks()
{
    TEST_ASSERT_EQUAL_UINT32(0, gnss::nullReceiver().outputMask(gnss::Model::kUm982));
#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
    {
        const uint32_t baseMask = (1U << 0) | (1U << 4) | (1U << 5) | (1U << 6); // GGA|RMC|GSA|GSV
        const uint32_t single = unicoreDrv().outputMask(gnss::Model::kUm980);
        TEST_ASSERT_EQUAL_UINT32(baseMask | (1U << 1) | (1U << 3), single); // + kSatsInfo + kGst
        // HEADINGA 只在型号驱动上出现：调用方须先 forModel（ADR-0012 §2.8）。
        const uint32_t dual =
            unicoreDrv().forModel(gnss::Model::kUm982).outputMask(gnss::Model::kUm982);
        TEST_ASSERT_EQUAL_UINT32(baseMask | (1U << 1) | (1U << 2) | (1U << 3), dual); // + kHeading
    }
#endif
#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
    {
        const uint32_t baseMask = (1U << 0) | (1U << 4) | (1U << 5) | (1U << 6); // GGA|RMC|GSA|GSV
        TEST_ASSERT_EQUAL_UINT32(baseMask | (1U << 3), ubloxDrv().outputMask(gnss::Model::kZedF9p));
    }
#endif
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testNmeaClassification);
    RUN_TEST(testVendorFrames);
    RUN_TEST(testFrameTags);
    RUN_TEST(testOutputMasks);
    return UNITY_END();
}
