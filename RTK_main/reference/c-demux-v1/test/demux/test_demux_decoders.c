// 内置解码器黑盒测试（ADR-0004 §4，覆盖 5 个通用 GNSS 解码器 + 产品帧解码器与视图）。
// Generated-by: pi + antigravity/gemini-3.8-flash，人工负责人：UNKNOWN（待人工审查签字）
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "demux_gen.h"
#include "rtk_demux.h"
#include "rtk_demux_decoders.h"
#include "rtk_proto_demux.h"
#include "rtk_proto_frame.h"
#include "unity.h"

#define DEMUX_BUF_SIZE (4096U)
#define MAX_DEC_FRAMES (16U)
#define MAX_UNK_CAP    (4096U)

typedef struct {
    uint16_t proto_id;
    uint8_t decoder_index;
    size_t len;
    uint8_t data[DEMUX_BUF_SIZE];
} dec_frame_t;

typedef struct {
    dec_frame_t frames[MAX_DEC_FRAMES];
    size_t frame_count;
    uint8_t unknown[MAX_UNK_CAP];
    size_t unknown_len;
} dec_sink_t;

static uint8_t s_demux_buf[DEMUX_BUF_SIZE];
static uint8_t s_work_buf[4200];
static rtk_demux_t s_dmx;
static dec_sink_t s_sink;

static void sink_on_frame(void *user, const rtk_demux_decoder_t *decoder, uint8_t decoder_index, const uint8_t *frame,
                          size_t len)
{
    dec_sink_t *sink = (dec_sink_t *)user;

    if (sink->frame_count < MAX_DEC_FRAMES) {
        dec_frame_t *dst = &sink->frames[sink->frame_count++];
        dst->proto_id = (decoder != NULL) ? decoder->proto_id : 0U;
        dst->decoder_index = decoder_index;
        dst->len = len;
        if ((len <= sizeof(dst->data)) && (frame != NULL)) {
            memcpy(dst->data, frame, len);
        }
    }
}

static void sink_on_unknown(void *user, const uint8_t *bytes, size_t len)
{
    dec_sink_t *sink = (dec_sink_t *)user;

    if ((sink->unknown_len + len <= sizeof(sink->unknown)) && (bytes != NULL)) {
        memcpy(&sink->unknown[sink->unknown_len], bytes, len);
    }
    sink->unknown_len += len;
}

static void run_demux_feed_and_abort(const rtk_demux_decoder_t *const *decs, uint8_t count, const uint8_t *data,
                                     size_t len)
{
    rtk_demux_config_t cfg = {
        .decoders = decs,
        .decoder_count = count,
        .buf = s_demux_buf,
        .buf_size = sizeof(s_demux_buf),
        .on_frame = sink_on_frame,
        .on_unknown = sink_on_unknown,
        .user = &s_sink,
    };

    memset(&s_sink, 0, sizeof(s_sink));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, data, len));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx));
}

/* 按解码器 *need 契约驱动 step 直至出现 FRAME / REJECT / 耗尽输入 */
static rtk_demux_step_t step_by_need(const rtk_demux_decoder_t *dec, const uint8_t *buf, size_t total_len,
                                     size_t *out_end_pos, size_t *out_calls)
{
    size_t pos = 1U;
    size_t calls = 0U;

    TEST_ASSERT_TRUE(total_len >= 1U);
    TEST_ASSERT_TRUE(dec->first_byte(buf[0]));

    for (;;) {
        size_t need = 0U;
        rtk_demux_step_t st = dec->step(buf, pos, &need);
        calls++;
        if (st != RTK_DEMUX_STEP_NEED_MORE) {
            if (out_end_pos != NULL) {
                *out_end_pos = pos;
            }
            if (out_calls != NULL) {
                *out_calls = calls;
            }
            return st;
        }
        TEST_ASSERT_TRUE(need >= 1U);
        TEST_ASSERT_TRUE(need <= (size_t)dec->max_frame);
        TEST_ASSERT_TRUE((pos + need) <= (size_t)dec->max_frame);
        if ((pos + need) > total_len) {
            if (out_end_pos != NULL) {
                *out_end_pos = pos;
            }
            if (out_calls != NULL) {
                *out_calls = calls;
            }
            return RTK_DEMUX_STEP_NEED_MORE;
        }
        pos += need;
    }
}

static void assert_only_first_byte(const rtk_demux_decoder_t *dec, uint8_t expected)
{
    for (uint32_t b = 0U; b <= 0xFFU; b++) {
        bool got = dec->first_byte((uint8_t)b); /* b <= 255 */
        if (b == (uint32_t)expected) {
            TEST_ASSERT_TRUE(got);
        } else {
            TEST_ASSERT_FALSE(got);
        }
    }
}

/* 自定义 '$' 同首字节解码器（模拟 SBF $@ 6 字节短帧），用于测试 NMEA 与同首字节 '$' 冲突 */
static bool custom_sbf_first(uint8_t b)
{
    return b == (uint8_t)'$';
}

