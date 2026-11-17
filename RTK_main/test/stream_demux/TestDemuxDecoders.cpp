// 内置解码器黑盒测试（ADR-0005 / ADR-0004 §4，覆盖 5 个通用 GNSS 解码器 + 产品帧解码器与视图）。

#ifndef STREAM_DEMUX_RTCM3
#define STREAM_DEMUX_RTCM3 1
#endif
#ifndef STREAM_DEMUX_NMEA
#define STREAM_DEMUX_NMEA 1
#endif
#ifndef STREAM_DEMUX_UBX
#define STREAM_DEMUX_UBX 1
#endif
#ifndef STREAM_DEMUX_UNICORE_BIN
#define STREAM_DEMUX_UNICORE_BIN 1
#endif
#ifndef STREAM_DEMUX_UNICORE_HASH
#define STREAM_DEMUX_UNICORE_HASH 1
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "demux/Demux.hpp"
#if STREAM_DEMUX_NMEA
#include "demux/decoders/Nmea.hpp"
#endif
#if STREAM_DEMUX_RTCM3
#include "demux/decoders/Rtcm3.hpp"
#endif
#if STREAM_DEMUX_UBX
#include "demux/decoders/Ubx.hpp"
#endif
#if STREAM_DEMUX_UNICORE_BIN
#include "demux/decoders/UnicoreBin.hpp"
#endif
#if STREAM_DEMUX_UNICORE_HASH
#include "demux/decoders/UnicoreHash.hpp"
#endif
#include "DemuxGen.hpp"
#include "proto/DemuxFrame.hpp"
#include "proto/Frame.hpp"
#include "unity.h"

namespace {

constexpr std::size_t kDemuxBufSize = 4096U;
constexpr std::size_t kWorkBufSize = 4200U;
constexpr std::size_t kMaxDecFrames = 16U;
constexpr std::size_t kMaxUnkCap = 4096U;

struct DecodedFrame {
    const demux::Decoder *decoder = nullptr;
    uint8_t decoderIndex = 0U;
    std::size_t len = 0U;
    std::array<uint8_t, kDemuxBufSize> data{};
};

class DecoderSink final : public demux::Sink {
public:
    void reset() noexcept
    {
        frameCount = 0U;
        unknownLen = 0U;
    }

    void onFrame(const demux::Decoder &decoder, uint8_t index,
                 std::span<const uint8_t> frame) noexcept override
    {
        if (frameCount < kMaxDecFrames) {
            DecodedFrame &dst = frames[frameCount++];
            dst.decoder = &decoder;
            dst.decoderIndex = index;
            dst.len = frame.size();
            if (frame.size() <= dst.data.size()) {
                std::copy(frame.begin(), frame.end(), dst.data.begin());
            }
        }
    }

    void onUnknown(std::span<const uint8_t> bytes) noexcept override
    {
        if ((unknownLen + bytes.size()) <= unknown.size()) {
            std::copy(bytes.begin(), bytes.end(),
                      unknown.begin() + static_cast<std::ptrdiff_t>(unknownLen));
        }
        unknownLen += bytes.size();
    }

    std::array<DecodedFrame, kMaxDecFrames> frames{};
    std::size_t frameCount = 0U;
    std::array<uint8_t, kMaxUnkCap> unknown{};
    std::size_t unknownLen = 0U;
};

std::array<uint8_t, kDemuxBufSize> s_demux_buf{};
std::array<uint8_t, kWorkBufSize> s_work_buf{};
demux::Demux s_dmx;
DecoderSink s_sink;

void runDemuxFeedAndAbort(std::span<const demux::Decoder *const> decs,
                          std::span<const uint8_t> data)
{
    s_sink.reset();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(decs, s_demux_buf, s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(data));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
}

// 按解码器 need 契约驱动 step 直至出现 kFrame / kReject / 耗尽输入
demux::Step stepByNeed(const demux::Decoder &dec, std::span<const uint8_t> buf,
                       std::size_t *outEndPos, std::size_t *outCalls)
{
    std::size_t pos = 1U;
    std::size_t calls = 0U;

    TEST_ASSERT_TRUE(!buf.empty());
    TEST_ASSERT_TRUE(dec.firstByte(buf[0]));

    for (;;) {
        std::size_t need = 0U;
        const demux::Step st = dec.step(buf.subspan(0, pos), need);
        calls++;
        if (st != demux::Step::kNeedMore) {
            if (outEndPos != nullptr) {
                *outEndPos = pos;
            }
            if (outCalls != nullptr) {
                *outCalls = calls;
            }
            return st;
        }
        TEST_ASSERT_TRUE(need >= 1U);
        TEST_ASSERT_TRUE(need <= dec.maxFrame);
        TEST_ASSERT_TRUE((pos + need) <= dec.maxFrame);
        if ((pos + need) > buf.size()) {
            if (outEndPos != nullptr) {
                *outEndPos = pos;
            }
            if (outCalls != nullptr) {
                *outCalls = calls;
            }
            return demux::Step::kNeedMore;
        }
        pos += need;
    }
}

void assertOnlyFirstByte(const demux::Decoder &dec, uint8_t expected)
{
    for (uint32_t b = 0U; b <= 0xFFU; ++b) {
        // b <= 0xFFU，在 uint8_t 范围内
        const bool got = dec.firstByte(static_cast<uint8_t>(b));
        if (b == static_cast<uint32_t>(expected)) {
            TEST_ASSERT_TRUE(got);
        } else {
            TEST_ASSERT_FALSE(got);
        }
    }
}

#if STREAM_DEMUX_NMEA || STREAM_DEMUX_UNICORE_HASH
void copySvToSpan(std::string_view sv, std::span<uint8_t> dst) noexcept
{
    for (std::size_t i = 0U; i < sv.size(); ++i) {
        // ASCII 字符位模式按 0..255 解释
        dst[i] = static_cast<uint8_t>(sv[i]);
    }
}
#endif

#if STREAM_DEMUX_NMEA
// 自定义 '$' 同首字节解码器（模拟 SBF $@ 6 字节短帧），用于测试 NMEA 与同首字节 '$' 冲突
bool customSbfFirst(uint8_t byte) noexcept
{
    return byte == static_cast<uint8_t>('$');
}

demux::Step customSbfStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 2U) {
        need = 1U;
        return demux::Step::kNeedMore;
    }
    if (frame[1] != static_cast<uint8_t>('@')) {
        return demux::Step::kReject;
    }
    if (frame.size() < 6U) {
        need = 6U - frame.size();
        return demux::Step::kNeedMore;
    }
    return demux::Step::kFrame;
}

