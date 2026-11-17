// 通用字节流分流器属性与场景流测试（ADR-0005 / ADR-0004 §3、§4）。
// 覆盖：I1 分块无关（>=1000 轮随机切分）、I2 字节守恒、假前导混合流恢复、蓝牙/GNSS 场景与 AA 冲突。
// 随 STREAM_DEMUX_<协议> 编译定义自动裁剪对应解码器及场景。

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
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "demux/Demux.hpp"
#include "demux/StaticDemux.hpp"
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
constexpr std::size_t kStreamCap = 16384U;
constexpr std::size_t kMaxFramesRec = 128U;
constexpr std::size_t kBlobCap = 16384U;
constexpr uint32_t kSplitRounds = 1000U;
constexpr uint32_t kMixedSeedRounds = 40U;
constexpr uint32_t kBasePrngSeed = 0x5EED1234U;
constexpr std::size_t kFailMsgCap = 160U;

enum class ProtoKind : uint8_t {
    kRtcm3 = 0,
    kNmea = 1,
    kUbx = 2,
    kUnicoreBin = 3,
    kUnicoreHash = 4,
    kProduct = 5,
};

struct ActiveProtoEntry {
    ProtoKind kind = ProtoKind::kProduct;
    const demux::Decoder *decoder = nullptr;
};

struct FrameMeta {
    const demux::Decoder *decoder = nullptr;
    uint8_t decoderIndex = 0U;
    std::size_t offset = 0U;
    std::size_t len = 0U;
};

struct StreamCapture {
    void clear() noexcept
    {
        frameCount = 0U;
        frameBlobUsed = 0U;
        unknownLen = 0U;
        overflowed = false;
    }

    std::array<FrameMeta, kMaxFramesRec> meta{};
    std::size_t frameCount = 0U;
    std::array<uint8_t, kBlobCap> frameBlob{};
    std::size_t frameBlobUsed = 0U;
    std::array<uint8_t, kStreamCap> unknownBlob{};
    std::size_t unknownLen = 0U;
    bool overflowed = false;
};

class CaptureSink final : public demux::Sink {
public:
    explicit CaptureSink(StreamCapture &cap) noexcept : cap_(&cap) {}

    void onFrame(const demux::Decoder &decoder, uint8_t index,
                 std::span<const uint8_t> frame) noexcept override
    {
        if ((cap_->frameCount >= kMaxFramesRec) ||
            ((cap_->frameBlobUsed + frame.size()) > kBlobCap)) {
            cap_->overflowed = true;
            return;
        }
        FrameMeta &m = cap_->meta[cap_->frameCount];
        m.decoder = &decoder;
        m.decoderIndex = index;
        m.offset = cap_->frameBlobUsed;
        m.len = frame.size();
        if (!frame.empty()) {
            std::copy(frame.begin(), frame.end(),
                      cap_->frameBlob.begin() + static_cast<std::ptrdiff_t>(cap_->frameBlobUsed));
        }
        cap_->frameBlobUsed += frame.size();
        cap_->frameCount++;
    }

    void onUnknown(std::span<const uint8_t> bytes) noexcept override
    {
        if ((cap_->unknownLen + bytes.size()) > kStreamCap) {
            cap_->overflowed = true;
            return;
        }
        if (!bytes.empty()) {
            std::copy(bytes.begin(), bytes.end(),
                      cap_->unknownBlob.begin() + static_cast<std::ptrdiff_t>(cap_->unknownLen));
        }
        cap_->unknownLen += bytes.size();
    }

private:
    StreamCapture *cap_;
};

std::array<uint8_t, kDemuxBufSize> s_demux_buf{};
demux::Demux s_dmx;
StreamCapture s_ref_cap;
StreamCapture s_cur_cap;
StreamCapture s_truth_cap;
std::array<uint8_t, kStreamCap> s_stream_buf{};