static rtk_demux_step_t custom_sbf_step(const uint8_t *frame, size_t len, size_t *need)
{
    if (len < 2U) {
        *need = 1U;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[1] != (uint8_t)'@') {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < 6U) {
        *need = 6U - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return RTK_DEMUX_STEP_FRAME;
}

static const rtk_demux_decoder_t s_dec_custom_sbf = {
    .name = "custom_sbf",
    .proto_id = RTK_DEMUX_PROTO_USER + 1U,
    .max_frame = 6U,
    .first_byte = custom_sbf_first,
    .step = custom_sbf_step,
};

void setUp(void) {}
void tearDown(void) {}

/* -------------------------------------------------------------------------- */
/* 1. 描述符元数据与首字节全覆盖                                                */
/* -------------------------------------------------------------------------- */

static void test_decoder_descriptors_and_first_bytes(void)
{
    TEST_ASSERT_NOT_NULL(rtk_demux_dec_rtcm3.name);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_RTCM3, rtk_demux_dec_rtcm3.proto_id);
    TEST_ASSERT_EQUAL_UINT16(1029U, rtk_demux_dec_rtcm3.max_frame);
    assert_only_first_byte(&rtk_demux_dec_rtcm3, 0xD3U);

    TEST_ASSERT_NOT_NULL(rtk_demux_dec_nmea.name);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_NMEA, rtk_demux_dec_nmea.proto_id);
    TEST_ASSERT_EQUAL_UINT16(256U, rtk_demux_dec_nmea.max_frame);
    assert_only_first_byte(&rtk_demux_dec_nmea, (uint8_t)'$');

    TEST_ASSERT_NOT_NULL(rtk_demux_dec_ubx.name);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_UBX, rtk_demux_dec_ubx.proto_id);
    TEST_ASSERT_EQUAL_UINT16(4096U, rtk_demux_dec_ubx.max_frame);
    assert_only_first_byte(&rtk_demux_dec_ubx, 0xB5U);

    TEST_ASSERT_NOT_NULL(rtk_demux_dec_unicore_bin.name);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_UNICORE_BIN, rtk_demux_dec_unicore_bin.proto_id);
    TEST_ASSERT_EQUAL_UINT16(4096U, rtk_demux_dec_unicore_bin.max_frame);
    assert_only_first_byte(&rtk_demux_dec_unicore_bin, 0xAAU);

    TEST_ASSERT_NOT_NULL(rtk_demux_dec_unicore_hash.name);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_UNICORE_HASH, rtk_demux_dec_unicore_hash.proto_id);
    TEST_ASSERT_EQUAL_UINT16(1024U, rtk_demux_dec_unicore_hash.max_frame);
    assert_only_first_byte(&rtk_demux_dec_unicore_hash, (uint8_t)'#');

    TEST_ASSERT_NOT_NULL(rtk_proto_demux_decoder.name);
    TEST_ASSERT_EQUAL_UINT16(RTK_PROTO_DEMUX_ID, rtk_proto_demux_decoder.proto_id);
    TEST_ASSERT_EQUAL_UINT16(RTK_PROTO_MAX_FRAME, rtk_proto_demux_decoder.max_frame);
    assert_only_first_byte(&rtk_proto_demux_decoder, RTK_PROTO_SYNC_A);
}

/* -------------------------------------------------------------------------- */
/* 2. RTCM3                                                                   */
/* -------------------------------------------------------------------------- */

static void test_rtcm3_valid_early_reject_and_crc_err(void)
{
    const rtk_demux_decoder_t *const decs[] = {&rtk_demux_dec_rtcm3};
    static const uint8_t s_bad_b1[] = {0x04U, 0x08U, 0x10U, 0x20U, 0x40U, 0x80U, 0xFCU, 0xFFU};
    static uint8_t s_max_pl[DEMUX_GEN_RTCM3_MAX_PAYLOAD];
    uint8_t rtcm_1005[19];
    size_t flen = 0U;
    size_t end_pos = 0U;

    /* 0 载荷（6 字节） */
    TEST_ASSERT_TRUE(demux_gen_rtcm3(s_work_buf, sizeof(s_work_buf), NULL, 0U, &flen));
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME, step_by_need(&rtk_demux_dec_rtcm3, s_work_buf, flen, &end_pos, NULL));
    TEST_ASSERT_EQUAL_UINT(6U, end_pos);

    /* 典型 RTCM 1005 载荷（消息号 1005 = 0x3ED，19 字节） */
    memset(rtcm_1005, 0x3AU, sizeof(rtcm_1005));
    rtcm_1005[0] = 0x3EU;
    rtcm_1005[1] = 0xD0U;
    TEST_ASSERT_TRUE(demux_gen_rtcm3(s_work_buf, sizeof(s_work_buf), rtcm_1005, sizeof(rtcm_1005), &flen));
    run_demux_feed_and_abort(decs, 1U, s_work_buf, flen);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_RTCM3, s_sink.frames[0].proto_id);
    TEST_ASSERT_EQUAL_UINT(flen, s_sink.frames[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf, s_sink.frames[0].data, flen);

    /* 最大载荷 1023（总长 1029 == max_frame），载荷阶段批量累积只调极少次 step */
    for (size_t i = 0U; i < sizeof(s_max_pl); i++) {
        s_max_pl[i] = (uint8_t)(i ^ 0x5AU); /* 仅取低 8 位 */
    }
    TEST_ASSERT_TRUE(demux_gen_rtcm3(s_work_buf, sizeof(s_work_buf), s_max_pl, sizeof(s_max_pl), &flen));
    TEST_ASSERT_EQUAL_UINT(1029U, flen);
    {
        size_t calls = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME, step_by_need(&rtk_demux_dec_rtcm3, s_work_buf, flen, &end_pos, &calls));
        TEST_ASSERT_EQUAL_UINT(1029U, end_pos);
        TEST_ASSERT_TRUE(calls <= 4U);
    }

    /* 早拒绝点：第 2 字节高 6 位非 0 必须在 len == 2 拒绝 */
    for (size_t i = 0U; i < sizeof(s_bad_b1); i++) {
        uint8_t prefix[3] = {0xD3U, s_bad_b1[i], 0x00U};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_rtcm3.step(prefix, 2U, &need));
    }

    /* CRC 错：翻转头、载荷、CRC 各一处均应 REJECT；且能从坏帧载荷内重同步出真帧 */
    TEST_ASSERT_TRUE(demux_gen_rtcm3(s_work_buf, sizeof(s_work_buf), rtcm_1005, sizeof(rtcm_1005), &flen));
    s_work_buf[flen - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_rtcm3, s_work_buf, flen, &end_pos, NULL));
    s_work_buf[flen - 1U] ^= 0x01U;
    s_work_buf[5] ^= 0x80U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_rtcm3, s_work_buf, flen, &end_pos, NULL));

    /* 坏 RTCM3 头（D3 00 04）后紧跟真 RTCM3（6 字节）：前帧 CRC 错回溯后应恢复真帧 */
    s_work_buf[0] = 0xD3U;
    s_work_buf[1] = 0x00U;
    s_work_buf[2] = 0x04U;
    s_work_buf[3] = 0x11U;
    TEST_ASSERT_TRUE(demux_gen_rtcm3(&s_work_buf[4], sizeof(s_work_buf) - 4U, NULL, 0U, &flen));
    run_demux_feed_and_abort(decs, 1U, s_work_buf, 4U + flen);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT(6U, s_sink.frames[0].len);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.unknown_len);
}

