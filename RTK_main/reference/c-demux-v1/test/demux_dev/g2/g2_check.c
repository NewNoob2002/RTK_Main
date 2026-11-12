// G2 自测（不参与构建、不交付）：用 test/demux_dev/g2/g2_vectors.h 里的独立向量驱动各解码器的
// step，核对 1) 合法帧逐前缀纯函数行为与 need 契约，2) 校验错/结构错必被拒绝，3) frame_view 字段。
// 向量由 gen_vectors.py（逐位 CRC 实现）生成，与被核对代码的查表法互相独立。
//
// Generated-by: pi + DeepSeek-V4.1-Flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include <stdio.h>
#include <string.h>

#include "g2_vectors.h"
#include "rtk_demux_decoders.h"
#include "rtk_proto_demux.h"

static int s_fail;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            s_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* 合法帧：每个真前缀都必须 NEED_MORE 且满足 need 契约；整帧长度时必须 FRAME。 */
static void expect_frame(const rtk_demux_decoder_t *dec, const char *name, const uint8_t *data, size_t len)
{
    size_t need = 0U;
    rtk_demux_step_t step = RTK_DEMUX_STEP_REJECT;

    CHECK(dec->first_byte(data[0]), "%s/%s: first_byte 不接受首字节", dec->name, name);
    for (size_t prefix = 1U; prefix < len; prefix++) {
        need = 0U;
        step = dec->step(data, prefix, &need);
        CHECK(step == RTK_DEMUX_STEP_NEED_MORE, "%s/%s: 前缀 %zu 得到 %d（应为 NEED_MORE）", dec->name, name, prefix,
              (int)step);
        CHECK(need >= 1U, "%s/%s: 前缀 %zu need=%zu（应 ≥ 1）", dec->name, name, prefix, need);
        CHECK((prefix + need) <= (size_t)dec->max_frame, "%s/%s: 前缀 %zu need=%zu 超出 max_frame %u", dec->name, name,
              prefix, need, (unsigned)dec->max_frame);
        /* 纯函数：同一前缀重复调用结论一致 */
        {
            size_t need2 = 0U;

            CHECK(dec->step(data, prefix, &need2) == step && need2 == need, "%s/%s: 前缀 %zu 非纯函数", dec->name, name,
                  prefix);
        }
    }
    need = 0U;
    step = dec->step(data, len, &need);
    CHECK(step == RTK_DEMUX_STEP_FRAME, "%s/%s: 整帧得到 %d（应为 FRAME, len=%zu）", dec->name, name, (int)step, len);
    CHECK(len <= (size_t)dec->max_frame, "%s/%s: 向量长度 %zu 超过 max_frame", dec->name, name, len);
}

/* 坏帧（只坏最后一个字节）：任何前缀都不得 FRAME；整帧必须 REJECT。 */
static void expect_reject(const rtk_demux_decoder_t *dec, const char *name, const uint8_t *data, size_t len)
{
    size_t need = 0U;
    rtk_demux_step_t step = RTK_DEMUX_STEP_FRAME;

    for (size_t prefix = 1U; prefix < len; prefix++) {
        need = 0U;
        step = dec->step(data, prefix, &need);
        CHECK(step != RTK_DEMUX_STEP_FRAME, "%s/%s: 前缀 %zu 提前成帧", dec->name, name, prefix);
    }
    need = 0U;
    CHECK(dec->step(data, len, &need) == RTK_DEMUX_STEP_REJECT, "%s/%s: 整帧未被拒绝", dec->name, name);
}

/* 在指定前缀长度上必须 REJECT（用于早拒绝点）。 */
static void expect_reject_at(const rtk_demux_decoder_t *dec, const char *what, const uint8_t *data, size_t prefix)
{
    size_t need = 0U;

    CHECK(dec->step(data, prefix, &need) == RTK_DEMUX_STEP_REJECT, "%s: 长度 %zu 应 REJECT", what, prefix);
}