constexpr demux::Decoder kDecCustomSbf{"custom_sbf", 6U, &customSbfFirst, &customSbfStep};
#endif

} // namespace

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

// --------------------------------------------------------------------------
// 1. 描述符元数据与首字节全覆盖
// --------------------------------------------------------------------------

static void testDecoderDescriptorsAndFirstBytes()
{
#if STREAM_DEMUX_RTCM3
    TEST_ASSERT_NOT_NULL(demux::rtcm3::kDecoder.name);
    TEST_ASSERT_EQUAL_UINT(1029U, demux::rtcm3::kDecoder.maxFrame);
    TEST_ASSERT_EQUAL_UINT(demux::rtcm3::kMaxFrame, demux::rtcm3::kDecoder.maxFrame);
    assertOnlyFirstByte(demux::rtcm3::kDecoder, 0xD3U);
#endif

#if STREAM_DEMUX_NMEA
    TEST_ASSERT_NOT_NULL(demux::nmea::kDecoder.name);
    TEST_ASSERT_EQUAL_UINT(256U, demux::nmea::kDecoder.maxFrame);
    TEST_ASSERT_EQUAL_UINT(demux::nmea::kMaxFrame, demux::nmea::kDecoder.maxFrame);
    assertOnlyFirstByte(demux::nmea::kDecoder, static_cast<uint8_t>('$'));
#endif

#if STREAM_DEMUX_UBX
    TEST_ASSERT_NOT_NULL(demux::ubx::kDecoder.name);
    TEST_ASSERT_EQUAL_UINT(4096U, demux::ubx::kDecoder.maxFrame);
    TEST_ASSERT_EQUAL_UINT(demux::ubx::kMaxFrame, demux::ubx::kDecoder.maxFrame);
    assertOnlyFirstByte(demux::ubx::kDecoder, 0xB5U);
#endif

#if STREAM_DEMUX_UNICORE_BIN
    TEST_ASSERT_NOT_NULL(demux::unicore_bin::kDecoder.name);
    TEST_ASSERT_EQUAL_UINT(4096U, demux::unicore_bin::kDecoder.maxFrame);
    TEST_ASSERT_EQUAL_UINT(demux::unicore_bin::kMaxFrame, demux::unicore_bin::kDecoder.maxFrame);
    assertOnlyFirstByte(demux::unicore_bin::kDecoder, 0xAAU);
#endif

#if STREAM_DEMUX_UNICORE_HASH
    TEST_ASSERT_NOT_NULL(demux::unicore_hash::kDecoder.name);
    TEST_ASSERT_EQUAL_UINT(4096U, demux::unicore_hash::kDecoder.maxFrame);
    TEST_ASSERT_EQUAL_UINT(demux::unicore_hash::kMaxFrame, demux::unicore_hash::kDecoder.maxFrame);
    assertOnlyFirstByte(demux::unicore_hash::kDecoder, static_cast<uint8_t>('#'));
#endif

    TEST_ASSERT_NOT_NULL(proto::demux_frame::kDecoder.name);
    TEST_ASSERT_EQUAL_UINT(DEV_PROTO_MAX_FRAME, proto::demux_frame::kDecoder.maxFrame);
    TEST_ASSERT_EQUAL_UINT(proto::demux_frame::kMaxFrame, proto::demux_frame::kDecoder.maxFrame);
    assertOnlyFirstByte(proto::demux_frame::kDecoder, proto::kSyncA);
}

// --------------------------------------------------------------------------
// 2. RTCM3
// --------------------------------------------------------------------------

#if STREAM_DEMUX_RTCM3
static void testRtcm3ValidEarlyRejectAndCrcErr()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&demux::rtcm3::kDecoder};
    static constexpr std::array<uint8_t, 8> kBadB1{0x04U, 0x08U, 0x10U, 0x20U,
                                                   0x40U, 0x80U, 0xFCU, 0xFFU};
    static std::array<uint8_t, demux_test::kRtcm3MaxPayload> sMaxPl{};
    std::array<uint8_t, 19> rtcm1005{};
    std::size_t flen = 0U;
    std::size_t endPos = 0U;
    const std::span<uint8_t> work(s_work_buf);

    // 0 载荷（6 字节）
    TEST_ASSERT_TRUE(demux_test::genRtcm3(work, {}, flen));
    TEST_ASSERT_EQUAL(demux::Step::kFrame,
                      stepByNeed(demux::rtcm3::kDecoder, work.subspan(0, flen), &endPos, nullptr));
    TEST_ASSERT_EQUAL_UINT(6U, endPos);

    // 典型 RTCM 1005 载荷（消息号 1005 = 0x3ED，19 字节）
    rtcm1005.fill(0x3AU);
    rtcm1005[0] = 0x3EU;
    rtcm1005[1] = 0xD0U;
    TEST_ASSERT_TRUE(demux_test::genRtcm3(work, rtcm1005, flen));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&demux::rtcm3::kDecoder, s_sink.frames[0].decoder);
    TEST_ASSERT_EQUAL_UINT(flen, s_sink.frames[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf.data(), s_sink.frames[0].data.data(), flen);

    // 最大载荷 1023（总长 1029 == maxFrame），载荷阶段批量累积只调极少次 step
    for (std::size_t i = 0U; i < sMaxPl.size(); ++i) {
        // 掩码后在 uint8_t 范围内
        sMaxPl[i] = static_cast<uint8_t>((i ^ 0x5AU) & 0xFFU);
    }
    TEST_ASSERT_TRUE(demux_test::genRtcm3(work, sMaxPl, flen));
    TEST_ASSERT_EQUAL_UINT(1029U, flen);
    {
        std::size_t calls = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kFrame, stepByNeed(demux::rtcm3::kDecoder,
                                                          work.subspan(0, flen), &endPos, &calls));
        TEST_ASSERT_EQUAL_UINT(1029U, endPos);
        TEST_ASSERT_TRUE(calls <= 4U);
    }

    // 早拒绝点：第 2 字节高 6 位非 0 必须在 len == 2 拒绝
    for (const uint8_t bad : kBadB1) {
        const std::array<uint8_t, 3> prefix{0xD3U, bad, 0x00U};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(
            demux::Step::kReject,
            demux::rtcm3::kDecoder.step(std::span<const uint8_t>(prefix).subspan(0, 2U), need));
    }

    // CRC 错：翻转 CRC 与载荷各一处均应 kReject；且能从坏帧载荷内重同步出真帧
    TEST_ASSERT_TRUE(demux_test::genRtcm3(work, rtcm1005, flen));
    s_work_buf[flen - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(demux::Step::kReject,
                      stepByNeed(demux::rtcm3::kDecoder, work.subspan(0, flen), &endPos, nullptr));
    s_work_buf[flen - 1U] ^= 0x01U;
    s_work_buf[5] ^= 0x80U;
    TEST_ASSERT_EQUAL(demux::Step::kReject,
                      stepByNeed(demux::rtcm3::kDecoder, work.subspan(0, flen), &endPos, nullptr));

    // 坏 RTCM3 头（D3 00 04）后紧跟真 RTCM3（6 字节）：前帧 CRC 错回溯后应恢复真帧
    s_work_buf[0] = 0xD3U;
    s_work_buf[1] = 0x00U;
    s_work_buf[2] = 0x04U;
    s_work_buf[3] = 0x11U;
    TEST_ASSERT_TRUE(demux_test::genRtcm3(work.subspan(4U), {}, flen));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, 4U + flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&demux::rtcm3::kDecoder, s_sink.frames[0].decoder);
    TEST_ASSERT_EQUAL_UINT(6U, s_sink.frames[0].len);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.unknownLen);
}
#endif