/* -------------------------------------------------------------------------- */
/* 3. NMEA                                                                    */
/* -------------------------------------------------------------------------- */

static void test_nmea_valid_early_reject_chars_case_and_len(void)
{
    const rtk_demux_decoder_t *const decs[] = {&rtk_demux_dec_nmea};
    static const uint8_t s_bad_b1[] = {'$', '@', 'a', 'z', '/', ':', '[', '*', ' ', '\r', '\n', 0x1FU, 0x7FU, 0x80U};
    static const uint8_t s_bad_body[] = {'$', 0x00U, 0x09U, 0x0AU, 0x0DU, 0x1FU, 0x7FU, 0x80U, 0xFFU};
    static const char *const s_valid_bodies[] = {
        "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,",
        "PUBX,00,123456,0.0",
        "A",
        "Z,~ ",
        "0TEST",
        "9TEST",
    };
    char max_body[DEMUX_GEN_NMEA_MAX_FRAME - 5U];
    size_t flen = 0U;
    size_t end_pos = 0U;

    /* 正例 + 大小写十六进制 */
    for (size_t i = 0U; i < (sizeof(s_valid_bodies) / sizeof(s_valid_bodies[0])); i++) {
        for (int lc = 0; lc < 2; lc++) {
            TEST_ASSERT_TRUE(demux_gen_nmea(s_work_buf, sizeof(s_work_buf), s_valid_bodies[i], lc != 0, &flen));
            run_demux_feed_and_abort(decs, 1U, s_work_buf, flen);
            TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
            TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_NMEA, s_sink.frames[0].proto_id);
            TEST_ASSERT_EQUAL_UINT(flen, s_sink.frames[0].len);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf, s_sink.frames[0].data, flen);
        }
    }

    /* 最大长度 256 字节 */
    memset(max_body, 'X', 250U);
    max_body[0] = 'G';
    max_body[250] = '\0';
    TEST_ASSERT_TRUE(demux_gen_nmea(s_work_buf, sizeof(s_work_buf), max_body, false, &flen));
    TEST_ASSERT_EQUAL_UINT(256U, flen);
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME, step_by_need(&rtk_demux_dec_nmea, s_work_buf, flen, &end_pos, NULL));
    TEST_ASSERT_EQUAL_UINT(256U, end_pos);

    /* 长度超限（257 字节，先放 251 字节正文 + *00\r\n），后面紧跟合法 NMEA */
    s_work_buf[0] = (uint8_t)'$';
    memset(&s_work_buf[1], 'X', 251U);
    s_work_buf[1] = (uint8_t)'G';
    memcpy(&s_work_buf[252], "*00\r\n", 5U);
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_nmea, s_work_buf, 257U, &end_pos, NULL));
    TEST_ASSERT_TRUE(end_pos <= 256U);
    TEST_ASSERT_TRUE(demux_gen_nmea(&s_work_buf[257], sizeof(s_work_buf) - 257U, "GNGGA,OK", false, &flen));
    run_demux_feed_and_abort(decs, 1U, s_work_buf, 257U + flen);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT(257U, s_sink.unknown_len);

    /* 早拒绝：第 2 字节非 [A-Z0-9] */
    for (size_t i = 0U; i < sizeof(s_bad_b1); i++) {
        uint8_t p[2] = {(uint8_t)'$', s_bad_b1[i]};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_nmea.step(p, 2U, &need));
    }

    /* 正文非法字符立即拒绝 */
    for (size_t i = 0U; i < sizeof(s_bad_body); i++) {
        uint8_t p[4] = {(uint8_t)'$', (uint8_t)'G', (uint8_t)'P', s_bad_body[i]};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_nmea.step(p, 4U, &need));
    }

    /* 校验格式与行尾错误 */
    {
        static const char *const s_bad_tails[] = {
            "$GP*G0\r\n", "$GP*0g\r\n", "$GP*37\n", "$GP*37\rX",
            "$GP*37\r\r", "$GP*37  ",   "$GP*\r\n", "$GP*38\r\n", /* 正确 XOR 为 0x37 */
        };
        for (size_t i = 0U; i < (sizeof(s_bad_tails) / sizeof(s_bad_tails[0])); i++) {
            size_t tlen = strlen(s_bad_tails[i]);
            TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT,
                              step_by_need(&rtk_demux_dec_nmea, (const uint8_t *)s_bad_tails[i], tlen, &end_pos, NULL));
        }
    }

    /* 正文中含 '$' 立即拒并在分流器中恢复后句 */
    {
        static const char s_broken[] = "$GPGGA,BAD";
        size_t blen = strlen(s_broken);
        memcpy(s_work_buf, s_broken, blen);
        TEST_ASSERT_TRUE(demux_gen_nmea(&s_work_buf[blen], sizeof(s_work_buf) - blen, "GNGGA,1", true, &flen));
        run_demux_feed_and_abort(decs, 1U, s_work_buf, blen + flen);
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
        TEST_ASSERT_EQUAL_UINT(blen, s_sink.unknown_len);
    }
}

