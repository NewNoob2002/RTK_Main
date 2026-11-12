// 通用字节流分流器属性与场景流测试（ADR-0005 / ADR-0004 §3、§4）。
// 覆盖：I1 分块无关（>=1000 轮随机切分）、I2 字节守恒、假前导混合流恢复、蓝牙/GNSS 场景与 AA 冲突。
// 随 STREAM_DEMUX_<协议> 编译定义自动裁剪对应解码器及场景。
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

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

#include "rtk/demux/decoder.hpp"
#include "rtk/demux/demux.hpp"
#include "rtk/demux/static_demux.hpp"
#if STREAM_DEMUX_NMEA
#include "rtk/demux/decoders/nmea.hpp"
#endif
#if STREAM_DEMUX_RTCM3
#include "rtk/demux/decoders/rtcm3.hpp"
#endif
#if STREAM_DEMUX_UBX
#include "rtk/demux/decoders/ubx.hpp"
#endif
#if STREAM_DEMUX_UNICORE_BIN
#include "rtk/demux/decoders/unicore_bin.hpp"
#endif
#if STREAM_DEMUX_UNICORE_HASH
#include "rtk/demux/decoders/unicore_hash.hpp"
#endif
#include "demux_gen.hpp"
#include "rtk/proto/demux_frame.hpp"
#include "rtk/proto/frame.hpp"
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
    const rtk::demux::Decoder *decoder = nullptr;
};

struct FrameMeta {
    const rtk::demux::Decoder *decoder = nullptr;
    uint8_t decoder_index = 0U;
    std::size_t offset = 0U;
    std::size_t len = 0U;
};

struct StreamCapture {
    void clear() noexcept
    {
        frame_count = 0U;
        frame_blob_used = 0U;
        unknown_len = 0U;
        overflowed = false;
    }

    std::array<FrameMeta, kMaxFramesRec> meta{};
    std::size_t frame_count = 0U;
    std::array<uint8_t, kBlobCap> frame_blob{};
    std::size_t frame_blob_used = 0U;
    std::array<uint8_t, kStreamCap> unknown_blob{};
    std::size_t unknown_len = 0U;
    bool overflowed = false;
};

class CaptureSink final : public rtk::demux::Sink {
public:
    explicit CaptureSink(StreamCapture &cap) noexcept : cap_(&cap) {}

    void on_frame(const rtk::demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> frame) noexcept override
    {
        if ((cap_->frame_count >= kMaxFramesRec) || ((cap_->frame_blob_used + frame.size()) > kBlobCap)) {
            cap_->overflowed = true;
            return;
        }
        FrameMeta &m = cap_->meta[cap_->frame_count];
        m.decoder = &decoder;
        m.decoder_index = index;
        m.offset = cap_->frame_blob_used;
        m.len = frame.size();
        if (!frame.empty()) {
            std::copy(frame.begin(), frame.end(),
                      cap_->frame_blob.begin() + static_cast<std::ptrdiff_t>(cap_->frame_blob_used));
        }
        cap_->frame_blob_used += frame.size();
        cap_->frame_count++;
    }

    void on_unknown(std::span<const uint8_t> bytes) noexcept override
    {
        if ((cap_->unknown_len + bytes.size()) > kStreamCap) {
            cap_->overflowed = true;
            return;
        }
        if (!bytes.empty()) {
            std::copy(bytes.begin(), bytes.end(),
                      cap_->unknown_blob.begin() + static_cast<std::ptrdiff_t>(cap_->unknown_len));
        }
        cap_->unknown_len += bytes.size();
    }

private:
    StreamCapture *cap_;
};

std::array<uint8_t, kDemuxBufSize> s_demux_buf{};
rtk::demux::Demux s_dmx;
StreamCapture s_ref_cap;
StreamCapture s_cur_cap;
StreamCapture s_truth_cap;
std::array<uint8_t, kStreamCap> s_stream_buf{};