// --------------------------------------------------------------------------
// 3. NMEA
// --------------------------------------------------------------------------

#if STREAM_DEMUX_NMEA
static void testNmeaValidEarlyRejectCharsCaseAndLen()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&demux::nmea::kDecoder};
    static constexpr std::array<uint8_t, 14> kBadB1{
        '$', '@', 'a', 'z', '/', ':', '[', '*', ' ', '\r', '\n', 0x1FU, 0x7FU, 0x80U,
    };
    static constexpr std::array<uint8_t, 9> kBadBody{
        '$', 0x00U, 0x09U, 0x0AU, 0x0DU, 0x1FU, 0x7FU, 0x80U, 0xFFU,
    };
    static constexpr std::array<std::string_view, 6> kValidBodies{
        "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,",
        "PUBX,00,123456,0.0",
        "A",
        "Z,~ ",
        "0TEST",
        "9TEST",
    };
    std::array<char, demux_test::kNmeaMaxFrame - 5U> maxBody{};
    std::size_t flen = 0U;
    std::size_t endPos = 0U;
    const std::span<uint8_t> work(s_work_buf);

    // 正例 + 大小写十六进制
    for (const std::string_view body : kValidBodies) {
        for (const bool lc : {false, true}) {
            TEST_ASSERT_TRUE(demux_test::genNmea(work, body, lc, flen));
            runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
            TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
            TEST_ASSERT_EQUAL_PTR(&demux::nmea::kDecoder, s_sink.frames[0].decoder);
            TEST_ASSERT_EQUAL_UINT(flen, s_sink.frames[0].len);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf.data(), s_sink.frames[0].data.data(), flen);
        }
    }

    // 最大长度 256 字节
    maxBody.fill('X');
    maxBody[0] = 'G';
    TEST_ASSERT_TRUE(
        demux_test::genNmea(work, std::string_view(maxBody.data(), 250U), false, flen));
    TEST_ASSERT_EQUAL_UINT(256U, flen);
    TEST_ASSERT_EQUAL(demux::Step::kFrame,
                      stepByNeed(demux::nmea::kDecoder, work.subspan(0, flen), &endPos, nullptr));
    TEST_ASSERT_EQUAL_UINT(256U, endPos);

    // 长度超限（257 字节：251 字节正文 + *00\r\n），后面紧跟合法 NMEA
    s_work_buf[0] = static_cast<uint8_t>('$');
    std::fill_n(work.subspan(1U).begin(), 251U, static_cast<uint8_t>('X'));
    s_work_buf[1] = static_cast<uint8_t>('G');
    copySvToSpan("*00\r\n", work.subspan(252U, 5U));
    TEST_ASSERT_EQUAL(demux::Step::kReject,
                      stepByNeed(demux::nmea::kDecoder, work.subspan(0, 257U), &endPos, nullptr));
    TEST_ASSERT_TRUE(endPos <= 256U);
    TEST_ASSERT_TRUE(demux_test::genNmea(work.subspan(257U), "GNGGA,OK", false, flen));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, 257U + flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_UINT(257U, s_sink.unknownLen);

    // 早拒绝：第 2 字节非 [A-Z0-9]
    for (const uint8_t b1 : kBadB1) {
        const std::array<uint8_t, 2> p{static_cast<uint8_t>('$'), b1};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::nmea::kDecoder.step(p, need));
    }

    // 正文非法字符立即拒绝
    for (const uint8_t badCh : kBadBody) {
        const std::array<uint8_t, 4> p{static_cast<uint8_t>('$'), static_cast<uint8_t>('G'),
                                       static_cast<uint8_t>('P'), badCh};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::nmea::kDecoder.step(p, need));
    }

    // 校验格式与行尾错误
    {
        static constexpr std::array<std::string_view, 8> kBadTails{
            "$GP*G0\r\n", "$GP*0g\r\n", "$GP*37\n", "$GP*37\rX",
            "$GP*37\r\r", "$GP*37  ",   "$GP*\r\n", "$GP*38\r\n", // 正确 XOR 为 0x37
        };
        for (const std::string_view tail : kBadTails) {
            copySvToSpan(tail, work);
            TEST_ASSERT_EQUAL(
                demux::Step::kReject,
                stepByNeed(demux::nmea::kDecoder, work.subspan(0, tail.size()), &endPos, nullptr));
        }
    }

    // 正文中含 '$' 立即拒并在分流器中恢复后句
    {
        static constexpr std::string_view kBroken = "$GPGGA,BAD";
        copySvToSpan(kBroken, work);
        TEST_ASSERT_TRUE(demux_test::genNmea(work.subspan(kBroken.size()), "GNGGA,1", true, flen));
        runDemuxFeedAndAbort(kDecs, work.subspan(0, kBroken.size() + flen));
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
        TEST_ASSERT_EQUAL_UINT(kBroken.size(), s_sink.unknownLen);
    }
}
#endif

// --------------------------------------------------------------------------
// 4. UBX
// --------------------------------------------------------------------------