/* -------------------------------------------------------------------------- */
/* 4. UBX                                                                     */
/* -------------------------------------------------------------------------- */

static void test_ubx_valid_early_reject_len_and_ck_err(void)
{
    const rtk_demux_decoder_t *const decs[] = {&rtk_demux_dec_ubx};
    static const uint8_t s_bad_b1[] = {0x00U, 0x61U, 0x63U, 0xB5U, 0xFFU};
    static uint8_t s_max_pl[DEMUX_GEN_UBX_MAX_PAYLOAD];
    uint8_t pl[12];
    size_t flen = 0U;
    size_t end_pos = 0U;

    /* 0 载荷（8 字节） */
    TEST_ASSERT_TRUE(demux_gen_ubx(s_work_buf, sizeof(s_work_buf), 0x0AU, 0x04U, NULL, 0U, &flen));
    TEST_ASSERT_EQUAL_UINT(8U, flen);
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME, step_by_need(&rtk_demux_dec_ubx, s_work_buf, flen, &end_pos, NULL));

    /* 典型载荷 + 经引擎投递 */
    memset(pl, 0x55U, sizeof(pl));
    TEST_ASSERT_TRUE(demux_gen_ubx(s_work_buf, sizeof(s_work_buf), 0x01U, 0x07U, pl, sizeof(pl), &flen));
    run_demux_feed_and_abort(decs, 1U, s_work_buf, flen);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_UBX, s_sink.frames[0].proto_id);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf, s_sink.frames[0].data, flen);

    /* 最大载荷 4088（总长 4096 == max_frame），验证批量累积 */
    for (size_t i = 0U; i < sizeof(s_max_pl); i++) {
        s_max_pl[i] = (uint8_t)(i & 0xFFU);
    }
    TEST_ASSERT_TRUE(demux_gen_ubx(s_work_buf, sizeof(s_work_buf), 0x02U, 0x15U, s_max_pl, sizeof(s_max_pl), &flen));
    TEST_ASSERT_EQUAL_UINT(4096U, flen);
    {
        size_t calls = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME, step_by_need(&rtk_demux_dec_ubx, s_work_buf, flen, &end_pos, &calls));
        TEST_ASSERT_TRUE(calls <= 4U);
    }

    /* 早拒绝：第 2 字节 != 0x62 */
    for (size_t i = 0U; i < sizeof(s_bad_b1); i++) {
        uint8_t p[2] = {0xB5U, s_bad_b1[i]};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_ubx.step(p, 2U, &need));
    }

    /* 早拒绝：payload_len = 4089（超限）与 0xFFFF */
    {
        uint8_t hdr_4089[6] = {0xB5U, 0x62U, 0x01U, 0x07U, 0xF9U, 0x0FU};
        uint8_t hdr_ffff[6] = {0xB5U, 0x62U, 0x01U, 0x07U, 0xFFU, 0xFFU};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_ubx.step(hdr_4089, 6U, &need));
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_ubx.step(hdr_ffff, 6U, &need));
    }

    /* 校验错：CK_A 错、CK_B 错、载荷篡改 */
    TEST_ASSERT_TRUE(demux_gen_ubx(s_work_buf, sizeof(s_work_buf), 0x01U, 0x07U, pl, sizeof(pl), &flen));
    s_work_buf[flen - 2U] ^= 0x01U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_ubx, s_work_buf, flen, &end_pos, NULL));
    s_work_buf[flen - 2U] ^= 0x01U;
    s_work_buf[flen - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_ubx, s_work_buf, flen, &end_pos, NULL));
    s_work_buf[flen - 1U] ^= 0x01U;
    s_work_buf[6] ^= 0x01U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_ubx, s_work_buf, flen, &end_pos, NULL));

    /* 坏 UBX 头后嵌真 UBX 帧经分流器恢复 */
    s_work_buf[0] = 0xB5U;
    s_work_buf[1] = 0x62U;
    s_work_buf[2] = 0x01U;
    s_work_buf[3] = 0x01U;
    s_work_buf[4] = 0x04U;
    s_work_buf[5] = 0x00U;
    TEST_ASSERT_TRUE(demux_gen_ubx(&s_work_buf[6], sizeof(s_work_buf) - 6U, 0x0AU, 0x04U, NULL, 0U, &flen));
    run_demux_feed_and_abort(decs, 1U, s_work_buf, 6U + flen);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT(8U, s_sink.frames[0].len);
    TEST_ASSERT_EQUAL_UINT(6U, s_sink.unknown_len);
}

