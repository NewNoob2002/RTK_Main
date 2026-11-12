// 通用字节流分流器属性与场景流测试（ADR-0004 §3、§4、§8）。
// 覆盖：I1 分块无关（>=1000 轮随机切分）、I2 字节守恒、假前导混合流恢复、蓝牙/GNSS 场景与 AA 冲突。
// Generated-by: pi + antigravity/gemini-3.8-flash，人工负责人：UNKNOWN（待人工审查签字）
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "demux_gen.h"
#include "rtk_demux.h"
#include "rtk_demux_decoders.h"
#include "rtk_proto_demux.h"
#include "rtk_proto_frame.h"
#include "unity.h"

#define DEMUX_BUF_SIZE    (4096U)
#define STREAM_CAP        (16384U)
#define MAX_FRAMES_REC    (128U)
#define BLOB_CAP          (16384U)
#define SPLIT_ROUNDS      (1000U)
#define MIXED_SEED_ROUNDS (40U)
#define BASE_PRNG_SEED    (0x5EED1234U)
#define FAIL_MSG_CAP      (160U)

typedef struct {
    uint16_t proto_id;
    uint8_t decoder_index;
    size_t offset;
    size_t len;
} frame_meta_t;

typedef struct {
    frame_meta_t meta[MAX_FRAMES_REC];
    size_t frame_count;
    uint8_t frame_blob[BLOB_CAP];
    size_t frame_blob_used;
    uint8_t unknown_blob[STREAM_CAP];
    size_t unknown_len;
    bool overflowed;
} stream_capture_t;

static uint8_t s_demux_buf[DEMUX_BUF_SIZE];
static rtk_demux_t s_dmx;
static stream_capture_t s_ref_cap;
static stream_capture_t s_cur_cap;
static stream_capture_t s_truth_cap;
static uint8_t s_stream_buf[STREAM_CAP];

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;

    if (x == 0U) {
        x = 0x6D2B79F5U;
    }
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return x;
}

static void capture_on_frame(void *user, const rtk_demux_decoder_t *decoder, uint8_t decoder_index,
                             const uint8_t *frame, size_t len)
{
    stream_capture_t *cap = (stream_capture_t *)user;

    if ((cap->frame_count >= MAX_FRAMES_REC) || ((cap->frame_blob_used + len) > BLOB_CAP)) {
        cap->overflowed = true;
        return;
    }
    cap->meta[cap->frame_count].proto_id = (decoder != NULL) ? decoder->proto_id : 0U;
    cap->meta[cap->frame_count].decoder_index = decoder_index;
    cap->meta[cap->frame_count].offset = cap->frame_blob_used;
    cap->meta[cap->frame_count].len = len;
    if ((len > 0U) && (frame != NULL)) {
        memcpy(&cap->frame_blob[cap->frame_blob_used], frame, len);
    }
    cap->frame_blob_used += len;
    cap->frame_count++;
}

static void capture_on_unknown(void *user, const uint8_t *bytes, size_t len)
{
    stream_capture_t *cap = (stream_capture_t *)user;

    if ((cap->unknown_len + len) > STREAM_CAP) {
        cap->overflowed = true;
        return;
    }
    if ((len > 0U) && (bytes != NULL)) {
        memcpy(&cap->unknown_blob[cap->unknown_len], bytes, len);
    }
    cap->unknown_len += len;
}