static void test_rtcm3(void)
{
    expect_frame(&rtk_demux_dec_rtcm3, "1005", k_rtcm3_1005, sizeof(k_rtcm3_1005));
    expect_frame(&rtk_demux_dec_rtcm3, "max", k_rtcm3_max, sizeof(k_rtcm3_max));
    expect_frame(&rtk_demux_dec_rtcm3, "empty", k_rtcm3_empty, sizeof(k_rtcm3_empty));
    expect_reject(&rtk_demux_dec_rtcm3, "crc", b_k_rtcm3_1005, sizeof(b_k_rtcm3_1005));
    expect_reject(&rtk_demux_dec_rtcm3, "crc_max", b_k_rtcm3_max, sizeof(b_k_rtcm3_max));

    {
        /* 第 2 字节高 6 位保留位非 0 */
        static const uint8_t k_reserved[] = {0xD3, 0x04, 0x00, 0x00, 0x00, 0x00};

        expect_reject_at(&rtk_demux_dec_rtcm3, "rtcm3 保留位", k_reserved, 2U);
        CHECK(!rtk_demux_dec_rtcm3.first_byte(0xD4), "rtcm3: first_byte(0xD4) 应为 false");
    }
}

static void test_nmea(void)
{
    static uint8_t lower[sizeof(k_nmea_short)]; /* 小写十六进制校验位 */
    size_t star = 0U;
    static const uint8_t k_body_bad[] = {'$', 'G', 'G', 'A', 0x07U};
    static const uint8_t k_body_dollar[] = {'$', 'G', 'A', '$', 'A'};
    static const uint8_t k_lower_addr[] = {'$', 'g', 'A'};
    static const uint8_t k_bad_hex[] = {'$', 'G', 'A', '*', 'Z', 'Z'};
    static const uint8_t k_bad_cr[] = {'$', 'G', 'A', '*', '0', '0', 'X'};
    static uint8_t too_long[256];

    expect_frame(&rtk_demux_dec_nmea, "gga", k_nmea_gga, sizeof(k_nmea_gga));
    expect_frame(&rtk_demux_dec_nmea, "short", k_nmea_short, sizeof(k_nmea_short));
    expect_frame(&rtk_demux_dec_nmea, "max", k_nmea_max, sizeof(k_nmea_max));
    expect_reject(&rtk_demux_dec_nmea, "xor", b_k_nmea_gga, sizeof(b_k_nmea_gga));
    expect_reject(&rtk_demux_dec_nmea, "xor_max", b_k_nmea_max, sizeof(b_k_nmea_max));

    /* 小写十六进制校验位必须接受 */
    memcpy(lower, k_nmea_short, sizeof(lower));
    for (size_t idx = 0U; idx < sizeof(lower); idx++) {
        if (lower[idx] == '*') {
            star = idx;
            break;
        }
    }
    CHECK(star != 0U, "nmea: 向量里没有 '*'");
    for (size_t idx = star + 1U; idx < star + 3U; idx++) {
        if (lower[idx] >= 'A' && lower[idx] <= 'F') {
            lower[idx] = (uint8_t)(lower[idx] - 'A' + 'a');
        }
    }
    expect_frame(&rtk_demux_dec_nmea, "lower_hex", lower, sizeof(lower));

    expect_reject_at(&rtk_demux_dec_nmea, "nmea 非法正文字节", k_body_bad, 5U);
    expect_reject_at(&rtk_demux_dec_nmea, "nmea 正文含 '$'", k_body_dollar, 4U);
    expect_reject_at(&rtk_demux_dec_nmea, "nmea 第 2 字节小写", k_lower_addr, 2U);
    expect_reject_at(&rtk_demux_dec_nmea, "nmea 校验位非十六进制", k_bad_hex, 6U);
    expect_reject_at(&rtk_demux_dec_nmea, "nmea 校验位后非 '\\r'", k_bad_cr, 7U);

    /* 超过 max_frame 仍未结束 → 到 256 必须 REJECT */
    too_long[0] = '$';
    too_long[1] = 'G';
    memset(&too_long[2], 'A', sizeof(too_long) - 2U);
    expect_reject_at(&rtk_demux_dec_nmea, "nmea 超 max_frame", too_long, 256U);
    CHECK(!rtk_demux_dec_nmea.first_byte('!'), "nmea: first_byte('!') 应为 false");
}