/* -------------------------------------------------------------------------- */
/* 5. Unicore 二进制                                                          */
/* -------------------------------------------------------------------------- */

static void test_unicore_bin_valid_early_reject_len_and_crc_err(void)
{
    const rtk_demux_decoder_t *const decs[] = {&rtk_demux_dec_unicore_bin};
    static uint8_t s_max_pl[DEMUX_GEN_UNICORE_BIN_MAX_PAYLOAD];
    uint8_t rest[16];
    uint8_t pl[32];
    size_t flen = 0U;
    size_t end_pos = 0U;

    /* 0 载荷（28 字节） */
    TEST_ASSERT_TRUE(demux_gen_unicore_bin(s_work_buf, sizeof(s_work_buf), 42U, 15U, NULL, NULL, 0U, &flen));
    TEST_ASSERT_EQUAL_UINT(28U, flen);
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME, step_by_need(&rtk_demux_dec_unicore_bin, s_work_buf, flen, &end_pos, NULL));

    /* 非零时间头 + 载荷 + 经引擎投递 */
    for (size_t i = 0U; i < sizeof(rest); i++) {
        rest[i] = (uint8_t)(0x80U + i); /* 范围 0x80..0x8F */
    }
    for (size_t i = 0U; i < sizeof(pl); i++) {
        pl[i] = (uint8_t)(i * 3U); /* 范围 0..93 */
    }
    TEST_ASSERT_TRUE(demux_gen_unicore_bin(s_work_buf, sizeof(s_work_buf), 101U, 30U, rest, pl, sizeof(pl), &flen));
    run_demux_feed_and_abort(decs, 1U, s_work_buf, flen);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_UNICORE_BIN, s_sink.frames[0].proto_id);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_work_buf, s_sink.frames[0].data, flen);

    /* 最大载荷 4068（总长 4096 == max_frame），验证批量累积 */
    for (size_t i = 0U; i < sizeof(s_max_pl); i++) {
        s_max_pl[i] = (uint8_t)(i & 0xFFU);
    }
    TEST_ASSERT_TRUE(
        demux_gen_unicore_bin(s_work_buf, sizeof(s_work_buf), 999U, 0U, NULL, s_max_pl, sizeof(s_max_pl), &flen));
    TEST_ASSERT_EQUAL_UINT(4096U, flen);
    {
        size_t calls = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME,
                          step_by_need(&rtk_demux_dec_unicore_bin, s_work_buf, flen, &end_pos, &calls));
        TEST_ASSERT_TRUE(calls <= 8U);
    }

    /* 早拒绝：第 2 字节 != 0x44；第 3 字节 != 0xB5（含产品帧 0x18） */
    {
        uint8_t bad2[2] = {0xAAU, 0x43U};
        uint8_t bad3_prod[3] = {0xAAU, 0x44U, 0x18U};
        uint8_t bad3_other[3] = {0xAAU, 0x44U, 0xB4U};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_unicore_bin.step(bad2, 2U, &need));
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_unicore_bin.step(bad3_prod, 3U, &need));
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_unicore_bin.step(bad3_other, 3U, &need));
    }

    /* 早拒绝：payload_len = 4069（超限）与 0xFFFF，必须在 <= 24 字节头部完成前拒绝 */
    {
        uint8_t hdr[24];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 0xAAU;
        hdr[1] = 0x44U;
        hdr[2] = 0xB5U;
        hdr[6] = 0xE5U; /* 4069 = 0x0FE5 */
        hdr[7] = 0x0FU;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_unicore_bin, hdr, 24U, &end_pos, NULL));
        TEST_ASSERT_TRUE(end_pos <= 24U);

        hdr[6] = 0xFFU;
        hdr[7] = 0xFFU;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, step_by_need(&rtk_demux_dec_unicore_bin, hdr, 24U, &end_pos, NULL));
        TEST_ASSERT_TRUE(end_pos <= 24U);
    }

    /* CRC 错误 & 若错用标准 IEEE 取反 CRC-32 必须拒绝 */
    TEST_ASSERT_TRUE(demux_gen_unicore_bin(s_work_buf, sizeof(s_work_buf), 101U, 30U, rest, pl, sizeof(pl), &flen));
    s_work_buf[flen - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT,
                      step_by_need(&rtk_demux_dec_unicore_bin, s_work_buf, flen, &end_pos, NULL));
    {
        uint32_t ieee_crc = demux_gen_crc32_ieee(s_work_buf, flen - 4U);
        s_work_buf[flen - 4U] = (uint8_t)(ieee_crc & 0xFFU);
        s_work_buf[flen - 3U] = (uint8_t)((ieee_crc >> 8U) & 0xFFU);
        s_work_buf[flen - 2U] = (uint8_t)((ieee_crc >> 16U) & 0xFFU);
        s_work_buf[flen - 1U] = (uint8_t)((ieee_crc >> 24U) & 0xFFU);
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT,
                          step_by_need(&rtk_demux_dec_unicore_bin, s_work_buf, flen, &end_pos, NULL));
    }
}

/* -------------------------------------------------------------------------- */
/* 6. Unicore Hash                                                            */
/* -------------------------------------------------------------------------- */