// 当前构建开启的协议列表（产品帧优先于 Unicore 二进制）
constexpr std::array kActiveProtos{
    ActiveProtoEntry{ProtoKind::kProduct, &rtk::proto::demux_frame::kDecoder},
#if STREAM_DEMUX_UNICORE_BIN
    ActiveProtoEntry{ProtoKind::kUnicoreBin, &rtk::demux::unicore_bin::kDecoder},
#endif
#if STREAM_DEMUX_RTCM3
    ActiveProtoEntry{ProtoKind::kRtcm3, &rtk::demux::rtcm3::kDecoder},
#endif
#if STREAM_DEMUX_NMEA
    ActiveProtoEntry{ProtoKind::kNmea, &rtk::demux::nmea::kDecoder},
#endif
#if STREAM_DEMUX_UNICORE_HASH
    ActiveProtoEntry{ProtoKind::kUnicoreHash, &rtk::demux::unicore_hash::kDecoder},
#endif
#if STREAM_DEMUX_UBX
    ActiveProtoEntry{ProtoKind::kUbx, &rtk::demux::ubx::kDecoder},
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

void run_chunked(std::span<const rtk::demux::Decoder *const> decs, std::span<const uint8_t> data, StreamCapture &cap,
                 std::size_t fixed_chunk, uint32_t *rng_or_null, const char *ctx_msg)
{
    CaptureSink sink(cap);
    std::size_t pos = 0U;
    std::size_t total_frame_bytes = 0U;

    cap.clear();
    TEST_ASSERT_EQUAL_MESSAGE(rtk::demux::Error::kOk, s_dmx.init(decs, s_demux_buf, sink), ctx_msg);

    while (pos < data.size()) {
        const std::size_t remain = data.size() - pos;
        std::size_t chunk = remain;

        if (rng_or_null != nullptr) {
            const uint32_t r = xorshift32(*rng_or_null);
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
        } else if ((fixed_chunk > 0U) && (chunk > fixed_chunk)) {
            chunk = fixed_chunk;
        } else {
            // 整块喂入
        }

        TEST_ASSERT_EQUAL_MESSAGE(rtk::demux::Error::kOk, s_dmx.feed(data.subspan(pos, chunk)), ctx_msg);
        pos += chunk;
    }

    TEST_ASSERT_EQUAL_MESSAGE(rtk::demux::Error::kOk, s_dmx.abort(), ctx_msg);
    TEST_ASSERT_FALSE_MESSAGE(s_dmx.busy(), ctx_msg);
    TEST_ASSERT_FALSE_MESSAGE(cap.overflowed, ctx_msg);

    // I2 字节守恒：帧总字节数 + 未知字节数 == 输入总字节数
    const rtk::demux::Stats &st = s_dmx.stats();
    for (std::size_t i = 0U; i < decs.size(); ++i) {
        total_frame_bytes += st.decoder[i].bytes;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(cap.frame_blob_used, total_frame_bytes, ctx_msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(cap.unknown_len, st.unknown_bytes, ctx_msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(data.size(), cap.frame_blob_used + cap.unknown_len, ctx_msg);
}

void assert_frames_match_truth(const StreamCapture &truth, const StreamCapture &got, const char *ctx_msg)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(truth.frame_count, got.frame_count, ctx_msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(truth.frame_blob_used, got.frame_blob_used, ctx_msg);
    for (std::size_t i = 0U; i < truth.frame_count; ++i) {
        TEST_ASSERT_EQUAL_PTR_MESSAGE(truth.meta[i].decoder, got.meta[i].decoder, ctx_msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(truth.meta[i].decoder_index, got.meta[i].decoder_index, ctx_msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(truth.meta[i].len, got.meta[i].len, ctx_msg);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&truth.frame_blob[truth.meta[i].offset],
                                             &got.frame_blob[got.meta[i].offset], truth.meta[i].len, ctx_msg);
    }
}

void assert_captures_equal(const StreamCapture &exp, const StreamCapture &got, const char *ctx_msg)
{
    assert_frames_match_truth(exp, got, ctx_msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(exp.unknown_len, got.unknown_len, ctx_msg);
    if (exp.unknown_len > 0U) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(exp.unknown_blob.data(), got.unknown_blob.data(), exp.unknown_len,
                                             ctx_msg);
    }
}

// 向流追加假前导与随机垃圾（避开 '\n'，防止意外拼出合法 NMEA/Hash 行尾）
std::size_t append_fake_garbage(std::span<uint8_t> dst, uint32_t &rng) noexcept
{
    static constexpr std::array<uint8_t, 5> kF0{0xD3U, 0x00U, 0x10U, 0x01U, 0x02U};
    static constexpr std::array<uint8_t, 3> kF1{0xD3U, 0xFCU, 0x00U};
    static constexpr std::array<uint8_t, 2> kF2{0xAAU, 0x44U};
    static constexpr std::array<uint8_t, 7> kF3{0xAAU, 0x44U, 0x18U, 0x14U, 0x01U, 0x00U, 0x00U};
    static constexpr std::array<uint8_t, 8> kF4{0xAAU, 0x44U, 0xB5U, 0x20U, 0x05U, 0x00U, 0x08U, 0x00U};
    static constexpr std::array<uint8_t, 7> kF5{'$', 'G', 'P', 'G', 'G', 'A', ','};
    static constexpr std::array<uint8_t, 5> kF6{'$', '@', 'S', 'B', 'F'};
    static constexpr std::array<uint8_t, 5> kF7{'#', 'V', 'E', 'R', ','};
    static constexpr std::array<uint8_t, 5> kF8{'#', '0', 'B', 'A', 'D'};
    static constexpr std::array<uint8_t, 8> kF9{0xB5U, 0x62U, 0x01U, 0x07U, 0x20U, 0x00U, 0x11U, 0x22U};
    static constexpr std::array<uint8_t, 2> kF10{0xB5U, 0x61U};
    static constexpr std::array<uint8_t, 1> kF11{0xAAU};
    static constexpr std::array<uint8_t, 1> kF12{0xD3U};
    static constexpr std::array<std::span<const uint8_t>, 13> kFakes{
        kF0, kF1, kF2, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF10, kF11, kF12,
    };

    std::size_t pos = 0U;
    const std::size_t rand_len = static_cast<std::size_t>(xorshift32(rng) % 7U);
    const std::size_t fake_idx = static_cast<std::size_t>(xorshift32(rng) % kFakes.size());

    for (std::size_t i = 0U; (i < rand_len) && (pos < dst.size()); ++i) {
        // 掩码后范围为 0..255
        auto b = static_cast<uint8_t>(xorshift32(rng) & 0xFFU);
        if ((b == static_cast<uint8_t>('\n')) || (b == 0x62U)) {
            b = 0x5AU; // 避免随机字节偶发凑成 UBX 0xB5 0x62 或行结束符
        }
        dst[pos++] = b;
    }
    const std::span<const uint8_t> fake = kFakes[fake_idx];
    if ((pos + fake.size()) <= dst.size()) {
        std::copy(fake.begin(), fake.end(), dst.subspan(pos).begin());
        pos += fake.size();
    }
    return pos;
}

// 按协议类型生成随机真帧并记录到 truth
std::size_t append_random_proto_frame(ProtoKind kind, const rtk::demux::Decoder *decoder_ptr, uint8_t dec_idx,
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
        ok = demux_test::gen_rtcm3(dst, std::span<const uint8_t>(payload.data(), plen), flen);
        break;
    }
    case ProtoKind::kNmea: {
        const uint32_t seq = xorshift32(rng) % 10000U;
        const bool lc = (xorshift32(rng) & 1U) != 0U;
        const int n = std::snprintf(text.data(), text.size(), "GNGGA,1235%02u,4807.038,N,01131.000,E,1,08,%u",
                                    static_cast<unsigned>(seq % 60U), static_cast<unsigned>(seq));
        TEST_ASSERT_TRUE(n > 0);
        // n > 0 且受 text.size() 限制
        ok = demux_test::gen_nmea(dst, std::string_view(text.data(), static_cast<std::size_t>(n)), lc, flen);
        break;
    }
    case ProtoKind::kUbx: {
        const std::size_t plen = static_cast<std::size_t>(xorshift32(rng) % 64U);
        for (std::size_t i = 0U; i < plen; ++i) {
            payload[i] = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        }
        const auto msg_id = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        ok = demux_test::gen_ubx(dst, 0x01U, msg_id, std::span<const uint8_t>(payload.data(), plen), flen);
        break;
    }
    case ProtoKind::kUnicoreBin: {
        const std::size_t plen = static_cast<std::size_t>(xorshift32(rng) % 80U);
        for (std::size_t i = 0U; i < plen; ++i) {
            payload[i] = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        }
        const auto msg_id = static_cast<uint16_t>(xorshift32(rng) & 0xFFFFU); // 掩码后 0..65535
        ok = demux_test::gen_unicore_bin(dst, msg_id, 42U, {}, std::span<const uint8_t>(payload.data(), plen), flen);
        break;
    }
    case ProtoKind::kUnicoreHash: {
        const uint32_t seq = xorshift32(rng) % 100000U;
        const bool lc = (xorshift32(rng) & 1U) != 0U;
        const bool use_xor = (xorshift32(rng) & 2U) != 0U;
        const int n =
            std::snprintf(text.data(), text.size(), "BESTPOSA,97,GPS;%u,SOL_COMPUTED", static_cast<unsigned>(seq));
        TEST_ASSERT_TRUE(n > 0);
        // n > 0 且受 text.size() 限制
        const std::string_view sv(text.data(), static_cast<std::size_t>(n));
        if (use_xor) {
            ok = demux_test::gen_unicore_hash_xor(dst, sv, lc, flen);
        } else {
            ok = demux_test::gen_unicore_hash_crc32(dst, sv, lc, flen);
        }
        break;
    }
    case ProtoKind::kProduct: {
        const std::size_t plen = static_cast<std::size_t>(xorshift32(rng) % 64U);
        for (std::size_t i = 0U; i < plen; ++i) {
            payload[i] = static_cast<uint8_t>(xorshift32(rng) & 0xFFU); // 掩码后 0..255
        }
        const auto msg_id = static_cast<uint16_t>(xorshift32(rng) & 0xFFFFU); // 掩码后 0..65535
        ok = demux_test::gen_product(dst, msg_id, rtk::proto::sender::kApp, rtk::proto::msg_type::kSet,
                                     std::span<const uint8_t>(payload.data(), plen), flen);
        break;
    }
    default:
        return 0U;
    }

    TEST_ASSERT_TRUE(ok);
    if (truth != nullptr) {
        TEST_ASSERT_TRUE(truth->frame_count < kMaxFramesRec);
        TEST_ASSERT_TRUE((truth->frame_blob_used + flen) <= kBlobCap);
        FrameMeta &m = truth->meta[truth->frame_count];
        m.decoder = decoder_ptr;
        m.decoder_index = dec_idx;
        m.offset = truth->frame_blob_used;
        m.len = flen;
        std::copy(dst.begin(), dst.begin() + static_cast<std::ptrdiff_t>(flen),
                  truth->frame_blob.begin() + static_cast<std::ptrdiff_t>(truth->frame_blob_used));
        truth->frame_blob_used += flen;
        truth->frame_count++;
    }
    return flen;
}

std::size_t build_all_proto_mixed_stream(uint32_t seed, std::span<uint8_t> buf, std::size_t frame_count,
                                         StreamCapture *truth)
{
    uint32_t rng = seed;
    std::size_t pos = 0U;

    if (truth != nullptr) {
        truth->clear();
    }
    for (std::size_t i = 0U; i < frame_count; ++i) {
        const std::size_t idx = static_cast<std::size_t>(xorshift32(rng) % kActiveProtoCount);
        const ActiveProtoEntry &entry = kActiveProtos[idx];
        pos += append_fake_garbage(buf.subspan(pos), rng);
        // idx < kActiveProtoCount <= 6，在 uint8_t 范围内
        pos += append_random_proto_frame(entry.kind, entry.decoder, static_cast<uint8_t>(idx), buf.subspan(pos), rng,
                                         truth);
    }
    pos += append_fake_garbage(buf.subspan(pos), rng);
    return pos;
}

std::array<const rtk::demux::Decoder *, kActiveProtoCount> get_active_decoders() noexcept
{
    std::array<const rtk::demux::Decoder *, kActiveProtoCount> out{};
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

static void test_property_i1_chunk_independence_1000_splits()
{
    const auto decs = get_active_decoders();
    uint32_t rng = kBasePrngSeed;
    std::array<char, kFailMsgCap> msg{};
    const std::span<uint8_t> stream(s_stream_buf);

    const std::size_t stream_len = build_all_proto_mixed_stream(kBasePrngSeed, stream, 36U, &s_truth_cap);
    TEST_ASSERT_TRUE(stream_len > 1000U);

    // 参考运行 0：整块喂入
    run_chunked(decs, stream.subspan(0, stream_len), s_ref_cap, 0U, nullptr, "whole-buffer");
    TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_ref_cap.frame_count);

    // 参考运行 1：逐字节喂入
    run_chunked(decs, stream.subspan(0, stream_len), s_cur_cap, 1U, nullptr, "1-byte-chunk");
    assert_captures_equal(s_ref_cap, s_cur_cap, "1-byte-vs-whole");

    // >= 1000 轮随机切分
    for (uint32_t round = 0U; round < kSplitRounds; ++round) {
        const uint32_t round_seed = rng;
        static_cast<void>(std::snprintf(msg.data(), msg.size(),
                                        "I1 split failed: base_seed=0x%08lX round=%lu round_rng=0x%08lX",
                                        static_cast<unsigned long>(kBasePrngSeed), static_cast<unsigned long>(round),
                                        static_cast<unsigned long>(round_seed)));
        run_chunked(decs, stream.subspan(0, stream_len), s_cur_cap, 0U, &rng, msg.data());
        assert_captures_equal(s_ref_cap, s_cur_cap, msg.data());
    }
}

static void test_property_mixed_stream_false_preambles_recovery()
{
    const auto decs = get_active_decoders();
    uint32_t master_rng = 0xA1B2C3D4U;
    std::array<char, kFailMsgCap> msg{};
    const std::span<uint8_t> stream(s_stream_buf);

    for (uint32_t iter = 0U; iter < kMixedSeedRounds; ++iter) {
        const uint32_t stream_seed = xorshift32(master_rng);
        uint32_t split_rng = stream_seed ^ 0x9E3779B9U;
        const std::size_t stream_len = build_all_proto_mixed_stream(stream_seed, stream, 28U, &s_truth_cap);

        static_cast<void>(std::snprintf(msg.data(), msg.size(), "mixed recovery failed: iter=%lu stream_seed=0x%08lX",
                                        static_cast<unsigned long>(iter), static_cast<unsigned long>(stream_seed)));
        run_chunked(decs, stream.subspan(0, stream_len), s_cur_cap, 0U, &split_rng, msg.data());

        // abort 后所有真帧必须按序恢复且逐字节相同
        TEST_ASSERT_EQUAL_UINT_MESSAGE(s_truth_cap.frame_count, s_cur_cap.frame_count, msg.data());
        for (std::size_t i = 0U; i < s_truth_cap.frame_count; ++i) {
            TEST_ASSERT_EQUAL_PTR_MESSAGE(s_truth_cap.meta[i].decoder, s_cur_cap.meta[i].decoder, msg.data());
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_truth_cap.meta[i].decoder_index, s_cur_cap.meta[i].decoder_index,
                                            msg.data());
            TEST_ASSERT_EQUAL_UINT_MESSAGE(s_truth_cap.meta[i].len, s_cur_cap.meta[i].len, msg.data());
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&s_truth_cap.frame_blob[s_truth_cap.meta[i].offset],
                                                 &s_cur_cap.frame_blob[s_cur_cap.meta[i].offset],
                                                 s_truth_cap.meta[i].len, msg.data());
        }
    }
}

#if STREAM_DEMUX_RTCM3
static void test_scenario_bluetooth_product_and_rtcm3()
{
    static constexpr std::array<const rtk::demux::Decoder *, 2> kBtDecs{
        &rtk::proto::demux_frame::kDecoder,
        &rtk::demux::rtcm3::kDecoder,
    };
    using BtStaticDemux = rtk::demux::StaticDemux<rtk::proto::demux_frame::kDecoder, rtk::demux::rtcm3::kDecoder>;
    static_assert(BtStaticDemux::kBufSize == rtk::demux::rtcm3::kMaxFrame);
    static_assert(BtStaticDemux::index_of<rtk::proto::demux_frame::kDecoder>() == 0U);
    static_assert(BtStaticDemux::index_of<rtk::demux::rtcm3::kDecoder>() == 1U);

    uint32_t rng = 0x20261111U;
    std::size_t pos = 0U;
    const std::span<uint8_t> stream(s_stream_buf);

    s_truth_cap.clear();
    for (std::size_t i = 0U; i < 30U; ++i) {
        const bool is_rtcm = (i % 2U) != 0U;
        if ((i % 3U) == 0U) {
            pos += append_fake_garbage(stream.subspan(pos), rng);
        }
        pos += append_random_proto_frame(is_rtcm ? ProtoKind::kRtcm3 : ProtoKind::kProduct,
                                         is_rtcm ? &rtk::demux::rtcm3::kDecoder : &rtk::proto::demux_frame::kDecoder,
                                         is_rtcm ? BtStaticDemux::index_of<rtk::demux::rtcm3::kDecoder>()
                                                 : BtStaticDemux::index_of<rtk::proto::demux_frame::kDecoder>(),
                                         stream.subspan(pos), rng, &s_truth_cap);
    }

    run_chunked(kBtDecs, stream.subspan(0, pos), s_ref_cap, 0U, nullptr, "bt-whole");
    run_chunked(kBtDecs, stream.subspan(0, pos), s_cur_cap, 0U, &rng, "bt-chunked");
    assert_captures_equal(s_ref_cap, s_cur_cap, "bt-i1");
    TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_cur_cap.frame_count);

    // StaticDemux 同流端到端核对与 demux_frame::view 解码
    BtStaticDemux bt_sd;
    CaptureSink sink(s_cur_cap);
    s_cur_cap.clear();
    TEST_ASSERT_EQUAL(rtk::demux::Error::kOk, bt_sd.init(sink));
    TEST_ASSERT_EQUAL(rtk::demux::Error::kOk, bt_sd.feed(stream.subspan(0, pos)));
    TEST_ASSERT_EQUAL(rtk::demux::Error::kOk, bt_sd.abort());
    assert_captures_equal(s_ref_cap, s_cur_cap, "bt-static-demux");

    for (std::size_t i = 0U; i < s_cur_cap.frame_count; ++i) {
        if (s_cur_cap.meta[i].decoder == &rtk::proto::demux_frame::kDecoder) {
            const std::span<const uint8_t> wire(&s_cur_cap.frame_blob[s_cur_cap.meta[i].offset], s_cur_cap.meta[i].len);
            const std::optional<rtk::proto::FrameView> fv = rtk::proto::demux_frame::view(wire);
            TEST_ASSERT_TRUE(fv.has_value());
            TEST_ASSERT_EQUAL_UINT(rtk::proto::kHeaderLen + fv->payload.size() + rtk::proto::kCrcLen,
                                   s_cur_cap.meta[i].len);
        }
    }
}
#endif