#if STREAM_DEMUX_UBX
static void testUbxValidEarlyRejectLenAndCkErr()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&demux::ubx::kDecoder};
    static constexpr std::array<uint8_t, 5> kBadB1{0x00U, 0x61U, 0x63U, 0xB5U, 0xFFU};
    static std::array<uint8_t, demux_test::kUbxMaxPayload> sMaxPl{};
    std::array<uint8_t, 12> pl{};
    std::size_t flen = 0U;
    std::size_t endPos = 0U;
    const std::span<uint8_t> work(s_work_buf);

    // 0 载荷（8 字节）
    TEST_ASSERT_TRUE(demux_test::genUbx(work, 0x0AU, 0x04U, {}, flen));
    TEST_ASSERT_EQUAL_UINT(8U, flen);
    TEST_ASSERT_EQUAL(demux::Step::kFrame,
                      stepByNeed(demux::ubx::kDecoder, work.subspan(0, flen), &endPos, nullptr));

    // 典型载荷 + 经引擎投递
    pl.fill(0x55U);
    TEST_ASSERT_TRUE(demux_test::genUbx(work, 0x01U, 0x07U, pl, flen));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&demux::ubx::kDecoder, s_sink.frames[0].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf.data(), s_sink.frames[0].data.data(), flen);

    // 最大载荷 4088（总长 4096 == maxFrame），验证批量累积
    for (std::size_t i = 0U; i < sMaxPl.size(); ++i) {
        // 掩码后范围为 0..255
        sMaxPl[i] = static_cast<uint8_t>(i & 0xFFU);
    }
    TEST_ASSERT_TRUE(demux_test::genUbx(work, 0x02U, 0x15U, sMaxPl, flen));
    TEST_ASSERT_EQUAL_UINT(4096U, flen);
    {
        std::size_t calls = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kFrame,
                          stepByNeed(demux::ubx::kDecoder, work.subspan(0, flen), &endPos, &calls));
        TEST_ASSERT_TRUE(calls <= 4U);
    }

    // 早拒绝：第 2 字节 != 0x62
    for (const uint8_t b1 : kBadB1) {
        const std::array<uint8_t, 2> p{0xB5U, b1};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::ubx::kDecoder.step(p, need));
    }

    // 早拒绝：payloadLen = 4089（超限）与 0xFFFF
    {
        static constexpr std::array<uint8_t, 6> kHdr4089{0xB5U, 0x62U, 0x01U, 0x07U, 0xF9U, 0x0FU};
        static constexpr std::array<uint8_t, 6> kHdrFfff{0xB5U, 0x62U, 0x01U, 0x07U, 0xFFU, 0xFFU};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::ubx::kDecoder.step(kHdr4089, need));
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::ubx::kDecoder.step(kHdrFfff, need));
    }

    // 校验错：CK_A 错、CK_B 错、载荷篡改
    TEST_ASSERT_TRUE(demux_test::genUbx(work, 0x01U, 0x07U, pl, flen));
    s_work_buf[flen - 2U] ^= 0x01U;
    TEST_ASSERT_EQUAL(demux::Step::kReject,
                      stepByNeed(demux::ubx::kDecoder, work.subspan(0, flen), &endPos, nullptr));
    s_work_buf[flen - 2U] ^= 0x01U;
    s_work_buf[flen - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(demux::Step::kReject,
                      stepByNeed(demux::ubx::kDecoder, work.subspan(0, flen), &endPos, nullptr));
    s_work_buf[flen - 1U] ^= 0x01U;
    s_work_buf[6] ^= 0x01U;
    TEST_ASSERT_EQUAL(demux::Step::kReject,
                      stepByNeed(demux::ubx::kDecoder, work.subspan(0, flen), &endPos, nullptr));

    // 坏 UBX 头后嵌真 UBX 帧经分流器恢复
    s_work_buf[0] = 0xB5U;
    s_work_buf[1] = 0x62U;
    s_work_buf[2] = 0x01U;
    s_work_buf[3] = 0x01U;
    s_work_buf[4] = 0x04U;
    s_work_buf[5] = 0x00U;
    TEST_ASSERT_TRUE(demux_test::genUbx(work.subspan(6U), 0x0AU, 0x04U, {}, flen));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, 6U + flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_UINT(8U, s_sink.frames[0].len);
    TEST_ASSERT_EQUAL_UINT(6U, s_sink.unknownLen);
}
#endif

// --------------------------------------------------------------------------
// 5. Unicore 二进制
// --------------------------------------------------------------------------