static void test_unicore_hash_valid_early_reject_chars_case_and_len(void)
{
    const rtk_demux_decoder_t *const decs[] = {&rtk_demux_dec_unicore_hash};
    static const uint8_t s_bad_b1[] = {'0', '9', 'a', 'z', '#', '$', '*', ' ', '\r', 0x1FU, 0x7FU, 0x80U};
    static const uint8_t s_bad_body[] = {'#', '$', 0x00U, 0x09U, 0x0AU, 0x0DU, 0x1FU, 0x7FU, 0x80U};
    static const char s_ver_real[] =
        "#VERSION,97,GPS,FINE,2282,248561000,0,0,18,676;UM980,R4.10Build7923,"
        "HRPT00-S10C-P,2310415000001-MD22B1224962616,ff3bac96f31f9bdd,2022/09/28*7432d4ed\r\n";
    char max_body[DEMUX_GEN_UNICORE_HASH_MAX_FRAME];
    size_t flen = 0U;
    size_t end_pos = 0U;

    /* 真实 UM980 #VERSION 向量 */
    run_demux_feed_and_abort(decs, 1U, (const uint8_t *)s_ver_real, strlen(s_ver_real));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_UNICORE_HASH, s_sink.frames[0].proto_id);
    TEST_ASSERT_EQUAL_UINT(strlen(s_ver_real), s_sink.frames[0].len);

    /* 两种校验（CRC-32 与 XOR）+ 大小写十六进制 + 边界字母 'A'/'Z' + 边界 ASCII */
    for (int lc = 0; lc < 2; lc++) {
        TEST_ASSERT_TRUE(
            demux_gen_unicore_hash_crc32(s_work_buf, sizeof(s_work_buf), "BESTPOSA,99, ~", lc != 0, &flen));
        run_demux_feed_and_abort(decs, 1U, s_work_buf, flen);
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);

        TEST_ASSERT_TRUE(demux_gen_unicore_hash_xor(s_work_buf, sizeof(s_work_buf), "Z,1", lc != 0, &flen));
        run_demux_feed_and_abort(decs, 1U, s_work_buf, flen);
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    }

    /* 最大长度 1024（XOR 与 CRC-32 两种模式） */
    memset(max_body, 'A', 1018U);
    max_body[1018] = '\0';
    TEST_ASSERT_TRUE(demux_gen_unicore_hash_xor(s_work_buf, sizeof(s_work_buf), max_body, false, &flen));
    TEST_ASSERT_EQUAL_UINT(1024U, flen);
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME,
                      step_by_need(&rtk_demux_dec_unicore_hash, s_work_buf, flen, &end_pos, NULL));

    max_body[1012] = '\0';
    TEST_ASSERT_TRUE(demux_gen_unicore_hash_crc32(s_work_buf, sizeof(s_work_buf), max_body, false, &flen));
    TEST_ASSERT_EQUAL_UINT(1024U, flen);
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME,
                      step_by_need(&rtk_demux_dec_unicore_hash, s_work_buf, flen, &end_pos, NULL));

    /* 长度超限 1025 字节 */
    s_work_buf[0] = (uint8_t)'#';
    memset(&s_work_buf[1], 'A', 1019U);
    memcpy(&s_work_buf[1020], "*00\r\n", 5U);
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT,
                      step_by_need(&rtk_demux_dec_unicore_hash, s_work_buf, 1025U, &end_pos, NULL));

    /* 早拒绝：第 2 字节非 'A'..'Z'（包含数字 '0'..'9'） */
    for (size_t i = 0U; i < sizeof(s_bad_b1); i++) {
        uint8_t p[2] = {(uint8_t)'#', s_bad_b1[i]};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_unicore_hash.step(p, 2U, &need));
    }

    /* 正文非法字符立即拒绝 */
    for (size_t i = 0U; i < sizeof(s_bad_body); i++) {
        uint8_t p[4] = {(uint8_t)'#', (uint8_t)'V', (uint8_t)'E', s_bad_body[i]};
        size_t need = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_demux_dec_unicore_hash.step(p, 4U, &need));
    }

    /* 校验位数不对 / 非十六进制 / 缺 \r\n / 错校验值 */
    {
        static const char *const s_bad_hash[] = {
            "#A*\r\n",         "#A*4\r\n", "#A*41A\r\n", "#A*1234567\r\n", "#A*123456789\r\n", "#A*1G\r\n",
            "#A*1234567G\r\n", "#A*41\n",  "#A*41\rX",   "#A*42\r\n",      "#A*00000000\r\n",
        };
        for (size_t i = 0U; i < (sizeof(s_bad_hash) / sizeof(s_bad_hash[0])); i++) {
            size_t tlen = strlen(s_bad_hash[i]);
            TEST_ASSERT_EQUAL(
                RTK_DEMUX_STEP_REJECT,
                step_by_need(&rtk_demux_dec_unicore_hash, (const uint8_t *)s_bad_hash[i], tlen, &end_pos, NULL));
        }
    }

    /* # 句中含 '$' 立即拒并在 {nmea, unicore_hash} 中恢复其后的 $GPGGA */
    {
        const rtk_demux_decoder_t *const combo[] = {&rtk_demux_dec_nmea, &rtk_demux_dec_unicore_hash};
        static const char s_bad_prefix[] = "#VERSION,CUT";
        size_t plen = strlen(s_bad_prefix);
        memcpy(s_work_buf, s_bad_prefix, plen);
        TEST_ASSERT_TRUE(demux_gen_nmea(&s_work_buf[plen], sizeof(s_work_buf) - plen, "GPGGA,1", false, &flen));
        run_demux_feed_and_abort(combo, 2U, s_work_buf, plen + flen);
        TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
        TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_NMEA, s_sink.frames[0].proto_id);
        TEST_ASSERT_EQUAL_UINT(plen, s_sink.unknown_len);
    }
}