static void test_scenario_gnss_unicore_and_ublox_sets()
{
    const std::span<uint8_t> stream(s_stream_buf);

#if STREAM_DEMUX_NMEA && STREAM_DEMUX_RTCM3 && STREAM_DEMUX_UNICORE_BIN && STREAM_DEMUX_UNICORE_HASH
    // GNSS 场景 A（Unicore UM980）：{nmea, rtcm3, unicore_bin, unicore_hash}
    {
        static constexpr std::array<const rtk::demux::Decoder *, 4> kUm980Decs{
            &rtk::demux::nmea::kDecoder,
            &rtk::demux::rtcm3::kDecoder,
            &rtk::demux::unicore_bin::kDecoder,
            &rtk::demux::unicore_hash::kDecoder,
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
            pos += append_fake_garbage(stream.subspan(pos), rng);
            // slot < 4，在 uint8_t 范围内
            pos += append_random_proto_frame(kPicks[slot], kUm980Decs[slot], static_cast<uint8_t>(slot),
                                             stream.subspan(pos), rng, &s_truth_cap);
        }
        run_chunked(kUm980Decs, stream.subspan(0, pos), s_ref_cap, 0U, nullptr, "gnss-um980-whole");
        run_chunked(kUm980Decs, stream.subspan(0, pos), s_cur_cap, 0U, &rng, "gnss-um980");
        assert_captures_equal(s_ref_cap, s_cur_cap, "gnss-um980-i1");
        assert_frames_match_truth(s_truth_cap, s_cur_cap, "gnss-um980-truth");
    }
#endif

#if STREAM_DEMUX_NMEA && STREAM_DEMUX_RTCM3 && STREAM_DEMUX_UBX
    // GNSS 场景 B（u-blox F9P）：{nmea, rtcm3, ubx}
    {
        static constexpr std::array<const rtk::demux::Decoder *, 3> kF9pDecs{
            &rtk::demux::nmea::kDecoder,
            &rtk::demux::rtcm3::kDecoder,
            &rtk::demux::ubx::kDecoder,
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
            pos += append_fake_garbage(stream.subspan(pos), rng);
            // slot < 3，在 uint8_t 范围内
            pos += append_random_proto_frame(kPicks[slot], kF9pDecs[slot], static_cast<uint8_t>(slot),
                                             stream.subspan(pos), rng, &s_truth_cap);
        }
        run_chunked(kF9pDecs, stream.subspan(0, pos), s_ref_cap, 0U, nullptr, "gnss-f9p-whole");
        run_chunked(kF9pDecs, stream.subspan(0, pos), s_cur_cap, 0U, &rng, "gnss-f9p");
        assert_captures_equal(s_ref_cap, s_cur_cap, "gnss-f9p-i1");
        assert_frames_match_truth(s_truth_cap, s_cur_cap, "gnss-f9p-truth");
    }
#endif
}