#if STREAM_DEMUX_UNICORE_BIN
static void testUnicoreBinValidEarlyRejectLenAndCrcErr()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&demux::unicore_bin::kDecoder};
    static std::array<uint8_t, demux_test::kUnicoreBinMaxPayload> sMaxPl{};
    std::array<uint8_t, 16> rest{};
    std::array<uint8_t, 32> pl{};
    std::size_t flen = 0U;
    std::size_t endPos = 0U;
    const std::span<uint8_t> work(s_work_buf);

    // 0 载荷（28 字节）
    TEST_ASSERT_TRUE(demux_test::genUnicoreBin(work, 42U, 15U, {}, {}, flen));
    TEST_ASSERT_EQUAL_UINT(28U, flen);
    TEST_ASSERT_EQUAL(demux::Step::kFrame, stepByNeed(demux::unicore_bin::kDecoder,
                                                      work.subspan(0, flen), &endPos, nullptr));

    // 非零时间头 + 载荷 + 经引擎投递
    for (std::size_t i = 0U; i < rest.size(); ++i) {
        // 范围 0x80..0x8F
        rest[i] = static_cast<uint8_t>(0x80U + i);
    }
    for (std::size_t i = 0U; i < pl.size(); ++i) {
        // 范围 0..93
        pl[i] = static_cast<uint8_t>(i * 3U);
    }
    TEST_ASSERT_TRUE(demux_test::genUnicoreBin(work, 101U, 30U, rest, pl, flen));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&demux::unicore_bin::kDecoder, s_sink.frames[0].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf.data(), s_sink.frames[0].data.data(), flen);

    // 最大载荷 4068（总长 4096 == maxFrame），验证批量累积
    for (std::size_t i = 0U; i < sMaxPl.size(); ++i) {
        // 掩码后范围为 0..255
        sMaxPl[i] = static_cast<uint8_t>(i & 0xFFU);
    }
    TEST_ASSERT_TRUE(demux_test::genUnicoreBin(work, 999U, 0U, {}, sMaxPl, flen));
    TEST_ASSERT_EQUAL_UINT(4096U, flen);
    {
        std::size_t calls = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kFrame, stepByNeed(demux::unicore_bin::kDecoder,
                                                          work.subspan(0, flen), &endPos, &calls));
        TEST_ASSERT_TRUE(calls <= 8U);
    }

    // 早拒绝：第 2 字节 != 0x44；第 3 字节 != 0xB5（含产品帧 0x18）
    {
        static constexpr std::array<uint8_t, 2> kBad2{0xAAU, 0x43U};
        static constexpr std::array<uint8_t, 3> kBad3Prod{0xAAU, 0x44U, 0x18U};
        static constexpr std::array<uint8_t, 3> kBad3Other{0xAAU, 0x44U, 0xB4U};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::unicore_bin::kDecoder.step(kBad2, need));
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::unicore_bin::kDecoder.step(kBad3Prod, need));
        TEST_ASSERT_EQUAL(demux::Step::kReject,
                          demux::unicore_bin::kDecoder.step(kBad3Other, need));
    }

    // 早拒绝：payloadLen = 4069（超限）与 0xFFFF，必须在 <= 24 字节头部完成前拒绝
    {
        std::array<uint8_t, 24> hdr{};
        hdr[0] = 0xAAU;
        hdr[1] = 0x44U;
        hdr[2] = 0xB5U;
        hdr[6] = 0xE5U; // 4069 = 0x0FE5
        hdr[7] = 0x0FU;
        TEST_ASSERT_EQUAL(demux::Step::kReject,
                          stepByNeed(demux::unicore_bin::kDecoder, hdr, &endPos, nullptr));
        TEST_ASSERT_TRUE(endPos <= 24U);

        hdr[6] = 0xFFU;
        hdr[7] = 0xFFU;
        TEST_ASSERT_EQUAL(demux::Step::kReject,
                          stepByNeed(demux::unicore_bin::kDecoder, hdr, &endPos, nullptr));
        TEST_ASSERT_TRUE(endPos <= 24U);
    }

    // CRC 错误 & 若错用标准 IEEE 取反 CRC-32 必须拒绝
    TEST_ASSERT_TRUE(demux_test::genUnicoreBin(work, 101U, 30U, rest, pl, flen));
    s_work_buf[flen - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(demux::Step::kReject, stepByNeed(demux::unicore_bin::kDecoder,
                                                       work.subspan(0, flen), &endPos, nullptr));
    {
        const uint32_t ieeeCrc = demux_test::crc32Ieee(work.subspan(0, flen - 4U));
        s_work_buf[flen - 4U] = static_cast<uint8_t>(ieeeCrc & 0xFFU);         // 掩码后 0..255
        s_work_buf[flen - 3U] = static_cast<uint8_t>((ieeeCrc >> 8U) & 0xFFU); // 掩码后 0..255
        s_work_buf[flen - 2U] = static_cast<uint8_t>((ieeeCrc >> 16U) & 0xFFU);
        s_work_buf[flen - 1U] = static_cast<uint8_t>((ieeeCrc >> 24U) & 0xFFU);
        TEST_ASSERT_EQUAL(
            demux::Step::kReject,
            stepByNeed(demux::unicore_bin::kDecoder, work.subspan(0, flen), &endPos, nullptr));
    }
}
#endif

// --------------------------------------------------------------------------
// 6. Unicore Hash
// --------------------------------------------------------------------------

#if STREAM_DEMUX_UNICORE_HASH
static void testUnicoreHashValidEarlyRejectCharsCaseAndLen()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&demux::unicore_hash::kDecoder};
    static constexpr std::array<uint8_t, 12> kBadB1{
        '0', '9', 'a', 'z', '#', '$', '*', ' ', '\r', 0x1FU, 0x7FU, 0x80U,
    };
    static constexpr std::array<uint8_t, 9> kBadBody{
        '#', '$', 0x00U, 0x09U, 0x0AU, 0x0DU, 0x1FU, 0x7FU, 0x80U,
    };
    static constexpr std::string_view kVerReal =
        "#VERSION,97,GPS,FINE,2282,248561000,0,0,18,676;UM980,R4.10Build7923,"
        "HRPT00-S10C-P,2310415000001-MD22B1224962616,ff3bac96f31f9bdd,2022/09/28*7432d4ed\r\n";
    std::array<char, demux_test::kUnicoreHashMaxFrame> maxBody{};
    std::size_t flen = 0U;
    std::size_t endPos = 0U;
    const std::span<uint8_t> work(s_work_buf);

    // 真实 UM980 #VERSION 向量
    copySvToSpan(kVerReal, work);
    runDemuxFeedAndAbort(kDecs, work.subspan(0, kVerReal.size()));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&demux::unicore_hash::kDecoder, s_sink.frames[0].decoder);
    TEST_ASSERT_EQUAL_UINT(kVerReal.size(), s_sink.frames[0].len);

    // 两种校验（CRC-32 与 XOR）+ 大小写十六进制 + 边界字母 'A'/'Z' + 边界 ASCII
    for (const bool lc : {false, true}) {
        TEST_ASSERT_TRUE(demux_test::genUnicoreHashCrc32(work, "BESTPOSA,99, ~", lc, flen));
        runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
        TEST_ASSERT_EQUAL_PTR(&demux::unicore_hash::kDecoder, s_sink.frames[0].decoder);

        TEST_ASSERT_TRUE(demux_test::genUnicoreHashXor(work, "Z,1", lc, flen));
        runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
        TEST_ASSERT_EQUAL_PTR(&demux::unicore_hash::kDecoder, s_sink.frames[0].decoder);
    }

    // 最大长度 4096（XOR 与 CRC-32 两种模式，覆盖 #SATSINFOA 长帧）
    maxBody.fill('A');
    TEST_ASSERT_TRUE(
        demux_test::genUnicoreHashXor(work, std::string_view(maxBody.data(), 4090U), false, flen));
    TEST_ASSERT_EQUAL_UINT(4096U, flen);
    TEST_ASSERT_EQUAL(demux::Step::kFrame, stepByNeed(demux::unicore_hash::kDecoder,
                                                      work.subspan(0, flen), &endPos, nullptr));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_UINT(4096U, s_sink.frames[0].len);

    TEST_ASSERT_TRUE(demux_test::genUnicoreHashCrc32(work, std::string_view(maxBody.data(), 4084U),
                                                     false, flen));
    TEST_ASSERT_EQUAL_UINT(4096U, flen);
    TEST_ASSERT_EQUAL(demux::Step::kFrame, stepByNeed(demux::unicore_hash::kDecoder,
                                                      work.subspan(0, flen), &endPos, nullptr));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_UINT(4096U, s_sink.frames[0].len);

    // 长度超限 4097 字节
    s_work_buf[0] = static_cast<uint8_t>('#');
    std::fill_n(work.subspan(1U).begin(), 4091U, static_cast<uint8_t>('A'));
    copySvToSpan("*00\r\n", work.subspan(4092U, 5U));
    TEST_ASSERT_EQUAL(demux::Step::kReject, stepByNeed(demux::unicore_hash::kDecoder,
                                                       work.subspan(0, 4097U), &endPos, nullptr));

    // 早拒绝：第 2 字节非 'A'..'Z'（包含数字 '0'..'9'）
    for (const uint8_t b1 : kBadB1) {
        const std::array<uint8_t, 2> p{static_cast<uint8_t>('#'), b1};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::unicore_hash::kDecoder.step(p, need));
    }

    // 正文非法字符立即拒绝
    for (const uint8_t badCh : kBadBody) {
        const std::array<uint8_t, 4> p{static_cast<uint8_t>('#'), static_cast<uint8_t>('V'),
                                       static_cast<uint8_t>('E'), badCh};
        std::size_t need = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kReject, demux::unicore_hash::kDecoder.step(p, need));
    }

    // 校验位数不对 / 非十六进制 / 缺 \r\n / 错校验值
    {
        static constexpr std::array<std::string_view, 11> kBadHash{
            "#A*\r\n",          "#A*4\r\n",  "#A*41A\r\n",      "#A*1234567\r\n",
            "#A*123456789\r\n", "#A*1G\r\n", "#A*1234567G\r\n", "#A*41\n",
            "#A*41\rX",         "#A*42\r\n", "#A*00000000\r\n",
        };
        for (const std::string_view bad : kBadHash) {
            copySvToSpan(bad, work);
            TEST_ASSERT_EQUAL(demux::Step::kReject,
                              stepByNeed(demux::unicore_hash::kDecoder, work.subspan(0, bad.size()),
                                         &endPos, nullptr));
        }
    }