/* -------------------------------------------------------------------------- */
/* 7. 产品帧解码器与 rtk_proto_frame_view                                     */
/* -------------------------------------------------------------------------- */

static void test_product_demux_and_frame_view(void)
{
    const rtk_demux_decoder_t *const decs[] = {&rtk_proto_demux_decoder};
    static const uint8_t s_frame_c[] = {
        0xAA, 0x44, 0x18, 0x14, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x03, 0x04, 0x00, 0x00, 0x05, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x34, 0x05, 0xAA,
    };
    static uint8_t s_max_pl[RTK_PROTO_MAX_PAYLOAD];
    rtk_proto_frame_t view;
    rtk_proto_frame_t sentinel;
    size_t flen = 0U;
    size_t end_pos = 0U;

    /* 0 载荷、已知向量 frame_c、最大载荷 256 */
    TEST_ASSERT_TRUE(demux_gen_product(s_work_buf, sizeof(s_work_buf), 0x0001U, RTK_PROTO_SENDER_APP,
                                       RTK_PROTO_TYPE_QUERY, NULL, 0U, &flen));
    run_demux_feed_and_abort(decs, 1U, s_work_buf, flen);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT16(RTK_PROTO_DEMUX_ID, s_sink.frames[0].proto_id);
    TEST_ASSERT_TRUE(rtk_proto_frame_view(s_sink.frames[0].data, s_sink.frames[0].len, &view));
    TEST_ASSERT_EQUAL_HEX16(0x0001U, view.msg_id);
    TEST_ASSERT_EQUAL_UINT16(0U, view.payload_len);

    run_demux_feed_and_abort(decs, 1U, s_frame_c, sizeof(s_frame_c));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.frame_count);
    TEST_ASSERT_TRUE(rtk_proto_frame_view(s_sink.frames[0].data, s_sink.frames[0].len, &view));
    TEST_ASSERT_EQUAL_HEX16(0x0041U, view.msg_id);
    TEST_ASSERT_EQUAL_UINT8(RTK_PROTO_SENDER_ALARM, view.sender);
    TEST_ASSERT_EQUAL_UINT8(RTK_PROTO_TYPE_PUSH, view.type);
    TEST_ASSERT_EQUAL_UINT16(8U, view.payload_len);
    TEST_ASSERT_EQUAL_PTR(&s_sink.frames[0].data[RTK_PROTO_HEADER_LEN], view.payload);

    for (size_t i = 0U; i < sizeof(s_max_pl); i++) {
        s_max_pl[i] = (uint8_t)i; /* i < 256 */
    }
    TEST_ASSERT_TRUE(demux_gen_product(s_work_buf, sizeof(s_work_buf), 0x0077U, RTK_PROTO_SENDER_DEV,
                                       RTK_PROTO_TYPE_SET_RES, s_max_pl, RTK_PROTO_MAX_PAYLOAD, &flen));
    TEST_ASSERT_EQUAL_UINT(RTK_PROTO_MAX_FRAME, flen);
    {
        size_t calls = 0U;
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_FRAME,
                          step_by_need(&rtk_proto_demux_decoder, s_work_buf, flen, &end_pos, &calls));
        TEST_ASSERT_TRUE(calls <= 8U);
    }

    /* 早拒绝：第 2、3、4 字节 + payload_len > 256 */
    {
        uint8_t b2[2] = {0xAAU, 0x45U};
        uint8_t b3[3] = {0xAAU, 0x44U, 0xB5U};
        uint8_t b4[4] = {0xAAU, 0x44U, 0x18U, 0x15U};
        uint8_t hdr_257[RTK_PROTO_HEADER_LEN];
        size_t need = 0U;

        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_proto_demux_decoder.step(b2, 2U, &need));
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_proto_demux_decoder.step(b3, 3U, &need));
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT, rtk_proto_demux_decoder.step(b4, 4U, &need));

        memcpy(hdr_257, s_frame_c, RTK_PROTO_HEADER_LEN);
        hdr_257[12] = 0x01U;
        hdr_257[13] = 0x01U; /* 257 */
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT,
                          step_by_need(&rtk_proto_demux_decoder, hdr_257, sizeof(hdr_257), &end_pos, NULL));
        TEST_ASSERT_TRUE(end_pos <= RTK_PROTO_HEADER_LEN);
    }

    /* CRC 错 + NovAtel 非取反 CRC 必须被产品解码器拒绝 */
    memcpy(s_work_buf, s_frame_c, sizeof(s_frame_c));
    s_work_buf[sizeof(s_frame_c) - 1U] ^= 0x01U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT,
                      step_by_need(&rtk_proto_demux_decoder, s_work_buf, sizeof(s_frame_c), &end_pos, NULL));
    {
        uint32_t nov_crc = demux_gen_crc32_novatel(s_work_buf, sizeof(s_frame_c) - 4U);
        s_work_buf[sizeof(s_frame_c) - 4U] = (uint8_t)(nov_crc & 0xFFU);
        s_work_buf[sizeof(s_frame_c) - 3U] = (uint8_t)((nov_crc >> 8U) & 0xFFU);
        s_work_buf[sizeof(s_frame_c) - 2U] = (uint8_t)((nov_crc >> 16U) & 0xFFU);
        s_work_buf[sizeof(s_frame_c) - 1U] = (uint8_t)((nov_crc >> 24U) & 0xFFU);
        TEST_ASSERT_EQUAL(RTK_DEMUX_STEP_REJECT,
                          step_by_need(&rtk_proto_demux_decoder, s_work_buf, sizeof(s_frame_c), &end_pos, NULL));
    }

    /* rtk_proto_frame_view 非法输入不写 out；不重复校验 CRC；保留 protocol/interval */
    memset(&sentinel, 0xA5, sizeof(sentinel));
    view = sentinel;
    TEST_ASSERT_FALSE(rtk_proto_frame_view(NULL, sizeof(s_frame_c), &view));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &view, sizeof(view));
    TEST_ASSERT_FALSE(rtk_proto_frame_view(s_frame_c, sizeof(s_frame_c), NULL));
    TEST_ASSERT_FALSE(rtk_proto_frame_view(s_frame_c, 23U, &view));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &view, sizeof(view));
    TEST_ASSERT_FALSE(rtk_proto_frame_view(s_frame_c, RTK_PROTO_MAX_FRAME + 1U, &view));
    TEST_ASSERT_FALSE(rtk_proto_frame_view(s_frame_c, sizeof(s_frame_c) - 1U, &view)); /* len 与 payload_len 不一致 */

    memcpy(s_work_buf, s_frame_c, sizeof(s_frame_c));
    s_work_buf[2] = 0xB5U;
    TEST_ASSERT_FALSE(rtk_proto_frame_view(s_work_buf, sizeof(s_frame_c), &view));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &view, sizeof(view));

    memcpy(s_work_buf, s_frame_c, sizeof(s_frame_c));
    s_work_buf[3] = 0x15U;
    TEST_ASSERT_FALSE(rtk_proto_frame_view(s_work_buf, sizeof(s_frame_c), &view));

    memcpy(s_work_buf, s_frame_c, sizeof(s_frame_c));
    s_work_buf[12] = 0x01U;
    s_work_buf[13] = 0x01U; /* payload_len = 257 */
    TEST_ASSERT_FALSE(rtk_proto_frame_view(s_work_buf, sizeof(s_frame_c), &view));

    memcpy(s_work_buf, s_frame_c, sizeof(s_frame_c));
    s_work_buf[18] = 0x07U;                      /* protocol */
    s_work_buf[19] = 0x0AU;                      /* interval */
    s_work_buf[sizeof(s_frame_c) - 1U] ^= 0xFFU; /* 故意坏 CRC：frame_view 不重复校验 */
    TEST_ASSERT_TRUE(rtk_proto_frame_view(s_work_buf, sizeof(s_frame_c), &view));
    TEST_ASSERT_EQUAL_HEX8(0x07U, view.protocol);
    TEST_ASSERT_EQUAL_HEX8(0x0AU, view.interval);
}