// 当前构建开启的协议列表（产品帧优先于 Unicore 二进制）
constexpr std::array kActiveProtos{
    ActiveProtoEntry{ProtoKind::kProduct, &proto::demux_frame::kDecoder},
#if STREAM_DEMUX_UNICORE_BIN
    ActiveProtoEntry{ProtoKind::kUnicoreBin, &demux::unicore_bin::kDecoder},
#endif
#if STREAM_DEMUX_RTCM3
    ActiveProtoEntry{ProtoKind::kRtcm3, &demux::rtcm3::kDecoder},
#endif
#if STREAM_DEMUX_NMEA
    ActiveProtoEntry{ProtoKind::kNmea, &demux::nmea::kDecoder},
#endif
#if STREAM_DEMUX_UNICORE_HASH
    ActiveProtoEntry{ProtoKind::kUnicoreHash, &demux::unicore_hash::kDecoder},
#endif
#if STREAM_DEMUX_UBX
    ActiveProtoEntry{ProtoKind::kUbx, &demux::ubx::kDecoder},
#endif
};

constexpr std::size_t kActiveProtoCount = kActiveProtos.size();

uint32_t xorshift32(uint32_t &state) noexcept
{
    uint32_t x = state;
    if (x == 0U) {
        x = 0x6D2B79F5U;
    }
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    state = x;
    return x;
}