static void run_chunked(const rtk_demux_decoder_t *const *decs, uint8_t dec_count, const uint8_t *data, size_t data_len,
                        stream_capture_t *cap, size_t fixed_chunk, uint32_t *rng_or_null, const char *ctx_msg)
{
    rtk_demux_config_t cfg = {
        .decoders = decs,
        .decoder_count = dec_count,
        .buf = s_demux_buf,
        .buf_size = sizeof(s_demux_buf),
        .on_frame = capture_on_frame,
        .on_unknown = capture_on_unknown,
        .user = cap,
    };
    size_t pos = 0U;
    const rtk_demux_stats_t *st = NULL;
    size_t total_frame_bytes = 0U;

    memset(cap, 0, sizeof(*cap));
    TEST_ASSERT_EQUAL_MESSAGE(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg), ctx_msg);

    while (pos < data_len) {
        size_t remain = data_len - pos;
        size_t chunk = remain;

        if (rng_or_null != NULL) {
            uint32_t r = xorshift32(rng_or_null);
            uint32_t mode = r % 10U;
            if (mode == 0U) {
                chunk = 0U; /* 故意穿插 0 字节喂入 */
            } else if (mode <= 3U) {
                chunk = 1U;
            } else if (mode <= 6U) {
                chunk = (size_t)((r >> 8U) % 19U) + 1U;
            } else {
                chunk = (size_t)((r >> 8U) % 257U) + 1U;
            }
            if (chunk > remain) {
                chunk = remain;
            }
        } else if ((fixed_chunk > 0U) && (chunk > fixed_chunk)) {
            chunk = fixed_chunk;
        } else {
            /* 整块喂入 */
        }

        TEST_ASSERT_EQUAL_MESSAGE(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, &data[pos], chunk), ctx_msg);
        pos += chunk;
    }

    TEST_ASSERT_EQUAL_MESSAGE(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx), ctx_msg);
    TEST_ASSERT_FALSE_MESSAGE(rtk_demux_busy(&s_dmx), ctx_msg);
    TEST_ASSERT_FALSE_MESSAGE(cap->overflowed, ctx_msg);

    /* I2 字节守恒：帧总字节数 + 未知字节数 == 输入总字节数 */
    st = rtk_demux_stats(&s_dmx);
    for (uint8_t i = 0U; i < dec_count; i++) {
        total_frame_bytes += st->decoder[i].bytes;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(cap->frame_blob_used, total_frame_bytes, ctx_msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(cap->unknown_len, st->unknown_bytes, ctx_msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(data_len, cap->frame_blob_used + cap->unknown_len, ctx_msg);
}

static void assert_captures_equal(const stream_capture_t *exp, const stream_capture_t *got, const char *ctx_msg)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(exp->frame_count, got->frame_count, ctx_msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(exp->frame_blob_used, got->frame_blob_used, ctx_msg);
    for (size_t i = 0U; i < exp->frame_count; i++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(exp->meta[i].proto_id, got->meta[i].proto_id, ctx_msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(exp->meta[i].decoder_index, got->meta[i].decoder_index, ctx_msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(exp->meta[i].len, got->meta[i].len, ctx_msg);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&exp->frame_blob[exp->meta[i].offset],
                                             &got->frame_blob[got->meta[i].offset], exp->meta[i].len, ctx_msg);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(exp->unknown_len, got->unknown_len, ctx_msg);
    if (exp->unknown_len > 0U) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(exp->unknown_blob, got->unknown_blob, exp->unknown_len, ctx_msg);
    }
}

/* 向流追加假前导与随机垃圾（避开 '\n'，防止意外拼出合法 NMEA/Hash 行尾） */
static size_t append_fake_garbage(uint8_t *dst, size_t cap, uint32_t *rng)
{
    static const uint8_t s_f0[] = {0xD3U, 0x00U, 0x10U, 0x01U, 0x02U};
    static const uint8_t s_f1[] = {0xD3U, 0xFCU, 0x00U};
    static const uint8_t s_f2[] = {0xAAU, 0x44U};
    static const uint8_t s_f3[] = {0xAAU, 0x44U, 0x18U, 0x14U, 0x01U, 0x00U, 0x00U};
    static const uint8_t s_f4[] = {0xAAU, 0x44U, 0xB5U, 0x20U, 0x05U, 0x00U, 0x08U, 0x00U};
    static const uint8_t s_f5[] = {(uint8_t)'$', (uint8_t)'G', (uint8_t)'P', (uint8_t)'G',
                                   (uint8_t)'G', (uint8_t)'A', (uint8_t)','};
    static const uint8_t s_f6[] = {(uint8_t)'$', (uint8_t)'@', (uint8_t)'S', (uint8_t)'B', (uint8_t)'F'};
    static const uint8_t s_f7[] = {(uint8_t)'#', (uint8_t)'V', (uint8_t)'E', (uint8_t)'R', (uint8_t)','};
    static const uint8_t s_f8[] = {(uint8_t)'#', (uint8_t)'0', (uint8_t)'B', (uint8_t)'A', (uint8_t)'D'};
    static const uint8_t s_f9[] = {0xB5U, 0x62U, 0x01U, 0x07U, 0x20U, 0x00U, 0x11U, 0x22U};
    static const uint8_t s_f10[] = {0xB5U, 0x61U};
    static const uint8_t s_f11[] = {0xAAU};
    static const uint8_t s_f12[] = {0xD3U};
    static const uint8_t *const s_fakes[] = {
        s_f0, s_f1, s_f2, s_f3, s_f4, s_f5, s_f6, s_f7, s_f8, s_f9, s_f10, s_f11, s_f12,
    };
    static const size_t s_fake_lens[] = {
        sizeof(s_f0), sizeof(s_f1), sizeof(s_f2), sizeof(s_f3),  sizeof(s_f4),  sizeof(s_f5),  sizeof(s_f6),
        sizeof(s_f7), sizeof(s_f8), sizeof(s_f9), sizeof(s_f10), sizeof(s_f11), sizeof(s_f12),
    };
    size_t pos = 0U;
    size_t rand_len = (size_t)(xorshift32(rng) % 7U);
    size_t fake_idx = (size_t)(xorshift32(rng) % (sizeof(s_fakes) / sizeof(s_fakes[0])));

    for (size_t i = 0U; (i < rand_len) && (pos < cap); i++) {
        uint8_t b = (uint8_t)(xorshift32(rng) & 0xFFU);
        if ((b == (uint8_t)'\n') || (b == 0x62U)) {
            b = 0x5AU; /* 避免随机字节偶发凑成 UBX 0xB5 0x62 或行结束符 */
        }
        dst[pos++] = b;
    }
    if ((pos + s_fake_lens[fake_idx]) <= cap) {
        memcpy(&dst[pos], s_fakes[fake_idx], s_fake_lens[fake_idx]);
        pos += s_fake_lens[fake_idx];
    }
    return pos;
}

/* 按协议类型编号生成随机真帧并记录到 truth_cap */
static size_t append_random_proto_frame(uint8_t proto_pick, uint8_t dec_idx, uint8_t *dst, size_t cap, uint32_t *rng,
                                        stream_capture_t *truth)
{
    uint8_t payload[260];
    char text[96];
    size_t flen = 0U;
    uint16_t pid = 0U;
    bool ok = false;

    switch (proto_pick) {
    case 0U: {
        size_t plen = (size_t)(xorshift32(rng) % 120U);
        for (size_t i = 0U; i < plen; i++) {
            payload[i] = (uint8_t)(xorshift32(rng) & 0xFFU);
        }
        ok = demux_gen_rtcm3(dst, cap, payload, plen, &flen);
        pid = RTK_DEMUX_PROTO_RTCM3;
        break;
    }
    case 1U: {
        uint32_t seq = xorshift32(rng) % 10000U;
        bool lc = (xorshift32(rng) & 1U) != 0U;
        (void)snprintf(text, sizeof(text), "GNGGA,1235%02u,4807.038,N,01131.000,E,1,08,%u", (unsigned)(seq % 60U),
                       (unsigned)seq);
        ok = demux_gen_nmea(dst, cap, text, lc, &flen);
        pid = RTK_DEMUX_PROTO_NMEA;
        break;
    }
    case 2U: {
        size_t plen = (size_t)(xorshift32(rng) % 64U);
        for (size_t i = 0U; i < plen; i++) {
            payload[i] = (uint8_t)(xorshift32(rng) & 0xFFU);
        }
        ok = demux_gen_ubx(dst, cap, 0x01U, (uint8_t)(xorshift32(rng) & 0xFFU), payload, plen, &flen);
        pid = RTK_DEMUX_PROTO_UBX;
        break;
    }
    case 3U: {
        size_t plen = (size_t)(xorshift32(rng) % 80U);
        for (size_t i = 0U; i < plen; i++) {
            payload[i] = (uint8_t)(xorshift32(rng) & 0xFFU);
        }
        ok = demux_gen_unicore_bin(dst, cap, (uint16_t)(xorshift32(rng) & 0xFFFFU), 42U, NULL, payload, plen, &flen);
        pid = RTK_DEMUX_PROTO_UNICORE_BIN;
        break;
    }
    case 4U: {
        uint32_t seq = xorshift32(rng) % 100000U;
        bool lc = (xorshift32(rng) & 1U) != 0U;
        bool use_xor = (xorshift32(rng) & 2U) != 0U;
        (void)snprintf(text, sizeof(text), "BESTPOSA,97,GPS;%u,SOL_COMPUTED", (unsigned)seq);
        if (use_xor) {
            ok = demux_gen_unicore_hash_xor(dst, cap, text, lc, &flen);
        } else {
            ok = demux_gen_unicore_hash_crc32(dst, cap, text, lc, &flen);
        }
        pid = RTK_DEMUX_PROTO_UNICORE_HASH;
        break;
    }
    case 5U: {
        uint16_t plen = (uint16_t)(xorshift32(rng) % 64U);
        for (size_t i = 0U; i < plen; i++) {
            payload[i] = (uint8_t)(xorshift32(rng) & 0xFFU);
        }
        ok = demux_gen_product(dst, cap, (uint16_t)(xorshift32(rng) & 0xFFFFU), RTK_PROTO_SENDER_APP,
                               RTK_PROTO_TYPE_SET, payload, plen, &flen);
        pid = RTK_PROTO_DEMUX_ID;
        break;
    }
    default:
        return 0U;
    }

    TEST_ASSERT_TRUE(ok);
    if (truth != NULL) {
        TEST_ASSERT_TRUE(truth->frame_count < MAX_FRAMES_REC);
        TEST_ASSERT_TRUE((truth->frame_blob_used + flen) <= BLOB_CAP);
        truth->meta[truth->frame_count].proto_id = pid;
        truth->meta[truth->frame_count].decoder_index = dec_idx;
        truth->meta[truth->frame_count].offset = truth->frame_blob_used;
        truth->meta[truth->frame_count].len = flen;
        memcpy(&truth->frame_blob[truth->frame_blob_used], dst, flen);
        truth->frame_blob_used += flen;
        truth->frame_count++;
    }
    return flen;
}

static size_t build_all_proto_mixed_stream(uint32_t seed, uint8_t *buf, size_t cap, size_t frame_count,
                                           stream_capture_t *truth)
{
    /* 按挂载表顺序：0=product, 1=unicore_bin, 2=rtcm3, 3=nmea, 4=unicore_hash, 5=ubx */
    static const uint8_t s_proto_to_dec[6] = {2U, 3U, 5U, 1U, 4U, 0U};
    uint32_t rng = seed;
    size_t pos = 0U;

    if (truth != NULL) {
        memset(truth, 0, sizeof(*truth));
    }
    for (size_t i = 0U; i < frame_count; i++) {
        uint8_t pick = (uint8_t)(xorshift32(&rng) % 6U);
        pos += append_fake_garbage(&buf[pos], cap - pos, &rng);
        pos += append_random_proto_frame(pick, s_proto_to_dec[pick], &buf[pos], cap - pos, &rng, truth);
    }
    pos += append_fake_garbage(&buf[pos], cap - pos, &rng);
    return pos;
}

void setUp(void) {}
void tearDown(void) {}

/* -------------------------------------------------------------------------- */
/* 测试用例                                                                     */
/* -------------------------------------------------------------------------- */

static void test_property_i1_chunk_independence_1000_splits(void)
{
    const rtk_demux_decoder_t *const decs[] = {
        &rtk_proto_demux_decoder, &rtk_demux_dec_unicore_bin,  &rtk_demux_dec_rtcm3,
        &rtk_demux_dec_nmea,      &rtk_demux_dec_unicore_hash, &rtk_demux_dec_ubx,
    };
    uint32_t rng = BASE_PRNG_SEED;
    size_t stream_len = 0U;
    char msg[FAIL_MSG_CAP];

    stream_len = build_all_proto_mixed_stream(BASE_PRNG_SEED, s_stream_buf, sizeof(s_stream_buf), 36U, &s_truth_cap);
    TEST_ASSERT_TRUE(stream_len > 1000U);

    /* 参考运行 0：整块喂入 */
    run_chunked(decs, 6U, s_stream_buf, stream_len, &s_ref_cap, 0U, NULL, "whole-buffer");
    TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_ref_cap.frame_count);

    /* 参考运行 1：逐字节喂入 */
    run_chunked(decs, 6U, s_stream_buf, stream_len, &s_cur_cap, 1U, NULL, "1-byte-chunk");
    assert_captures_equal(&s_ref_cap, &s_cur_cap, "1-byte-vs-whole");

    /* >= 1000 轮随机切分 */
    for (uint32_t round = 0U; round < SPLIT_ROUNDS; round++) {
        uint32_t round_seed = rng;
        (void)snprintf(msg, sizeof(msg), "I1 split failed: base_seed=0x%08X round=%u round_rng=0x%08X",
                       (unsigned)BASE_PRNG_SEED, (unsigned)round, (unsigned)round_seed);
        run_chunked(decs, 6U, s_stream_buf, stream_len, &s_cur_cap, 0U, &rng, msg);
        assert_captures_equal(&s_ref_cap, &s_cur_cap, msg);
    }
}

static void test_property_mixed_stream_false_preambles_recovery(void)
{
    const rtk_demux_decoder_t *const decs[] = {
        &rtk_proto_demux_decoder, &rtk_demux_dec_unicore_bin,  &rtk_demux_dec_rtcm3,
        &rtk_demux_dec_nmea,      &rtk_demux_dec_unicore_hash, &rtk_demux_dec_ubx,
    };
    uint32_t master_rng = 0xA1B2C3D4U;
    char msg[FAIL_MSG_CAP];

    for (uint32_t iter = 0U; iter < MIXED_SEED_ROUNDS; iter++) {
        uint32_t stream_seed = xorshift32(&master_rng);
        uint32_t split_rng = stream_seed ^ 0x9E3779B9U;
        size_t stream_len =
            build_all_proto_mixed_stream(stream_seed, s_stream_buf, sizeof(s_stream_buf), 28U, &s_truth_cap);

        (void)snprintf(msg, sizeof(msg), "mixed recovery failed: iter=%u stream_seed=0x%08X", (unsigned)iter,
                       (unsigned)stream_seed);
        run_chunked(decs, 6U, s_stream_buf, stream_len, &s_cur_cap, 0U, &split_rng, msg);

        /* abort 后所有真帧必须按序恢复且逐字节相同 */
        TEST_ASSERT_EQUAL_UINT_MESSAGE(s_truth_cap.frame_count, s_cur_cap.frame_count, msg);
        for (size_t i = 0U; i < s_truth_cap.frame_count; i++) {
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(s_truth_cap.meta[i].proto_id, s_cur_cap.meta[i].proto_id, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_truth_cap.meta[i].decoder_index, s_cur_cap.meta[i].decoder_index, msg);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(s_truth_cap.meta[i].len, s_cur_cap.meta[i].len, msg);
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(&s_truth_cap.frame_blob[s_truth_cap.meta[i].offset],
                                                 &s_cur_cap.frame_blob[s_cur_cap.meta[i].offset],
                                                 s_truth_cap.meta[i].len, msg);
        }
    }
}

static void test_scenario_bluetooth_product_and_rtcm3(void)
{
    const rtk_demux_decoder_t *const bt_decs[] = {&rtk_proto_demux_decoder, &rtk_demux_dec_rtcm3};
    uint32_t rng = 0x20261111U;
    size_t pos = 0U;

    memset(&s_truth_cap, 0, sizeof(s_truth_cap));
    for (size_t i = 0U; i < 30U; i++) {
        bool is_rtcm = (i % 2U) != 0U;
        if ((i % 3U) == 0U) {
            pos += append_fake_garbage(&s_stream_buf[pos], sizeof(s_stream_buf) - pos, &rng);
        }
        pos += append_random_proto_frame(is_rtcm ? 0U : 5U, is_rtcm ? 1U : 0U, &s_stream_buf[pos],
                                         sizeof(s_stream_buf) - pos, &rng, &s_truth_cap);
    }

    run_chunked(bt_decs, 2U, s_stream_buf, pos, &s_ref_cap, 0U, NULL, "bt-whole");
    run_chunked(bt_decs, 2U, s_stream_buf, pos, &s_cur_cap, 0U, &rng, "bt-chunked");
    assert_captures_equal(&s_ref_cap, &s_cur_cap, "bt-i1");
    TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_cur_cap.frame_count);

    /* 每条产品帧均可经 rtk_proto_frame_view 解出视图 */
    for (size_t i = 0U; i < s_cur_cap.frame_count; i++) {
        if (s_cur_cap.meta[i].proto_id == RTK_PROTO_DEMUX_ID) {
            rtk_proto_frame_t view;
            TEST_ASSERT_TRUE(
                rtk_proto_frame_view(&s_cur_cap.frame_blob[s_cur_cap.meta[i].offset], s_cur_cap.meta[i].len, &view));
            TEST_ASSERT_EQUAL_UINT(24U + (size_t)view.payload_len, s_cur_cap.meta[i].len);
        }
    }
}

static void test_scenario_gnss_unicore_and_ublox_sets(void)
{
    /* GNSS 场景 A（Unicore UM980）：{nmea, rtcm3, unicore_bin, unicore_hash} */
    {
        const rtk_demux_decoder_t *const um980_decs[] = {
            &rtk_demux_dec_nmea,
            &rtk_demux_dec_rtcm3,
            &rtk_demux_dec_unicore_bin,
            &rtk_demux_dec_unicore_hash,
        };
        static const uint8_t s_picks[] = {1U, 0U, 3U, 4U}; /* nmea, rtcm3, unicore_bin, unicore_hash */
        uint32_t rng = 0x98009800U;
        size_t pos = 0U;

        memset(&s_truth_cap, 0, sizeof(s_truth_cap));
        for (size_t i = 0U; i < 24U; i++) {
            uint8_t slot = (uint8_t)(i % 4U);
            pos += append_fake_garbage(&s_stream_buf[pos], sizeof(s_stream_buf) - pos, &rng);
            pos += append_random_proto_frame(s_picks[slot], slot, &s_stream_buf[pos], sizeof(s_stream_buf) - pos, &rng,
                                             &s_truth_cap);
        }
        run_chunked(um980_decs, 4U, s_stream_buf, pos, &s_cur_cap, 0U, &rng, "gnss-um980");
        TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_cur_cap.frame_count);
    }

    /* GNSS 场景 B（u-blox F9P）：{nmea, rtcm3, ubx} */
    {
        const rtk_demux_decoder_t *const f9p_decs[] = {
            &rtk_demux_dec_nmea,
            &rtk_demux_dec_rtcm3,
            &rtk_demux_dec_ubx,
        };
        static const uint8_t s_picks[] = {1U, 0U, 2U}; /* nmea, rtcm3, ubx */
        uint32_t rng = 0xF9BF9B01U;
        size_t pos = 0U;

        memset(&s_truth_cap, 0, sizeof(s_truth_cap));
        for (size_t i = 0U; i < 24U; i++) {
            uint8_t slot = (uint8_t)(i % 3U);
            pos += append_fake_garbage(&s_stream_buf[pos], sizeof(s_stream_buf) - pos, &rng);
            pos += append_random_proto_frame(s_picks[slot], slot, &s_stream_buf[pos], sizeof(s_stream_buf) - pos, &rng,
                                             &s_truth_cap);
        }
        run_chunked(f9p_decs, 3U, s_stream_buf, pos, &s_cur_cap, 0U, &rng, "gnss-f9p");
        TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_cur_cap.frame_count);
    }
}