#if STREAM_DEMUX_NMEA
    // # 句中含 '$' 立即拒并在 {nmea, unicore_hash} 中恢复其后的 $GPGGA
    {
        static constexpr std::array<const demux::Decoder *, 2> kCombo{
            &demux::nmea::kDecoder,
            &demux::unicore_hash::kDecoder,
        };
        static constexpr std::string_view kBadPrefix = "#VERSION,CUT";
        copySvToSpan(kBadPrefix, work);
        TEST_ASSERT_TRUE(
            demux_test::genNmea(work.subspan(kBadPrefix.size()), "GPGGA,1", false, flen));
        runDemuxFeedAndAbort(kCombo, work.subspan(0, kBadPrefix.size() + flen));
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
        TEST_ASSERT_EQUAL_PTR(&demux::nmea::kDecoder, s_sink.frames[0].decoder);
        TEST_ASSERT_EQUAL_UINT(kBadPrefix.size(), s_sink.unknownLen);
    }
#endif
}
#endif

// --------------------------------------------------------------------------
// 7. 产品帧解码器与 proto::demux_frame::view
// --------------------------------------------------------------------------

static void testProductDemuxAndFrameView()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&proto::demux_frame::kDecoder};
    static constexpr std::array<uint8_t, 32> kFrameC{
        0xAA, 0x44, 0x18, 0x14, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x08, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x00, 0x05, 0x00,
        0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x34, 0x05, 0xAA,
    };
    static std::array<uint8_t, proto::kMaxPayload> sMaxPl{};
    std::size_t flen = 0U;
    std::size_t endPos = 0U;
    const std::span<uint8_t> work(s_work_buf);

    // 0 载荷、已知向量 kFrameC、最大载荷 256
    TEST_ASSERT_TRUE(demux_test::genProduct(work, 0x0001U, proto::sender::kApp,
                                            proto::msg_type::kQuery, {}, flen));
    runDemuxFeedAndAbort(kDecs, work.subspan(0, flen));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&proto::demux_frame::kDecoder, s_sink.frames[0].decoder);
    {
        const std::optional<proto::FrameView> fv = proto::demux_frame::view(
            std::span<const uint8_t>(s_sink.frames[0].data.data(), s_sink.frames[0].len));
        TEST_ASSERT_TRUE(fv.has_value());
        TEST_ASSERT_EQUAL_HEX16(0x0001U, fv->msgId);
        TEST_ASSERT_TRUE(fv->payload.empty());
    }

    runDemuxFeedAndAbort(kDecs, kFrameC);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&proto::demux_frame::kDecoder, s_sink.frames[0].decoder);
    {
        const std::span<const uint8_t> wire(s_sink.frames[0].data.data(), s_sink.frames[0].len);
        const std::optional<proto::FrameView> fv = proto::demux_frame::view(wire);
        TEST_ASSERT_TRUE(fv.has_value());
        TEST_ASSERT_EQUAL_HEX16(0x0041U, fv->msgId);
        TEST_ASSERT_EQUAL_UINT8(proto::sender::kAlarm, fv->sender);
        TEST_ASSERT_EQUAL_UINT8(proto::msg_type::kPush, fv->type);
        TEST_ASSERT_EQUAL_UINT(8U, fv->payload.size());
        TEST_ASSERT_EQUAL_PTR(wire.subspan(proto::kHeaderLen).data(), fv->payload.data());
    }

    for (std::size_t i = 0U; i < sMaxPl.size(); ++i) {
        // i < 256，在 uint8_t 范围内
        sMaxPl[i] = static_cast<uint8_t>(i);
    }
    TEST_ASSERT_TRUE(demux_test::genProduct(work, 0x0077U, proto::sender::kDevice,
                                            proto::msg_type::kSetRes, sMaxPl, flen));
    TEST_ASSERT_EQUAL_UINT(proto::kMaxFrame, flen);
    {
        std::size_t calls = 0U;
        TEST_ASSERT_EQUAL(demux::Step::kFrame, stepByNeed(proto::demux_frame::kDecoder,
                                                          work.subspan(0, flen), &endPos, &calls));
        TEST_ASSERT_TRUE(calls <= 8U);
    }

    // 早拒绝：第 2、3、4 字节 + payloadLen > 256
    {
        static constexpr std::array<uint8_t, 2> kB2{0xAAU, 0x45U};
        static constexpr std::array<uint8_t, 3> kB3{0xAAU, 0x44U, 0xB5U};
        static constexpr std::array<uint8_t, 4> kB4{0xAAU, 0x44U, 0x18U, 0x15U};
        std::array<uint8_t, proto::kHeaderLen> hdr257{};
        std::size_t need = 0U;

        TEST_ASSERT_EQUAL(demux::Step::kReject, proto::demux_frame::kDecoder.step(kB2, need));
        TEST_ASSERT_EQUAL(demux::Step::kReject, proto::demux_frame::kDecoder.step(kB3, need));
        TEST_ASSERT_EQUAL(demux::Step::kReject, proto::demux_frame::kDecoder.step(kB4, need));

        std::copy(kFrameC.begin(), kFrameC.begin() + static_cast<std::ptrdiff_t>(proto::kHeaderLen),
                  hdr257.begin());
        hdr257[12] = 0x01U;
        hdr257[13] = 0x01U; // payloadLen = 257
        TEST_ASSERT_EQUAL(demux::Step::kReject,
                          stepByNeed(proto::demux_frame::kDecoder, hdr257, &endPos, nullptr));
        TEST_ASSERT_TRUE(endPos <= proto::kHeaderLen);
    }

    // CRC 错 + NovAtel 非取反 CRC 必须被产品解码器拒绝
    std::copy(kFrameC.begin(), kFrameC.end(), s_work_buf.begin());
    s_work_buf[kFrameC.size() - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(demux::Step::kReject,
                      stepByNeed(proto::demux_frame::kDecoder, work.subspan(0, kFrameC.size()),
                                 &endPos, nullptr));
    {
        const uint32_t novCrc = demux_test::crc32Novatel(work.subspan(0, kFrameC.size() - 4U));
        s_work_buf[kFrameC.size() - 4U] = static_cast<uint8_t>(novCrc & 0xFFU);
        s_work_buf[kFrameC.size() - 3U] = static_cast<uint8_t>((novCrc >> 8U) & 0xFFU);
        s_work_buf[kFrameC.size() - 2U] = static_cast<uint8_t>((novCrc >> 16U) & 0xFFU);
        s_work_buf[kFrameC.size() - 1U] = static_cast<uint8_t>((novCrc >> 24U) & 0xFFU);
        TEST_ASSERT_EQUAL(demux::Step::kReject,
                          stepByNeed(proto::demux_frame::kDecoder, work.subspan(0, kFrameC.size()),
                                     &endPos, nullptr));
    }

    // proto::demux_frame::view 非法输入返回 nullopt；不重复校验 CRC；保留 protocol/interval
    TEST_ASSERT_FALSE(proto::demux_frame::view({}).has_value());
    TEST_ASSERT_FALSE(
        proto::demux_frame::view(std::span<const uint8_t>(kFrameC).subspan(0, 23U)).has_value());
    TEST_ASSERT_FALSE(proto::demux_frame::view(work.subspan(0, proto::kMaxFrame + 1U)).has_value());
    TEST_ASSERT_FALSE(
        proto::demux_frame::view(std::span<const uint8_t>(kFrameC).subspan(0, kFrameC.size() - 1U))
            .has_value());

    std::copy(kFrameC.begin(), kFrameC.end(), s_work_buf.begin());
    s_work_buf[2] = 0xB5U;
    TEST_ASSERT_FALSE(proto::demux_frame::view(work.subspan(0, kFrameC.size())).has_value());

    std::copy(kFrameC.begin(), kFrameC.end(), s_work_buf.begin());
    s_work_buf[3] = 0x15U;
    TEST_ASSERT_FALSE(proto::demux_frame::view(work.subspan(0, kFrameC.size())).has_value());

    std::copy(kFrameC.begin(), kFrameC.end(), s_work_buf.begin());
    s_work_buf[12] = 0x01U;
    s_work_buf[13] = 0x01U; // payload_len = 257
    TEST_ASSERT_FALSE(proto::demux_frame::view(work.subspan(0, kFrameC.size())).has_value());

    std::copy(kFrameC.begin(), kFrameC.end(), s_work_buf.begin());
    s_work_buf[18] = 0x07U;                   // protocol
    s_work_buf[19] = 0x0AU;                   // interval
    s_work_buf[kFrameC.size() - 1U] ^= 0xFFU; // 故意坏 CRC：view() 不重复校验
    {
        const std::optional<proto::FrameView> fv =
            proto::demux_frame::view(work.subspan(0, kFrameC.size()));
        TEST_ASSERT_TRUE(fv.has_value());
        TEST_ASSERT_EQUAL_HEX8(0x07U, fv->protocol);
        TEST_ASSERT_EQUAL_HEX8(0x0AU, fv->interval);
    }
}