/* -------------------------------------------------------------------------- */
/* 8. '$' 同首字节冲突（NMEA vs 自定义 SBF $@）                                */
/* -------------------------------------------------------------------------- */

static void test_dollar_conflict_with_custom_decoder(void)
{
    const rtk_demux_decoder_t *const order_a[] = {&rtk_demux_dec_nmea, &s_dec_custom_sbf};
    const rtk_demux_decoder_t *const order_b[] = {&s_dec_custom_sbf, &rtk_demux_dec_nmea};
    static const uint8_t s_sbf[] = {'$', '@', 0x11U, 0x22U, 0x33U, 0x44U};
    size_t nmea_len = 0U;
    size_t pos = 0U;

    TEST_ASSERT_TRUE(demux_gen_nmea(&s_work_buf[pos], sizeof(s_work_buf) - pos, "GNGGA,123", false, &nmea_len));
    pos += nmea_len;
    memcpy(&s_work_buf[pos], s_sbf, sizeof(s_sbf));
    pos += sizeof(s_sbf);
    TEST_ASSERT_TRUE(demux_gen_nmea(&s_work_buf[pos], sizeof(s_work_buf) - pos, "GPRMC,456", true, &nmea_len));
    pos += nmea_len;

    run_demux_feed_and_abort(order_a, 2U, s_work_buf, pos);
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_NMEA, s_sink.frames[0].proto_id);
    TEST_ASSERT_EQUAL_UINT16(s_dec_custom_sbf.proto_id, s_sink.frames[1].proto_id);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_NMEA, s_sink.frames[2].proto_id);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->unknown_bytes);

    run_demux_feed_and_abort(order_b, 2U, s_work_buf, pos);
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.frame_count);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_NMEA, s_sink.frames[0].proto_id);
    TEST_ASSERT_EQUAL_UINT16(s_dec_custom_sbf.proto_id, s_sink.frames[1].proto_id);
    TEST_ASSERT_EQUAL_UINT16(RTK_DEMUX_PROTO_NMEA, s_sink.frames[2].proto_id);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->unknown_bytes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decoder_descriptors_and_first_bytes);
    RUN_TEST(test_rtcm3_valid_early_reject_and_crc_err);
    RUN_TEST(test_nmea_valid_early_reject_chars_case_and_len);
    RUN_TEST(test_ubx_valid_early_reject_len_and_ck_err);
    RUN_TEST(test_unicore_bin_valid_early_reject_len_and_crc_err);
    RUN_TEST(test_unicore_hash_valid_early_reject_chars_case_and_len);
    RUN_TEST(test_product_demux_and_frame_view);
    RUN_TEST(test_dollar_conflict_with_custom_decoder);
    return UNITY_END();
}