#if STREAM_DEMUX_UNICORE_BIN
static void test_scenario_product_vs_unicore_bin_conflict()
{
    static constexpr std::array<const rtk::demux::Decoder *, 2> kOrderAb{
        &rtk::proto::demux_frame::kDecoder,
        &rtk::demux::unicore_bin::kDecoder,
    };
    static constexpr std::array<const rtk::demux::Decoder *, 2> kOrderBa{
        &rtk::demux::unicore_bin::kDecoder,
        &rtk::proto::demux_frame::kDecoder,
    };
    uint32_t rng = 0xAA4418B5U;
    std::size_t pos = 0U;
    const std::span<uint8_t> stream(s_stream_buf);

    s_truth_cap.clear();
    for (std::size_t i = 0U; i < 16U; ++i) {
        const bool is_unicore = (i % 2U) != 0U;
        pos += append_random_proto_frame(is_unicore ? ProtoKind::kUnicoreBin : ProtoKind::kProduct,
                                         is_unicore ? &rtk::demux::unicore_bin::kDecoder
                                                    : &rtk::proto::demux_frame::kDecoder,
                                         is_unicore ? 1U : 0U, stream.subspan(pos), rng, &s_truth_cap);
    }

    // 无垃圾交错：即使回放到次优先级解码器，resyncs == 0，unknown_bytes == 0
    run_chunked(kOrderAb, stream.subspan(0, pos), s_cur_cap, 7U, nullptr, "aa-conflict-ab");
    TEST_ASSERT_EQUAL_UINT(16U, s_cur_cap.frame_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknown_bytes);

    // 对调优先级：{unicore_bin, product}
    run_chunked(kOrderBa, stream.subspan(0, pos), s_cur_cap, 5U, nullptr, "aa-conflict-ba");
    TEST_ASSERT_EQUAL_UINT(16U, s_cur_cap.frame_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknown_bytes);
    for (std::size_t i = 0U; i < s_cur_cap.frame_count; ++i) {
        const bool is_unicore = (i % 2U) != 0U;
        const rtk::demux::Decoder *exp_dec =
            is_unicore ? &rtk::demux::unicore_bin::kDecoder : &rtk::proto::demux_frame::kDecoder;
        const uint8_t exp_idx = is_unicore ? 0U : 1U;
        TEST_ASSERT_EQUAL_PTR(exp_dec, s_cur_cap.meta[i].decoder);
        TEST_ASSERT_EQUAL_UINT8(exp_idx, s_cur_cap.meta[i].decoder_index);
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
        pos += append_random_proto_frame(ProtoKind::kProduct, &rtk::proto::demux_frame::kDecoder, 0U,
                                         stream.subspan(pos), rng, &s_truth_cap);
        std::copy(kFakeAa2.begin(), kFakeAa2.end(), stream.subspan(pos).begin());
        pos += kFakeAa2.size();
        pos += append_random_proto_frame(ProtoKind::kUnicoreBin, &rtk::demux::unicore_bin::kDecoder, 1U,
                                         stream.subspan(pos), rng, &s_truth_cap);
        std::copy(kFakeAa3.begin(), kFakeAa3.end(), stream.subspan(pos).begin());
        pos += kFakeAa3.size();
        TEST_ASSERT_TRUE(demux_test::gen_product(stream.subspan(pos), 0x0042U, rtk::proto::sender::kApp,
                                                 rtk::proto::msg_type::kSet, {}, flen));
        s_truth_cap.meta[s_truth_cap.frame_count].decoder = &rtk::proto::demux_frame::kDecoder;
        s_truth_cap.meta[s_truth_cap.frame_count].decoder_index = 0U;
        s_truth_cap.meta[s_truth_cap.frame_count].offset = s_truth_cap.frame_blob_used;
        s_truth_cap.meta[s_truth_cap.frame_count].len = flen;
        std::copy(stream.subspan(pos, flen).begin(), stream.subspan(pos, flen).end(),
                  s_truth_cap.frame_blob.begin() + static_cast<std::ptrdiff_t>(s_truth_cap.frame_blob_used));
        s_truth_cap.frame_blob_used += flen;
        s_truth_cap.frame_count++;
        pos += flen;

        run_chunked(kOrderAb, stream.subspan(0, pos), s_cur_cap, 3U, nullptr, "aa-conflict-fake-preambles");
        TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_cur_cap.frame_count);
        for (std::size_t i = 0U; i < s_truth_cap.frame_count; ++i) {
            TEST_ASSERT_EQUAL_PTR(s_truth_cap.meta[i].decoder, s_cur_cap.meta[i].decoder);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_truth_cap.frame_blob[s_truth_cap.meta[i].offset],
                                         &s_cur_cap.frame_blob[s_cur_cap.meta[i].offset], s_truth_cap.meta[i].len);
        }
    }
}
#endif

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_property_i1_chunk_independence_1000_splits);
    RUN_TEST(test_property_mixed_stream_false_preambles_recovery);
#if STREAM_DEMUX_RTCM3
    RUN_TEST(test_scenario_bluetooth_product_and_rtcm3);
#endif
    RUN_TEST(test_scenario_gnss_unicore_and_ublox_sets);
#if STREAM_DEMUX_UNICORE_BIN
    RUN_TEST(test_scenario_product_vs_unicore_bin_conflict);
#endif
    return UNITY_END();
}