// --------------------------------------------------------------------------
// 8. '$' 同首字节冲突（NMEA vs 自定义 SBF $@）
// --------------------------------------------------------------------------

#if STREAM_DEMUX_NMEA
static void testDollarConflictWithCustomDecoder()
{
    static constexpr std::array<const demux::Decoder *, 2> kOrderA{&demux::nmea::kDecoder,
                                                                   &kDecCustomSbf};
    static constexpr std::array<const demux::Decoder *, 2> kOrderB{&kDecCustomSbf,
                                                                   &demux::nmea::kDecoder};
    static constexpr std::array<uint8_t, 6> kSbf{'$', '@', 0x11U, 0x22U, 0x33U, 0x44U};
    std::size_t nmeaLen = 0U;
    std::size_t pos = 0U;
    const std::span<uint8_t> work(s_work_buf);

    TEST_ASSERT_TRUE(demux_test::genNmea(work.subspan(pos), "GNGGA,123", false, nmeaLen));
    pos += nmeaLen;
    std::copy(kSbf.begin(), kSbf.end(), work.subspan(pos).begin());
    pos += kSbf.size();
    TEST_ASSERT_TRUE(demux_test::genNmea(work.subspan(pos), "GPRMC,456", true, nmeaLen));
    pos += nmeaLen;

    runDemuxFeedAndAbort(kOrderA, work.subspan(0, pos));
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&demux::nmea::kDecoder, s_sink.frames[0].decoder);
    TEST_ASSERT_EQUAL_PTR(&kDecCustomSbf, s_sink.frames[1].decoder);
    TEST_ASSERT_EQUAL_PTR(&demux::nmea::kDecoder, s_sink.frames[2].decoder);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknownBytes);

    runDemuxFeedAndAbort(kOrderB, work.subspan(0, pos));
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&demux::nmea::kDecoder, s_sink.frames[0].decoder);
    TEST_ASSERT_EQUAL_PTR(&kDecCustomSbf, s_sink.frames[1].decoder);
    TEST_ASSERT_EQUAL_PTR(&demux::nmea::kDecoder, s_sink.frames[2].decoder);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknownBytes);
}
#endif

