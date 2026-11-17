// GNSS NMEA 解析 host 测试：GGA/RMC/GSA/GSV、成轮发布、malformed 不改状态（ADR-0010 §2.5）。
// 纯 host 测试，无 IDF 依赖；使用 Unity。
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#include "gnss/NmeaParser.hpp"
#include "unity.h"

namespace {

std::string withChecksum(const std::string &body)
{
    unsigned checksum = 0;
    for (const char c : body) {
        checksum ^= static_cast<unsigned char>(c);
    }
    char tail[8] = {};
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", checksum);
    return "$" + body + tail;
}

gnss::NmeaResult feed(gnss::NmeaParser &parser, const std::string &body, uint32_t nowMs)
{
    const std::string sentence = withChecksum(body);
    return parser.feed(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(sentence.data()),
                                                sentence.size()),
                       nowMs);
}

const char *s_k_gga = "GNGGA,001043.00,4404.14036,N,12118.85961,E,1,12,0.98,111.20,M,-8.20,M,,";
const char *s_k_rmc = "GNRMC,001043.00,A,4404.14036,N,12118.85961,E,0.021,,010125,,,A";
const char *s_k_gsa = "GNGSA,A,3,01,02,03,,,,,,,,,,1.8,0.9,1.5,1";
const char *s_k_gsv1 = "GPGSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45";
const char *s_k_gsv2 = "GPGSV,2,2,08,15,35,120,44,18,20,045,40,20,10,200,33,24,05,310,30";

} // namespace

void setUp() {}
void tearDown() {}

static void testGgaFields()
{
    gnss::NmeaParser parser;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kFix),
                      static_cast<int>(feed(parser, s_k_gga, 1000)));

    const gnss::Fix &fix = parser.fix();
    TEST_ASSERT_TRUE(fix.valid);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::FixQuality::kSingle), static_cast<int>(fix.quality));
    TEST_ASSERT_EQUAL_INT64(44069006000LL, fix.latE9);
    TEST_ASSERT_EQUAL_INT64(121314326833LL, fix.lonE9);
    TEST_ASSERT_EQUAL_INT32(111200, fix.altMm);
    TEST_ASSERT_EQUAL_INT32(-8200, fix.geoidMm);
    TEST_ASSERT_EQUAL_UINT8(12, fix.satsUsed);
    TEST_ASSERT_EQUAL_UINT16(98, fix.hdopX100);
    TEST_ASSERT_EQUAL_UINT32(1000, fix.updateMs);
    TEST_ASSERT_EQUAL_UINT8(0, fix.utc.hour);
    TEST_ASSERT_EQUAL_UINT8(10, fix.utc.minute);
    TEST_ASSERT_EQUAL_UINT16(43000, fix.utc.ms);
    TEST_ASSERT_EQUAL_UINT8(1, parser.stats().gga);
}

static void testGgaSouthWestAndInvalidQuality()
{
    gnss::NmeaParser parser;
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::NmeaResult::kFix),
        static_cast<int>(
            feed(parser, "GNGGA,120000.00,3352.00000,S,15112.00000,W,0,3,1.0,5.0,M,0.0,M,,", 7)));
    TEST_ASSERT_FALSE(parser.fix().valid);
    TEST_ASSERT_EQUAL_INT64(-33866666667LL, parser.fix().latE9);
    TEST_ASSERT_EQUAL_INT64(-151200000000LL, parser.fix().lonE9);
}

static void testRmcDateSpeedCourse()
{
    gnss::NmeaParser parser;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kAux),
                      static_cast<int>(feed(parser, s_k_rmc, 2000)));
    const gnss::Fix &fix = parser.fix();
    TEST_ASSERT_EQUAL_UINT16(2025, fix.utc.year);
    TEST_ASSERT_EQUAL_UINT8(1, fix.utc.month);
    TEST_ASSERT_EQUAL_UINT8(1, fix.utc.day);
    TEST_ASSERT_EQUAL_UINT32(11, fix.speedMmps);
    TEST_ASSERT_EQUAL_UINT32(0, fix.updateMs); // RMC 不改 updateMs
    TEST_ASSERT_EQUAL_UINT8(1, parser.stats().rmc);
}