static void test_ubx(void)
{
    static const uint8_t k_bad_sync[] = {0xB5, 0x63};
    static const uint8_t k_oversize[] = {0xB5, 0x62, 0x01, 0x07, 0xFA, 0x0F};

    expect_frame(&rtk_demux_dec_ubx, "poll", k_ubx_poll, sizeof(k_ubx_poll));
    expect_frame(&rtk_demux_dec_ubx, "payload", k_ubx_payload, sizeof(k_ubx_payload));
    expect_frame(&rtk_demux_dec_ubx, "max", k_ubx_max, sizeof(k_ubx_max));
    expect_reject(&rtk_demux_dec_ubx, "ck", b_k_ubx_payload, sizeof(b_k_ubx_payload));
    expect_reject_at(&rtk_demux_dec_ubx, "ubx 第 2 字节", k_bad_sync, 2U);
    expect_reject_at(&rtk_demux_dec_ubx, "ubx 长度超限", k_oversize, 6U);
    CHECK(!rtk_demux_dec_ubx.first_byte(0x62), "ubx: first_byte(0x62) 应为 false");
}

static void test_unicore_bin(void)
{
    static const uint8_t k_bad_b[] = {0xAA, 0x45};
    static const uint8_t k_bad_c[] = {0xAA, 0x44, 0xB6};
    static const uint8_t k_oversize[] = {0xAA, 0x44, 0xB5, 0x00, 0x01, 0x00, 0xFD, 0x0F};

    expect_frame(&rtk_demux_dec_unicore_bin, "min", k_ucbin_min, sizeof(k_ucbin_min));
    expect_frame(&rtk_demux_dec_unicore_bin, "payload", k_ucbin_payload, sizeof(k_ucbin_payload));
    expect_frame(&rtk_demux_dec_unicore_bin, "max", k_ucbin_max, sizeof(k_ucbin_max));
    expect_reject(&rtk_demux_dec_unicore_bin, "crc", b_k_ucbin_payload, sizeof(b_k_ucbin_payload));
    expect_reject_at(&rtk_demux_dec_unicore_bin, "unicore 第 2 字节", k_bad_b, 2U);
    expect_reject_at(&rtk_demux_dec_unicore_bin, "unicore 第 3 字节", k_bad_c, 3U);
    expect_reject_at(&rtk_demux_dec_unicore_bin, "unicore 长度超限", k_oversize, 8U);
}

static void test_unicore_hash(void)
{
    static const uint8_t k_bad_addr[] = {'#', '1', 'A'};
    static const uint8_t k_body_hash[] = {'#', 'V', 'A', '#'};
    /* '*' 是校验字段的起点，本身合法；其后不是十六进制才非法 */
    static const uint8_t k_star_then_bad[] = {'#', 'V', 'A', '*', 'Z'};
    static uint8_t too_long[1024];

    expect_frame(&rtk_demux_dec_unicore_hash, "crc_form", k_hash_crc, sizeof(k_hash_crc));
    expect_frame(&rtk_demux_dec_unicore_hash, "xor_form", k_hash_xor, sizeof(k_hash_xor));
    expect_frame(&rtk_demux_dec_unicore_hash, "max", k_hash_max, sizeof(k_hash_max));
    expect_reject(&rtk_demux_dec_unicore_hash, "crc", b_k_hash_crc, sizeof(b_k_hash_crc));
    expect_reject(&rtk_demux_dec_unicore_hash, "xor", b_k_hash_xor, sizeof(b_k_hash_xor));
    expect_reject_at(&rtk_demux_dec_unicore_hash, "hash 第 2 字节非大写字母", k_bad_addr, 2U);
    expect_reject_at(&rtk_demux_dec_unicore_hash, "hash 正文含 '#'", k_body_hash, 4U);
    expect_reject_at(&rtk_demux_dec_unicore_hash, "hash 校验位非十六进制", k_star_then_bad, 5U);

    too_long[0] = '#';
    too_long[1] = 'V';
    memset(&too_long[2], '0', sizeof(too_long) - 2U);
    expect_reject_at(&rtk_demux_dec_unicore_hash, "hash 超 max_frame", too_long, 1024U);
}