// --------------------------------------------------------------------------
// explain：拒绝原因文本（ADR-0009 §2.11）。每个解码器至少两个原因。
// --------------------------------------------------------------------------

namespace {

bool textContains(std::string_view text, std::string_view needle) noexcept
{
    return text.find(needle) != std::string_view::npos;
}

std::string_view explainText(const demux::Decoder &decoder, std::span<const uint8_t> prefix,
                             std::array<char, 96> &buf) noexcept
{
    if (decoder.explain == nullptr) {
        return {};
    }
    const std::size_t len = decoder.explain(prefix, buf);
    return std::string_view(buf.data(), len);
}

void assertExplain(const demux::Decoder &decoder, std::span<const uint8_t> prefix,
                   std::string_view needle) noexcept
{
    std::array<char, 96> buf{};
    const std::string_view text = explainText(decoder, prefix, buf);
    if (!textContains(text, needle)) {
        TEST_FAIL_MESSAGE("explain text mismatch");
    }
}

} // namespace

static void testExplainNmea()
{
#if STREAM_DEMUX_NMEA
    static constexpr std::array<uint8_t, 13> kBadChecksum{'$', 'G', 'P', 'G', 'G',  'A', ',',
                                                          '1', '*', '0', '0', '\r', '\n'};
    assertExplain(demux::nmea::kDecoder, kBadChecksum,
                  "bad checksum, received 0x00, computed 0x4B");
    static constexpr std::array<uint8_t, 10> kIllegal{'$', 'G', 'P', 0x01U, 'X',
                                                      '*', '0', '7', '\r',  '\n'};
    assertExplain(demux::nmea::kDecoder, kIllegal, "illegal byte 0x01 at offset 3");
#endif
}

static void testExplainRtcm3()
{
#if STREAM_DEMUX_RTCM3
    static constexpr std::array<uint8_t, 7> kReserved{0xD3U, 0x40U, 0x01U, 0x3CU,
                                                      0x00U, 0x00U, 0x00U};
    assertExplain(demux::rtcm3::kDecoder, kReserved, "reserved bits set in byte 1: 0x40");
    static constexpr std::array<uint8_t, 7> kBadCrc{0xD3U, 0x00U, 0x01U, 0x3CU,
                                                    0x00U, 0x00U, 0x00U};
    assertExplain(demux::rtcm3::kDecoder, kBadCrc, "bad CRC-24Q, received 0x000000");
#endif
}

static void testExplainUbx()
{
#if STREAM_DEMUX_UBX
    static constexpr std::array<uint8_t, 2> kBadSync{0xB5U, 0x00U};
    assertExplain(demux::ubx::kDecoder, kBadSync, "sync byte 2 is 0x00, expected 0x62");
    static constexpr std::array<uint8_t, 8> kBadCk{0xB5U, 0x62U, 0x01U, 0x07U,
                                                   0x00U, 0x00U, 0x00U, 0x00U};
    assertExplain(demux::ubx::kDecoder, kBadCk, "bad checksum, received 0x0000");
#endif
}

static void testExplainUnicoreBin()
{
#if STREAM_DEMUX_UNICORE_BIN
    static constexpr std::array<uint8_t, 3> kBadSync{0xAAU, 0x44U, 0x00U};
    assertExplain(demux::unicore_bin::kDecoder, kBadSync, "sync byte 3 is 0x00, expected 0xB5");
    std::array<uint8_t, 28> frame{};
    frame[0] = 0xAAU;
    frame[1] = 0x44U;
    frame[2] = 0xB5U;
    // 24 B 头 + 0 B 载荷 + 4 B CRC，CRC 全 0 必然不符。
    assertExplain(demux::unicore_bin::kDecoder, frame, "bad CRC32, received 0x00000000");
#endif
}

static void testExplainUnicoreHash()
{
#if STREAM_DEMUX_UNICORE_HASH
    static constexpr std::array<uint8_t, 13> kNoStar{'#', 'V', 'E', 'R', 'S',  'I', 'O',
                                                     'N', 'A', ',', 'x', '\r', '\n'};
    assertExplain(demux::unicore_hash::kDecoder, kNoStar, "no '*' before LF");
    static constexpr std::array<uint8_t, 8> kBadXor{'#', 'A', 'B', '*', '0', '0', '\r', '\n'};
    assertExplain(demux::unicore_hash::kDecoder, kBadXor,
                  "bad XOR checksum, received 0x00, computed 0x03");
#endif
}

static void testExplainProductFrame()
{
    static constexpr std::array<uint8_t, 2> kBadSync{0xAAU, 0x00U};
    assertExplain(proto::demux_frame::kDecoder, kBadSync, "sync byte 2 is 0x00, expected 0x44");
    static constexpr std::array<uint8_t, 4> kBadHdr{0xAAU, 0x44U, 0x18U, 0x10U};
    assertExplain(proto::demux_frame::kDecoder, kBadHdr, "header length is 0x10, expected 0x14");
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testDecoderDescriptorsAndFirstBytes);
#if STREAM_DEMUX_RTCM3
    RUN_TEST(testRtcm3ValidEarlyRejectAndCrcErr);
#endif
#if STREAM_DEMUX_NMEA
    RUN_TEST(testNmeaValidEarlyRejectCharsCaseAndLen);
#endif
#if STREAM_DEMUX_UBX
    RUN_TEST(testUbxValidEarlyRejectLenAndCkErr);
#endif
#if STREAM_DEMUX_UNICORE_BIN
    RUN_TEST(testUnicoreBinValidEarlyRejectLenAndCrcErr);
#endif
#if STREAM_DEMUX_UNICORE_HASH
    RUN_TEST(testUnicoreHashValidEarlyRejectCharsCaseAndLen);
#endif
    RUN_TEST(testProductDemuxAndFrameView);
#if STREAM_DEMUX_NMEA
    RUN_TEST(testDollarConflictWithCustomDecoder);
    RUN_TEST(testExplainNmea);
    RUN_TEST(testExplainRtcm3);
    RUN_TEST(testExplainUbx);
    RUN_TEST(testExplainUnicoreBin);
    RUN_TEST(testExplainUnicoreHash);
    RUN_TEST(testExplainProductFrame);
#endif
    return UNITY_END();
}