static void testGsvRoundPublishedOnNextGga()
{
    gnss::NmeaParser parser;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kSatsPart),
                      static_cast<int>(feed(parser, s_k_gsv1, 10)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kSatsPart),
                      static_cast<int>(feed(parser, s_k_gsv2, 10)));
    TEST_ASSERT_EQUAL_UINT8(0, parser.sats().count); // 尚未到 GGA，不发布

    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kSatsReady),
                      static_cast<int>(feed(parser, s_k_gga, 20)));
    TEST_ASSERT_EQUAL_UINT8(8, parser.sats().count);
    TEST_ASSERT_EQUAL_UINT32(20, parser.sats().updateMs);
    TEST_ASSERT_EQUAL_UINT32(20, parser.fix().updateMs); // 同一 GGA 也更新了定位
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Constellation::kGps),
                      static_cast<int>(parser.sats().sats[0].system));

    // 发布后 pending 清空：再来一条 GGA 不再发布卫星。
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kFix),
                      static_cast<int>(feed(parser, s_k_gga, 30)));
    TEST_ASSERT_EQUAL_UINT32(30, parser.fix().updateMs);
    TEST_ASSERT_EQUAL_UINT32(20, parser.sats().updateMs);
}

static void testGsaUsedMarksMerged()
{
    gnss::NmeaParser parser;
    (void)feed(parser, s_k_gsa, 5);
    (void)feed(parser, s_k_gsv1, 6);
    (void)feed(parser, s_k_gsv2, 6);
    TEST_ASSERT_EQUAL_UINT16(180, parser.fix().pdopX100);
    TEST_ASSERT_EQUAL_UINT16(90, parser.fix().hdopX100);
    TEST_ASSERT_EQUAL_UINT16(150, parser.fix().vdopX100);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kSatsReady),
                      static_cast<int>(feed(parser, s_k_gga, 7)));

    const gnss::SatTable &sats = parser.sats();
    TEST_ASSERT_EQUAL_UINT8(8, sats.count);
    for (size_t i = 0; i < sats.count; ++i) {
        const bool expected =
            sats.sats[i].prn == 1 || sats.sats[i].prn == 2 || sats.sats[i].prn == 3;
        TEST_ASSERT_EQUAL(expected, sats.sats[i].used);
    }
}

static void testHalfRoundNotPublished()
{
    gnss::NmeaParser parser;
    (void)feed(parser, s_k_gsv1, 10); // 1/2，缺第 2 条
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kFix),
                      static_cast<int>(feed(parser, s_k_gga, 20)));
    TEST_ASSERT_EQUAL_UINT8(0, parser.sats().count);
}

static void testMalformedKeepsState()
{
    gnss::NmeaParser parser;
    (void)feed(parser, s_k_gga, 100);
    const gnss::Fix before = parser.fix();
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::NmeaResult::kMalformed),
        static_cast<int>(feed(
            parser, "GNGGA,001043.00,4404.14036,N,12118.85961,E,x,12,0.98,1.0,M,0.0,M,,", 200)));
    TEST_ASSERT_EQUAL_UINT32(before.updateMs, parser.fix().updateMs);
    TEST_ASSERT_EQUAL_INT32(before.altMm, parser.fix().altMm);
    TEST_ASSERT_EQUAL_UINT32(1, parser.stats().malformed);
    TEST_ASSERT_EQUAL_UINT32(1, parser.stats().gga);
}

static void testIgnoredSentenceAndReset()
{
    gnss::NmeaParser parser;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kIgnored),
                      static_cast<int>(feed(parser, "GNVTG,0.0,T,,M,0.0,N,0.0,K,A", 1)));
    TEST_ASSERT_EQUAL_UINT32(1, parser.stats().ignored);

    (void)feed(parser, s_k_gsv1, 2);
    (void)feed(parser, s_k_gsv2, 2);
    (void)feed(parser, s_k_gga, 3);
    TEST_ASSERT_EQUAL_UINT8(8, parser.sats().count);
    parser.reset();
    TEST_ASSERT_EQUAL_UINT8(0, parser.sats().count);
    TEST_ASSERT_EQUAL_UINT32(0, parser.fix().updateMs);
    TEST_ASSERT_EQUAL_UINT32(2, parser.stats().gsv); // reset 不清统计
}

static void testTalkerConstellationMapping()
{
    gnss::NmeaParser parser;
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::NmeaResult::kSatsPart),
                      static_cast<int>(feed(parser, "GLGSV,1,1,01,65,40,083,46", 1)));
    (void)feed(parser, s_k_gga, 2);
    TEST_ASSERT_EQUAL_UINT8(1, parser.sats().count);
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Constellation::kGlonass),
                      static_cast<int>(parser.sats().sats[0].system));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testGgaFields);
    RUN_TEST(testGgaSouthWestAndInvalidQuality);
    RUN_TEST(testRmcDateSpeedCourse);
    RUN_TEST(testGsvRoundPublishedOnNextGga);
    RUN_TEST(testGsaUsedMarksMerged);
    RUN_TEST(testHalfRoundNotPublished);
    RUN_TEST(testMalformedKeepsState);
    RUN_TEST(testIgnoredSentenceAndReset);
    RUN_TEST(testTalkerConstellationMapping);
    return UNITY_END();
}