static void test_proto(void)
{
    static const uint8_t k_bad_b[] = {0xAA, 0x45};
    static const uint8_t k_bad_c[] = {0xAA, 0x44, 0x19};
    static const uint8_t k_bad_hdr[] = {0xAA, 0x44, 0x18, 0x15};
    static const uint8_t k_bad_len[] = {0xAA, 0x44, 0x18, 0x14, 0x01, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01};
    rtk_proto_frame_t view;
    uint8_t wire[sizeof(k_proto)];

    expect_frame(&rtk_proto_demux_decoder, "one", k_proto, sizeof(k_proto));
    expect_frame(&rtk_proto_demux_decoder, "empty", k_proto_empty, sizeof(k_proto_empty));
    expect_frame(&rtk_proto_demux_decoder, "max", k_proto_max, sizeof(k_proto_max));
    expect_reject(&rtk_proto_demux_decoder, "crc", b_k_proto, sizeof(b_k_proto));
    expect_reject_at(&rtk_proto_demux_decoder, "产品帧第 2 字节", k_bad_b, 2U);
    expect_reject_at(&rtk_proto_demux_decoder, "产品帧第 3 字节（与 Unicore 区分）", k_bad_c, 3U);
    expect_reject_at(&rtk_proto_demux_decoder, "产品帧头长字节", k_bad_hdr, 4U);
    expect_reject_at(&rtk_proto_demux_decoder, "产品帧 payload 长度", k_bad_len, 14U);
    CHECK(rtk_proto_demux_decoder.max_frame == RTK_PROTO_MAX_FRAME, "产品帧 max_frame 应等于 RTK_PROTO_MAX_FRAME");
    CHECK(rtk_proto_demux_decoder.proto_id == RTK_PROTO_DEMUX_ID, "产品帧 proto_id 应为 RTK_PROTO_DEMUX_ID");

    /* frame_view：字段、payload 指针、长度一致性；不重复校验 CRC */
    memset(&view, 0, sizeof(view));
    CHECK(rtk_proto_frame_view(k_proto, sizeof(k_proto), &view), "frame_view: 合法帧应返回 true");
    CHECK(view.msg_id == 0x0102U, "frame_view: msg_id");
    CHECK(view.sender == 0x00U && view.type == 0x00U, "frame_view: sender/type");
    CHECK(view.payload_len == 4U, "frame_view: payload_len");
    CHECK(view.payload == &k_proto[RTK_PROTO_HEADER_LEN], "frame_view: payload 指针应在 wire 内部");
    CHECK(memcmp(view.payload, "\x01\x02\x03\x04", 4U) == 0, "frame_view: payload 内容");

    CHECK(rtk_proto_frame_view(b_k_proto, sizeof(b_k_proto), &view), "frame_view: 不校验 CRC，坏 CRC 也返回 true");
    CHECK(!rtk_proto_frame_view(k_proto, sizeof(k_proto) - 1U, &view), "frame_view: 长度不一致应返回 false");
    CHECK(!rtk_proto_frame_view(k_bad_c, 24U, &view), "frame_view: 同步头错误应返回 false");
    CHECK(!rtk_proto_frame_view(NULL, sizeof(k_proto), &view), "frame_view: wire 为 NULL 应返回 false");
    CHECK(!rtk_proto_frame_view(k_proto, sizeof(k_proto), NULL), "frame_view: out 为 NULL 应返回 false");

    memcpy(wire, k_proto, sizeof(wire));
    wire[RTK_PROTO_OFS_HDR_LEN] = 0x15U;
    CHECK(!rtk_proto_frame_view(wire, sizeof(wire), &view), "frame_view: 头长字节错误应返回 false");
}

int main(void)
{
    test_rtcm3();
    test_nmea();
    test_ubx();
    test_unicore_bin();
    test_unicore_hash();
    test_proto();

    if (s_fail == 0) {
        printf("G2 self-check: ALL PASS\n");
        return 0;
    }
    printf("G2 self-check: %d FAILURE(S)\n", s_fail);
    return 1;
}