void runChunked(std::span<const demux::Decoder *const> decs, std::span<const uint8_t> data,
                StreamCapture &cap, std::size_t fixedChunk, uint32_t *rngOrNull, const char *ctxMsg)
{
    CaptureSink sink(cap);
    std::size_t pos = 0U;
    std::size_t totalFrameBytes = 0U;

    cap.clear();
    TEST_ASSERT_EQUAL_MESSAGE(demux::Error::kOk, s_dmx.init(decs, s_demux_buf, sink), ctxMsg);

    while (pos < data.size()) {
        const std::size_t remain = data.size() - pos;
        std::size_t chunk = remain;

        if (rngOrNull != nullptr) {
            const uint32_t r = xorshift32(*rngOrNull);
            const uint32_t mode = r % 10U;
            if (mode == 0U) {
                chunk = 0U; // 故意穿插 0 字节喂入
            } else if (mode <= 3U) {
                chunk = 1U;
            } else if (mode <= 6U) {
                chunk = static_cast<std::size_t>((r >> 8U) % 19U) + 1U;
            } else {
                chunk = static_cast<std::size_t>((r >> 8U) % 257U) + 1U;
            }
            if (chunk > remain) {
                chunk = remain;
            }
        } else if ((fixedChunk > 0U) && (chunk > fixedChunk)) {
            chunk = fixedChunk;
        } else {
            // 整块喂入
        }

        TEST_ASSERT_EQUAL_MESSAGE(demux::Error::kOk, s_dmx.feed(data.subspan(pos, chunk)), ctxMsg);
        pos += chunk;
    }

    TEST_ASSERT_EQUAL_MESSAGE(demux::Error::kOk, s_dmx.abort(), ctxMsg);
    TEST_ASSERT_FALSE_MESSAGE(s_dmx.busy(), ctxMsg);
    TEST_ASSERT_FALSE_MESSAGE(cap.overflowed, ctxMsg);

    // I2 字节守恒：帧总字节数 + 未知字节数 == 输入总字节数
    const demux::Stats &st = s_dmx.stats();
    for (std::size_t i = 0U; i < decs.size(); ++i) {
        totalFrameBytes += st.decoder[i].bytes;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(cap.frameBlobUsed, totalFrameBytes, ctxMsg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(cap.unknownLen, st.unknownBytes, ctxMsg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(data.size(), cap.frameBlobUsed + cap.unknownLen, ctxMsg);
}

void assertFramesMatchTruth(const StreamCapture &truth, const StreamCapture &got,
                            const char *ctxMsg)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(truth.frameCount, got.frameCount, ctxMsg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(truth.frameBlobUsed, got.frameBlobUsed, ctxMsg);
    for (std::size_t i = 0U; i < truth.frameCount; ++i) {
        TEST_ASSERT_EQUAL_PTR_MESSAGE(truth.meta[i].decoder, got.meta[i].decoder, ctxMsg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(truth.meta[i].decoderIndex, got.meta[i].decoderIndex,
                                        ctxMsg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(truth.meta[i].len, got.meta[i].len, ctxMsg);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&truth.frameBlob[truth.meta[i].offset],
                                             &got.frameBlob[got.meta[i].offset], truth.meta[i].len,
                                             ctxMsg);
    }
}

void assertCapturesEqual(const StreamCapture &exp, const StreamCapture &got, const char *ctxMsg)
{
    assertFramesMatchTruth(exp, got, ctxMsg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(exp.unknownLen, got.unknownLen, ctxMsg);
    if (exp.unknownLen > 0U) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(exp.unknownBlob.data(), got.unknownBlob.data(),
                                             exp.unknownLen, ctxMsg);
    }
}

// 向流追加假前导与随机垃圾（避开 '\n'，防止意外拼出合法 NMEA/Hash 行尾）
std::size_t appendFakeGarbage(std::span<uint8_t> dst, uint32_t &rng) noexcept
{
    static constexpr std::array<uint8_t, 5> kF0{0xD3U, 0x00U, 0x10U, 0x01U, 0x02U};
    static constexpr std::array<uint8_t, 3> kF1{0xD3U, 0xFCU, 0x00U};
    static constexpr std::array<uint8_t, 2> kF2{0xAAU, 0x44U};
    static constexpr std::array<uint8_t, 7> kF3{0xAAU, 0x44U, 0x18U, 0x14U, 0x01U, 0x00U, 0x00U};
    static constexpr std::array<uint8_t, 8> kF4{0xAAU, 0x44U, 0xB5U, 0x20U,
                                                0x05U, 0x00U, 0x08U, 0x00U};
    static constexpr std::array<uint8_t, 7> kF5{'$', 'G', 'P', 'G', 'G', 'A', ','};
    static constexpr std::array<uint8_t, 5> kF6{'$', '@', 'S', 'B', 'F'};
    static constexpr std::array<uint8_t, 5> kF7{'#', 'V', 'E', 'R', ','};
    static constexpr std::array<uint8_t, 5> kF8{'#', '0', 'B', 'A', 'D'};
    static constexpr std::array<uint8_t, 8> kF9{0xB5U, 0x62U, 0x01U, 0x07U,
                                                0x20U, 0x00U, 0x11U, 0x22U};
    static constexpr std::array<uint8_t, 2> kF10{0xB5U, 0x61U};
    static constexpr std::array<uint8_t, 1> kF11{0xAAU};
    static constexpr std::array<uint8_t, 1> kF12{0xD3U};
    static constexpr std::array<std::span<const uint8_t>, 13> kFakes{
        kF0, kF1, kF2, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF10, kF11, kF12,
    };

    std::size_t pos = 0U;
    const std::size_t randLen = static_cast<std::size_t>(xorshift32(rng) % 7U);
    const std::size_t fakeIdx = static_cast<std::size_t>(xorshift32(rng) % kFakes.size());

    for (std::size_t i = 0U; (i < randLen) && (pos < dst.size()); ++i) {
        // 掩码后范围为 0..255
        auto b = static_cast<uint8_t>(xorshift32(rng) & 0xFFU);
        if ((b == static_cast<uint8_t>('\n')) || (b == 0x62U)) {
            b = 0x5AU; // 避免随机字节偶发凑成 UBX 0xB5 0x62 或行结束符
        }
        dst[pos++] = b;
    }
    const std::span<const uint8_t> fake = kFakes[fakeIdx];
    if ((pos + fake.size()) <= dst.size()) {
        std::copy(fake.begin(), fake.end(), dst.subspan(pos).begin());
        pos += fake.size();
    }
    return pos;
}

// 按协议类型生成随机真帧并记录到 truth
std::size_t appendRandomProtoFrame(ProtoKind kind, const demux::Decoder *decoderPtr, uint8_t decIdx,
                                   std::span<uint8_t> dst, uint32_t &rng, StreamCapture *truth)
{
    std::array<uint8_t, 260> payload{};
    std::array<char, 96> text{};
    std::size_t flen = 0U;
    bool ok = false;

    switch (kind) {
    case ProtoKind::kRtcm3: {
        const std::size_t plen = static_cast<std::size_t>(xorshift32(rng) % 120U);
        for (std::size_t i = 0U; i < plen; ++i) {
            payload[i] = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        }
        ok = demux_test::genRtcm3(dst, std::span<const uint8_t>(payload.data(), plen), flen);
        break;
    }
    case ProtoKind::kNmea: {
        const uint32_t seq = xorshift32(rng) % 10000U;
        const bool lc = (xorshift32(rng) & 1U) != 0U;
        const int n =
            std::snprintf(text.data(), text.size(), "GNGGA,1235%02u,4807.038,N,01131.000,E,1,08,%u",
                          static_cast<unsigned>(seq % 60U), static_cast<unsigned>(seq));
        TEST_ASSERT_TRUE(n > 0);
        // n > 0 且受 text.size() 限制
        ok = demux_test::genNmea(dst, std::string_view(text.data(), static_cast<std::size_t>(n)),
                                 lc, flen);
        break;
    }
    case ProtoKind::kUbx: {
        const std::size_t plen = static_cast<std::size_t>(xorshift32(rng) % 64U);
        for (std::size_t i = 0U; i < plen; ++i) {
            payload[i] = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        }
        const auto msgId = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        ok = demux_test::genUbx(dst, 0x01U, msgId, std::span<const uint8_t>(payload.data(), plen),
                                flen);
        break;
    }
    case ProtoKind::kUnicoreBin: {
        const std::size_t plen = static_cast<std::size_t>(xorshift32(rng) % 80U);
        for (std::size_t i = 0U; i < plen; ++i) {
            payload[i] = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        }
        const auto msgId = static_cast<uint16_t>(xorshift32(rng) & 0xFFFFU); // 掩码后 0..65535
        ok = demux_test::genUnicoreBin(dst, msgId, 42U, {},
                                       std::span<const uint8_t>(payload.data(), plen), flen);
        break;
    }
    case ProtoKind::kUnicoreHash: {
        const uint32_t seq = xorshift32(rng) % 100000U;
        const bool lc = (xorshift32(rng) & 1U) != 0U;
        const bool useXor = (xorshift32(rng) & 2U) != 0U;
        const int n = std::snprintf(text.data(), text.size(), "BESTPOSA,97,GPS;%u,SOL_COMPUTED",
                                    static_cast<unsigned>(seq));
        TEST_ASSERT_TRUE(n > 0);
        // n > 0 且受 text.size() 限制
        const std::string_view sv(text.data(), static_cast<std::size_t>(n));
        if (useXor) {
            ok = demux_test::genUnicoreHashXor(dst, sv, lc, flen);
        } else {
            ok = demux_test::genUnicoreHashCrc32(dst, sv, lc, flen);
        }
        break;
    }
    case ProtoKind::kProduct: {
        const std::size_t plen = static_cast<std::size_t>(xorshift32(rng) % 64U);
        for (std::size_t i = 0U; i < plen; ++i) {
            payload[i] = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        }
        const auto msgId = static_cast<uint16_t>(xorshift32(rng) & 0xFFFFU); // 掩码后 0..65535
        ok = demux_test::genProduct(dst, msgId, proto::sender::kApp, proto::msg_type::kSet,
                                    std::span<const uint8_t>(payload.data(), plen), flen);
        break;
    }
    default:
        return 0U;
    }

    TEST_ASSERT_TRUE(ok);
    if (truth != nullptr) {
        TEST_ASSERT_TRUE(truth->frameCount < kMaxFramesRec);
        TEST_ASSERT_TRUE((truth->frameBlobUsed + flen) <= kBlobCap);
        FrameMeta &m = truth->meta[truth->frameCount];
        m.decoder = decoderPtr;
        m.decoderIndex = decIdx;
        m.offset = truth->frameBlobUsed;
        m.len = flen;
        std::copy(dst.begin(), dst.begin() + static_cast<std::ptrdiff_t>(flen),
                  truth->frameBlob.begin() + static_cast<std::ptrdiff_t>(truth->frameBlobUsed));
        truth->frameBlobUsed += flen;
        truth->frameCount++;
    }
    return flen;
}

std::size_t buildAllProtoMixedStream(uint32_t seed, std::span<uint8_t> buf, std::size_t frameCount,
                                     StreamCapture *truth)
{
    uint32_t rng = seed;
    std::size_t pos = 0U;

    if (truth != nullptr) {
        truth->clear();
    }
    for (std::size_t i = 0U; i < frameCount; ++i) {
        const std::size_t idx = static_cast<std::size_t>(xorshift32(rng) % kActiveProtoCount);
        const ActiveProtoEntry &entry = kActiveProtos[idx];
        pos += appendFakeGarbage(buf.subspan(pos), rng);
        // idx < kActiveProtoCount <= 6，在 uint8_t 范围内
        pos += appendRandomProtoFrame(entry.kind, entry.decoder, static_cast<uint8_t>(idx),
                                      buf.subspan(pos), rng, truth);
    }
    pos += appendFakeGarbage(buf.subspan(pos), rng);
    return pos;
}

std::array<const demux::Decoder *, kActiveProtoCount> getActiveDecoders() noexcept
{
    std::array<const demux::Decoder *, kActiveProtoCount> out{};
    for (std::size_t i = 0U; i < kActiveProtoCount; ++i) {
        out[i] = kActiveProtos[i].decoder;
    }
    return out;
}

} // namespace

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

// --------------------------------------------------------------------------
// 测试用例
// --------------------------------------------------------------------------

static void testPropertyI1ChunkIndependence1000Splits()
{
    const auto decs = getActiveDecoders();
    uint32_t rng = kBasePrngSeed;
    std::array<char, kFailMsgCap> msg{};
    const std::span<uint8_t> stream(s_stream_buf);

    const std::size_t streamLen =
        buildAllProtoMixedStream(kBasePrngSeed, stream, 36U, &s_truth_cap);
    TEST_ASSERT_TRUE(streamLen > 1000U);

    // 参考运行 0：整块喂入
    runChunked(decs, stream.subspan(0, streamLen), s_ref_cap, 0U, nullptr, "whole-buffer");
    TEST_ASSERT_EQUAL_UINT(s_truth_cap.frameCount, s_ref_cap.frameCount);

    // 参考运行 1：逐字节喂入
    runChunked(decs, stream.subspan(0, streamLen), s_cur_cap, 1U, nullptr, "1-byte-chunk");
    assertCapturesEqual(s_ref_cap, s_cur_cap, "1-byte-vs-whole");

    // >= 1000 轮随机切分
    for (uint32_t round = 0U; round < kSplitRounds; ++round) {
        const uint32_t roundSeed = rng;
        static_cast<void>(std::snprintf(
            msg.data(), msg.size(),
            "I1 split failed: base_seed=0x%08lX round=%lu round_rng=0x%08lX",
            static_cast<unsigned long>(kBasePrngSeed), static_cast<unsigned long>(round),
            static_cast<unsigned long>(roundSeed)));
        runChunked(decs, stream.subspan(0, streamLen), s_cur_cap, 0U, &rng, msg.data());
        assertCapturesEqual(s_ref_cap, s_cur_cap, msg.data());
    }
}

static void testPropertyMixedStreamFalsePreamblesRecovery()
{
    const auto decs = getActiveDecoders();
    uint32_t masterRng = 0xA1B2C3D4U;
    std::array<char, kFailMsgCap> msg{};
    const std::span<uint8_t> stream(s_stream_buf);

    for (uint32_t iter = 0U; iter < kMixedSeedRounds; ++iter) {
        const uint32_t streamSeed = xorshift32(masterRng);
        uint32_t splitRng = streamSeed ^ 0x9E3779B9U;
        const std::size_t streamLen =
            buildAllProtoMixedStream(streamSeed, stream, 28U, &s_truth_cap);

        static_cast<void>(std::snprintf(
            msg.data(), msg.size(), "mixed recovery failed: iter=%lu stream_seed=0x%08lX",
            static_cast<unsigned long>(iter), static_cast<unsigned long>(streamSeed)));
        runChunked(decs, stream.subspan(0, streamLen), s_cur_cap, 0U, &splitRng, msg.data());

        // abort 后所有真帧必须按序恢复且逐字节相同
        TEST_ASSERT_EQUAL_UINT_MESSAGE(s_truth_cap.frameCount, s_cur_cap.frameCount, msg.data());
        for (std::size_t i = 0U; i < s_truth_cap.frameCount; ++i) {
            TEST_ASSERT_EQUAL_PTR_MESSAGE(s_truth_cap.meta[i].decoder, s_cur_cap.meta[i].decoder,
                                          msg.data());
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_truth_cap.meta[i].decoderIndex,
                                            s_cur_cap.meta[i].decoderIndex, msg.data());
            TEST_ASSERT_EQUAL_UINT_MESSAGE(s_truth_cap.meta[i].len, s_cur_cap.meta[i].len,
                                           msg.data());
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&s_truth_cap.frameBlob[s_truth_cap.meta[i].offset],
                                                 &s_cur_cap.frameBlob[s_cur_cap.meta[i].offset],
                                                 s_truth_cap.meta[i].len, msg.data());
        }
    }
}

#if STREAM_DEMUX_RTCM3
static void testScenarioBluetoothProductAndRtcm3()
{
    static constexpr std::array<const demux::Decoder *, 2> kBtDecs{
        &proto::demux_frame::kDecoder,
        &demux::rtcm3::kDecoder,
    };
    using BtStaticDemux = demux::StaticDemux<proto::demux_frame::kDecoder, demux::rtcm3::kDecoder>;
    static_assert(BtStaticDemux::kBufSize == demux::rtcm3::kMaxFrame);
    static_assert(BtStaticDemux::indexOf<proto::demux_frame::kDecoder>() == 0U);
    static_assert(BtStaticDemux::indexOf<demux::rtcm3::kDecoder>() == 1U);

    uint32_t rng = 0x20261111U;
    std::size_t pos = 0U;
    const std::span<uint8_t> stream(s_stream_buf);

    s_truth_cap.clear();
    for (std::size_t i = 0U; i < 30U; ++i) {
        const bool isRtcm = (i % 2U) != 0U;
        if ((i % 3U) == 0U) {
            pos += appendFakeGarbage(stream.subspan(pos), rng);
        }
        pos +=
            appendRandomProtoFrame(isRtcm ? ProtoKind::kRtcm3 : ProtoKind::kProduct,
                                   isRtcm ? &demux::rtcm3::kDecoder : &proto::demux_frame::kDecoder,
                                   isRtcm ? BtStaticDemux::indexOf<demux::rtcm3::kDecoder>()
                                          : BtStaticDemux::indexOf<proto::demux_frame::kDecoder>(),
                                   stream.subspan(pos), rng, &s_truth_cap);
    }

    runChunked(kBtDecs, stream.subspan(0, pos), s_ref_cap, 0U, nullptr, "bt-whole");
    runChunked(kBtDecs, stream.subspan(0, pos), s_cur_cap, 0U, &rng, "bt-chunked");
    assertCapturesEqual(s_ref_cap, s_cur_cap, "bt-i1");
    TEST_ASSERT_EQUAL_UINT(s_truth_cap.frameCount, s_cur_cap.frameCount);

    // StaticDemux 同流端到端核对与 demux_frame::view 解码
    BtStaticDemux btSd;
    CaptureSink sink(s_cur_cap);
    s_cur_cap.clear();
    TEST_ASSERT_EQUAL(demux::Error::kOk, btSd.init(sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, btSd.feed(stream.subspan(0, pos)));
    TEST_ASSERT_EQUAL(demux::Error::kOk, btSd.abort());
    assertCapturesEqual(s_ref_cap, s_cur_cap, "bt-static-demux");

    for (std::size_t i = 0U; i < s_cur_cap.frameCount; ++i) {
        if (s_cur_cap.meta[i].decoder == &proto::demux_frame::kDecoder) {
            const std::span<const uint8_t> wire(&s_cur_cap.frameBlob[s_cur_cap.meta[i].offset],
                                                s_cur_cap.meta[i].len);
            const std::optional<proto::FrameView> fv = proto::demux_frame::view(wire);
            TEST_ASSERT_TRUE(fv.has_value());
            TEST_ASSERT_EQUAL_UINT(proto::kHeaderLen + fv->payload.size() + proto::kCrcLen,
                                   s_cur_cap.meta[i].len);
        }
    }
}
#endif

static void testScenarioGnssUnicoreAndUbloxSets()
{
    const std::span<uint8_t> stream(s_stream_buf);

#if STREAM_DEMUX_NMEA && STREAM_DEMUX_RTCM3 && STREAM_DEMUX_UNICORE_BIN && STREAM_DEMUX_UNICORE_HASH
    // GNSS 场景 A（Unicore UM980）：{nmea, rtcm3, unicore_bin, unicore_hash}
    {
        static constexpr std::array<const demux::Decoder *, 4> kUm980Decs{
            &demux::nmea::kDecoder,
            &demux::rtcm3::kDecoder,
            &demux::unicore_bin::kDecoder,
            &demux::unicore_hash::kDecoder,
        };
        static constexpr std::array<ProtoKind, 4> kPicks{
            ProtoKind::kNmea,
            ProtoKind::kRtcm3,
            ProtoKind::kUnicoreBin,
            ProtoKind::kUnicoreHash,
        };
        uint32_t rng = 0x98009800U;
        std::size_t pos = 0U;

        s_truth_cap.clear();
        for (std::size_t i = 0U; i < 24U; ++i) {
            const std::size_t slot = i % 4U;
            pos += appendFakeGarbage(stream.subspan(pos), rng);
            // slot < 4，在 uint8_t 范围内
            pos +=
                appendRandomProtoFrame(kPicks[slot], kUm980Decs[slot], static_cast<uint8_t>(slot),
                                       stream.subspan(pos), rng, &s_truth_cap);
        }
        runChunked(kUm980Decs, stream.subspan(0, pos), s_ref_cap, 0U, nullptr, "gnss-um980-whole");
        runChunked(kUm980Decs, stream.subspan(0, pos), s_cur_cap, 0U, &rng, "gnss-um980");
        assertCapturesEqual(s_ref_cap, s_cur_cap, "gnss-um980-i1");
        assertFramesMatchTruth(s_truth_cap, s_cur_cap, "gnss-um980-truth");
    }
#endif

#if STREAM_DEMUX_NMEA && STREAM_DEMUX_RTCM3 && STREAM_DEMUX_UBX
    // GNSS 场景 B（u-blox F9P）：{nmea, rtcm3, ubx}
    {
        static constexpr std::array<const demux::Decoder *, 3> kF9pDecs{
            &demux::nmea::kDecoder,
            &demux::rtcm3::kDecoder,
            &demux::ubx::kDecoder,
        };
        static constexpr std::array<ProtoKind, 3> kPicks{
            ProtoKind::kNmea,
            ProtoKind::kRtcm3,
            ProtoKind::kUbx,
        };
        uint32_t rng = 0xF9BF9B01U;
        std::size_t pos = 0U;

        s_truth_cap.clear();
        for (std::size_t i = 0U; i < 24U; ++i) {
            const std::size_t slot = i % 3U;
            pos += appendFakeGarbage(stream.subspan(pos), rng);
            // slot < 3，在 uint8_t 范围内
            pos += appendRandomProtoFrame(kPicks[slot], kF9pDecs[slot], static_cast<uint8_t>(slot),
                                          stream.subspan(pos), rng, &s_truth_cap);
        }
        runChunked(kF9pDecs, stream.subspan(0, pos), s_ref_cap, 0U, nullptr, "gnss-f9p-whole");
        runChunked(kF9pDecs, stream.subspan(0, pos), s_cur_cap, 0U, &rng, "gnss-f9p");
        assertCapturesEqual(s_ref_cap, s_cur_cap, "gnss-f9p-i1");
        assertFramesMatchTruth(s_truth_cap, s_cur_cap, "gnss-f9p-truth");
    }
#endif
}

#if STREAM_DEMUX_UNICORE_BIN
static void testScenarioProductVsUnicoreBinConflict()
{
    static constexpr std::array<const demux::Decoder *, 2> kOrderAb{
        &proto::demux_frame::kDecoder,
        &demux::unicore_bin::kDecoder,
    };
    static constexpr std::array<const demux::Decoder *, 2> kOrderBa{
        &demux::unicore_bin::kDecoder,
        &proto::demux_frame::kDecoder,
    };
    uint32_t rng = 0xAA4418B5U;
    std::size_t pos = 0U;
    const std::span<uint8_t> stream(s_stream_buf);

    s_truth_cap.clear();
    for (std::size_t i = 0U; i < 16U; ++i) {
        const bool isUnicore = (i % 2U) != 0U;
        pos += appendRandomProtoFrame(isUnicore ? ProtoKind::kUnicoreBin : ProtoKind::kProduct,
                                      isUnicore ? &demux::unicore_bin::kDecoder
                                                : &proto::demux_frame::kDecoder,
                                      isUnicore ? 1U : 0U, stream.subspan(pos), rng, &s_truth_cap);
    }

    // 无垃圾交错：即使回放到次优先级解码器，resyncs == 0，unknownBytes == 0
    runChunked(kOrderAb, stream.subspan(0, pos), s_cur_cap, 7U, nullptr, "aa-conflict-ab");
    TEST_ASSERT_EQUAL_UINT(16U, s_cur_cap.frameCount);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknownBytes);

    // 对调优先级：{unicore_bin, product}
    runChunked(kOrderBa, stream.subspan(0, pos), s_cur_cap, 5U, nullptr, "aa-conflict-ba");
    TEST_ASSERT_EQUAL_UINT(16U, s_cur_cap.frameCount);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknownBytes);
    for (std::size_t i = 0U; i < s_cur_cap.frameCount; ++i) {
        const bool isUnicore = (i % 2U) != 0U;
        const demux::Decoder *expDec =
            isUnicore ? &demux::unicore_bin::kDecoder : &proto::demux_frame::kDecoder;
        const uint8_t expIdx = isUnicore ? 0U : 1U;
        TEST_ASSERT_EQUAL_PTR(expDec, s_cur_cap.meta[i].decoder);
        TEST_ASSERT_EQUAL_UINT8(expIdx, s_cur_cap.meta[i].decoderIndex);
    }

    // 假 AA 44 前导紧跟真产品帧/Unicore 帧 + 视图校验
    {
        static constexpr std::array<uint8_t, 3> kFakeAa1{0xAAU, 0x44U, 0x99U};
        static constexpr std::array<uint8_t, 2> kFakeAa2{0xAAU, 0x44U};
        static constexpr std::array<uint8_t, 1> kFakeAa3{0xAAU};
        std::size_t flen = 0U;

        pos = 0U;
        s_truth_cap.clear();
        std::copy(kFakeAa1.begin(), kFakeAa1.end(), stream.subspan(pos).begin());
        pos += kFakeAa1.size();
        pos += appendRandomProtoFrame(ProtoKind::kProduct, &proto::demux_frame::kDecoder, 0U,
                                      stream.subspan(pos), rng, &s_truth_cap);
        std::copy(kFakeAa2.begin(), kFakeAa2.end(), stream.subspan(pos).begin());
        pos += kFakeAa2.size();
        pos += appendRandomProtoFrame(ProtoKind::kUnicoreBin, &demux::unicore_bin::kDecoder, 1U,
                                      stream.subspan(pos), rng, &s_truth_cap);
        std::copy(kFakeAa3.begin(), kFakeAa3.end(), stream.subspan(pos).begin());
        pos += kFakeAa3.size();
        TEST_ASSERT_TRUE(demux_test::genProduct(stream.subspan(pos), 0x0042U, proto::sender::kApp,
                                                proto::msg_type::kSet, {}, flen));
        s_truth_cap.meta[s_truth_cap.frameCount].decoder = &proto::demux_frame::kDecoder;
        s_truth_cap.meta[s_truth_cap.frameCount].decoderIndex = 0U;
        s_truth_cap.meta[s_truth_cap.frameCount].offset = s_truth_cap.frameBlobUsed;
        s_truth_cap.meta[s_truth_cap.frameCount].len = flen;
        std::copy(stream.subspan(pos, flen).begin(), stream.subspan(pos, flen).end(),
                  s_truth_cap.frameBlob.begin() +
                      static_cast<std::ptrdiff_t>(s_truth_cap.frameBlobUsed));
        s_truth_cap.frameBlobUsed += flen;
        s_truth_cap.frameCount++;
        pos += flen;

        runChunked(kOrderAb, stream.subspan(0, pos), s_cur_cap, 3U, nullptr,
                   "aa-conflict-fake-preambles");
        TEST_ASSERT_EQUAL_UINT(s_truth_cap.frameCount, s_cur_cap.frameCount);
        for (std::size_t i = 0U; i < s_truth_cap.frameCount; ++i) {
            TEST_ASSERT_EQUAL_PTR(s_truth_cap.meta[i].decoder, s_cur_cap.meta[i].decoder);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_truth_cap.frameBlob[s_truth_cap.meta[i].offset],
                                         &s_cur_cap.frameBlob[s_cur_cap.meta[i].offset],
                                         s_truth_cap.meta[i].len);
        }
    }
}
#endif

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testPropertyI1ChunkIndependence1000Splits);
    RUN_TEST(testPropertyMixedStreamFalsePreamblesRecovery);
#if STREAM_DEMUX_RTCM3
    RUN_TEST(testScenarioBluetoothProductAndRtcm3);
#endif
    RUN_TEST(testScenarioGnssUnicoreAndUbloxSets);
#if STREAM_DEMUX_UNICORE_BIN
    RUN_TEST(testScenarioProductVsUnicoreBinConflict);
#endif
    return UNITY_END();
}