static void test_scenario_product_vs_unicore_bin_conflict(void)
{
    const rtk_demux_decoder_t *const order_ab[] = {&rtk_proto_demux_decoder, &rtk_demux_dec_unicore_bin};
    const rtk_demux_decoder_t *const order_ba[] = {&rtk_demux_dec_unicore_bin, &rtk_proto_demux_decoder};
    uint32_t rng = 0xAA4418B5U;
    size_t pos = 0U;

    memset(&s_truth_cap, 0, sizeof(s_truth_cap));
    for (size_t i = 0U; i < 16U; i++) {
        bool is_unicore = (i % 2U) != 0U;
        pos += append_random_proto_frame(is_unicore ? 3U : 5U, is_unicore ? 1U : 0U, &s_stream_buf[pos],
                                         sizeof(s_stream_buf) - pos, &rng, &s_truth_cap);
    }

    /* 无垃圾交错：即使回放到次优先级解码器，resyncs 必须为 0，unknown_bytes 必须为 0 */
    run_chunked(order_ab, 2U, s_stream_buf, pos, &s_cur_cap, 7U, NULL, "aa-conflict-ab");
    TEST_ASSERT_EQUAL_UINT(16U, s_cur_cap.frame_count);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->unknown_bytes);

    /* 对调优先级：{unicore_bin, product} */
    run_chunked(order_ba, 2U, s_stream_buf, pos, &s_cur_cap, 5U, NULL, "aa-conflict-ba");
    TEST_ASSERT_EQUAL_UINT(16U, s_cur_cap.frame_count);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->unknown_bytes);
    for (size_t i = 0U; i < s_cur_cap.frame_count; i++) {
        uint16_t exp_pid =
            (uint16_t)(((i % 2U) != 0U) ? RTK_DEMUX_PROTO_UNICORE_BIN : RTK_PROTO_DEMUX_ID); /* 常量均在 u16 范围 */
        uint8_t exp_idx = (uint8_t)(((i % 2U) != 0U) ? 0U : 1U);                             /* 仅为 0 或 1 */
        TEST_ASSERT_EQUAL_UINT16(exp_pid, s_cur_cap.meta[i].proto_id);
        TEST_ASSERT_EQUAL_UINT8(exp_idx, s_cur_cap.meta[i].decoder_index);
    }

    /* 假 AA 44 前导紧跟真产品帧/Unicore 帧 + 视图校验 */
    {
        static const uint8_t s_fake_aa_1[] = {0xAAU, 0x44U, 0x99U};
        static const uint8_t s_fake_aa_2[] = {0xAAU, 0x44U};
        static const uint8_t s_fake_aa_3[] = {0xAAU};
        size_t flen = 0U;

        pos = 0U;
        memset(&s_truth_cap, 0, sizeof(s_truth_cap));
        memcpy(&s_stream_buf[pos], s_fake_aa_1, sizeof(s_fake_aa_1));
        pos += sizeof(s_fake_aa_1);
        pos += append_random_proto_frame(5U, 0U, &s_stream_buf[pos], sizeof(s_stream_buf) - pos, &rng, &s_truth_cap);
        memcpy(&s_stream_buf[pos], s_fake_aa_2, sizeof(s_fake_aa_2));
        pos += sizeof(s_fake_aa_2);
        pos += append_random_proto_frame(3U, 1U, &s_stream_buf[pos], sizeof(s_stream_buf) - pos, &rng, &s_truth_cap);
        memcpy(&s_stream_buf[pos], s_fake_aa_3, sizeof(s_fake_aa_3));
        pos += sizeof(s_fake_aa_3);
        TEST_ASSERT_TRUE(demux_gen_product(&s_stream_buf[pos], sizeof(s_stream_buf) - pos, 0x0042U,
                                           RTK_PROTO_SENDER_APP, RTK_PROTO_TYPE_SET, NULL, 0U, &flen));
        s_truth_cap.meta[s_truth_cap.frame_count].proto_id = RTK_PROTO_DEMUX_ID;
        s_truth_cap.meta[s_truth_cap.frame_count].decoder_index = 0U;
        s_truth_cap.meta[s_truth_cap.frame_count].offset = s_truth_cap.frame_blob_used;
        s_truth_cap.meta[s_truth_cap.frame_count].len = flen;
        memcpy(&s_truth_cap.frame_blob[s_truth_cap.frame_blob_used], &s_stream_buf[pos], flen);
        s_truth_cap.frame_blob_used += flen;
        s_truth_cap.frame_count++;
        pos += flen;

        run_chunked(order_ab, 2U, s_stream_buf, pos, &s_cur_cap, 3U, NULL, "aa-conflict-fake-preambles");
        TEST_ASSERT_EQUAL_UINT(s_truth_cap.frame_count, s_cur_cap.frame_count);
        for (size_t i = 0U; i < s_truth_cap.frame_count; i++) {
            TEST_ASSERT_EQUAL_UINT16(s_truth_cap.meta[i].proto_id, s_cur_cap.meta[i].proto_id);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_truth_cap.frame_blob[s_truth_cap.meta[i].offset],
                                         &s_cur_cap.frame_blob[s_cur_cap.meta[i].offset], s_truth_cap.meta[i].len);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_property_i1_chunk_independence_1000_splits);
    RUN_TEST(test_property_mixed_stream_false_preambles_recovery);
    RUN_TEST(test_scenario_bluetooth_product_and_rtcm3);
    RUN_TEST(test_scenario_gnss_unicore_and_ublox_sets);
    RUN_TEST(test_scenario_product_vs_unicore_bin_conflict);
    return UNITY_END();
}
