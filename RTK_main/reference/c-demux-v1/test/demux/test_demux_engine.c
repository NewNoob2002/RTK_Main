// 通用字节流分流器引擎黑盒单元测试（ADR-0004 §3，自定义解码器测试状态机与边界）。
// Generated-by: pi + antigravity/gemini-3.8-flash，人工负责人：UNKNOWN（待人工审查签字）
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rtk_demux.h"
#include "unity.h"

#define TEST_BUF_CAP     (128U)
#define TEST_CANARY_LEN  (16U)
#define TEST_CANARY_BYTE (0xA5U)
#define MAX_EVENTS       (64U)
#define MAX_STREAM_CAP   (512U)

typedef enum {
    EVT_UNKNOWN = 1,
    EVT_FRAME = 2,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint8_t decoder_index;
    uint16_t proto_id;
    size_t len;
    uint8_t data[TEST_BUF_CAP];
} recorded_event_t;

typedef struct {
    rtk_demux_t *dmx;
    recorded_event_t events[MAX_EVENTS];
    size_t event_count;
    uint8_t unknown_stream[MAX_STREAM_CAP];
    size_t unknown_len;
    bool test_reentry_in_frame;
    bool test_reentry_in_unknown;
    rtk_demux_err_t reentry_feed_err;
    rtk_demux_err_t reentry_abort_err;
    rtk_demux_err_t reentry_init_err;
    const rtk_demux_config_t *reentry_cfg;
} test_sink_t;

static test_sink_t s_sink;
static uint8_t s_guarded_buf[TEST_BUF_CAP + TEST_CANARY_LEN];
static rtk_demux_t s_dmx;

/* -------------------------------------------------------------------------- */
/* 自定义测试解码器                                                             */
/* -------------------------------------------------------------------------- */

/* 1) 固定 4 字节帧：A1 <d1> <d2> <chk>，其中 chk == (A1 ^ d1 ^ d2) */
static bool dec_fixed4_first(uint8_t byte)
{
    return byte == 0xA1U;
}

static rtk_demux_step_t dec_fixed4_step(const uint8_t *frame, size_t len, size_t *need)
{
    if (len < 4U) {
        *need = 4U - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if ((uint8_t)(frame[0] ^ frame[1] ^ frame[2]) == frame[3]) {
        return RTK_DEMUX_STEP_FRAME;
    }
    return RTK_DEMUX_STEP_REJECT;
}

static const rtk_demux_decoder_t s_dec_fixed4 = {
    .name = "fixed4",
    .proto_id = 0x1001U,
    .max_frame = 4U,
    .first_byte = dec_fixed4_first,
    .step = dec_fixed4_step,
};

/* 2) 批量 spy 解码器：A2 <total_len> <payload...> <0xEE>；统计 step 调用次数 */
static uint32_t s_bulk_step_calls = 0U;

static bool dec_bulk_first(uint8_t byte)
{
    return byte == 0xA2U;
}

static rtk_demux_step_t dec_bulk_step(const uint8_t *frame, size_t len, size_t *need)
{
    size_t total = 0U;

    s_bulk_step_calls++;
    if (len < 2U) {
        *need = 2U - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    total = frame[1];
    if ((total < 3U) || (total > 64U)) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < total) {
        *need = total - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return (frame[total - 1U] == 0xEEU) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static const rtk_demux_decoder_t s_dec_bulk = {
    .name = "bulk64",
    .proto_id = 0x1002U,
    .max_frame = 64U,
    .first_byte = dec_bulk_first,
    .step = dec_bulk_step,
};

/* 3) 同首字节冲突组（均以 0xAA 开头）：
 *    - p0: 6 字节，要求 frame[5] == 0x55（前 5 字节不拒，凑满 6 字节后才可能拒，用于触发长前缀回放）
 *    - p1: 4 字节，要求 frame[1] == 0x22 && frame[3] == 0x44
 *    - p2: 3 字节，要求 frame[1] == 0x33 && frame[2] == 0x66
 */
static bool dec_aa_first(uint8_t byte)
{
    return byte == 0xAAU;
}

static rtk_demux_step_t dec_aa_p0_step(const uint8_t *frame, size_t len, size_t *need)
{
    if (len < 6U) {
        *need = 6U - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return (frame[5] == 0x55U) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static rtk_demux_step_t dec_aa_p1_step(const uint8_t *frame, size_t len, size_t *need)
{
    if (len >= 2U) {
        if (frame[1] != 0x22U) {
            return RTK_DEMUX_STEP_REJECT;
        }
    }
    if (len < 4U) {
        *need = 4U - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return (frame[3] == 0x44U) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static rtk_demux_step_t dec_aa_p2_step(const uint8_t *frame, size_t len, size_t *need)
{
    if ((len >= 2U) && (frame[1] != 0x33U)) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < 3U) {
        *need = 3U - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return (frame[2] == 0x66U) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static const rtk_demux_decoder_t s_dec_aa_p0 = {
    .name = "aa_p0",
    .proto_id = 0x1010U,
    .max_frame = 6U,
    .first_byte = dec_aa_first,
    .step = dec_aa_p0_step,
};

static const rtk_demux_decoder_t s_dec_aa_p1 = {
    .name = "aa_p1",
    .proto_id = 0x1011U,
    .max_frame = 4U,
    .first_byte = dec_aa_first,
    .step = dec_aa_p1_step,
};

static const rtk_demux_decoder_t s_dec_aa_p2 = {
    .name = "aa_p2",
    .proto_id = 0x1012U,
    .max_frame = 3U,
    .first_byte = dec_aa_first,
    .step = dec_aa_p2_step,
};

/* 4) 契约违规与溢出解码器（均以 0xBA 开头） */
static bool dec_ba_first(uint8_t byte)
{
    return byte == 0xBAU;
}

static rtk_demux_step_t dec_need_zero_step(const uint8_t *frame, size_t len, size_t *need)
{
    (void)frame;
    (void)len;
    *need = 0U; /* 违规：NEED_MORE 时 *need == 0 */
    return RTK_DEMUX_STEP_NEED_MORE;
}

static rtk_demux_step_t dec_exceed_max_step(const uint8_t *frame, size_t len, size_t *need)
{
    (void)frame;
    *need = 17U - len; /* max_frame == 16，17 超出 max_frame */
    return RTK_DEMUX_STEP_NEED_MORE;
}

static rtk_demux_step_t dec_overflow_buf_step(const uint8_t *frame, size_t len, size_t *need)
{
    (void)frame;
    (void)len;
    *need = 20U; /* 当 buf_size == 16 时，1 + 20 > 16 */
    return RTK_DEMUX_STEP_NEED_MORE;
}

static rtk_demux_step_t dec_overflow_sizemax_step(const uint8_t *frame, size_t len, size_t *need)
{
    (void)frame;
    (void)len;
    *need = SIZE_MAX; /* 测试无符号加法溢出 */
    return RTK_DEMUX_STEP_NEED_MORE;
}

static const rtk_demux_decoder_t s_dec_need_zero = {
    .name = "bad_need0",
    .proto_id = 0x1020U,
    .max_frame = 16U,
    .first_byte = dec_ba_first,
    .step = dec_need_zero_step,
};

static const rtk_demux_decoder_t s_dec_exceed_max = {
    .name = "bad_max",
    .proto_id = 0x1021U,
    .max_frame = 16U,
    .first_byte = dec_ba_first,
    .step = dec_exceed_max_step,
};

static const rtk_demux_decoder_t s_dec_overflow_buf = {
    .name = "bad_ovf",
    .proto_id = 0x1022U,
    .max_frame = 16U,
    .first_byte = dec_ba_first,
    .step = dec_overflow_buf_step,
};

static rtk_demux_step_t dec_ba_valid2_step(const uint8_t *frame, size_t len, size_t *need)
{
    if (len < 2U) {
        *need = 1U;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return (frame[1] == 0x77U) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static const rtk_demux_decoder_t s_dec_ba_valid2 = {
    .name = "ba_ok2",
    .proto_id = 0x1024U,
    .max_frame = 2U,
    .first_byte = dec_ba_first,
    .step = dec_ba_valid2_step,
};

static const rtk_demux_decoder_t s_dec_overflow_sizemax = {
    .name = "bad_smax",
    .proto_id = 0x1023U,
    .max_frame = 16U,
    .first_byte = dec_ba_first,
    .step = dec_overflow_sizemax_step,
};

/* 5) 8 个解码器满配测试（首字节 0xC0..0xC7，每帧 2 字节：0xCx <~0xCx>） */
static bool dec_slot_first(uint8_t byte, uint8_t tag)
{
    return byte == tag;
}

static rtk_demux_step_t dec_slot_step(const uint8_t *frame, size_t len, size_t *need)
{
    if (len < 2U) {
        *need = 1U;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return (frame[1] == (uint8_t)(~frame[0])) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

#define DEFINE_SLOT_DECODER(idx, tag_val)                                                                              \
    static bool dec_slot_##idx##_first(uint8_t b)                                                                      \
    {                                                                                                                  \
        return dec_slot_first(b, (tag_val));                                                                           \
    }                                                                                                                  \
    static const rtk_demux_decoder_t s_dec_slot_##idx = {                                                              \
        .name = "slot" #idx,                                                                                           \
        .proto_id = (uint16_t)(0x2000U + (idx)),                                                                       \
        .max_frame = 2U,                                                                                               \
        .first_byte = dec_slot_##idx##_first,                                                                          \
        .step = dec_slot_step,                                                                                         \
    }

DEFINE_SLOT_DECODER(0, 0xC0U);
DEFINE_SLOT_DECODER(1, 0xC1U);
DEFINE_SLOT_DECODER(2, 0xC2U);
DEFINE_SLOT_DECODER(3, 0xC3U);
DEFINE_SLOT_DECODER(4, 0xC4U);
DEFINE_SLOT_DECODER(5, 0xC5U);
DEFINE_SLOT_DECODER(6, 0xC6U);
DEFINE_SLOT_DECODER(7, 0xC7U);

/* -------------------------------------------------------------------------- */
/* 回调与辅助                                                                   */
/* -------------------------------------------------------------------------- */

static void on_frame_cb(void *user, const rtk_demux_decoder_t *decoder, uint8_t decoder_index, const uint8_t *frame,
                        size_t len)
{
    test_sink_t *sink = (test_sink_t *)user;
    static const uint8_t s_extra = 0x5AU;

    if (sink->test_reentry_in_frame && (sink->dmx != NULL)) {
        sink->reentry_feed_err = rtk_demux_feed(sink->dmx, &s_extra, 1U);
        sink->reentry_abort_err = rtk_demux_abort(sink->dmx);
        sink->reentry_init_err = rtk_demux_init(sink->dmx, sink->reentry_cfg);
    }

    if (sink->event_count < MAX_EVENTS) {
        recorded_event_t *ev = &sink->events[sink->event_count++];
        ev->kind = EVT_FRAME;
        ev->decoder_index = decoder_index;
        ev->proto_id = (decoder != NULL) ? decoder->proto_id : 0U;
        ev->len = len;
        if ((len <= sizeof(ev->data)) && (frame != NULL)) {
            memcpy(ev->data, frame, len);
        }
    }
}

static void on_unknown_cb(void *user, const uint8_t *bytes, size_t len)
{
    test_sink_t *sink = (test_sink_t *)user;
    static const uint8_t s_extra = 0x5AU;

    if (sink->test_reentry_in_unknown && (sink->dmx != NULL)) {
        sink->reentry_feed_err = rtk_demux_feed(sink->dmx, &s_extra, 1U);
        sink->reentry_abort_err = rtk_demux_abort(sink->dmx);
        sink->reentry_init_err = rtk_demux_init(sink->dmx, sink->reentry_cfg);
    }

    if (sink->event_count < MAX_EVENTS) {
        recorded_event_t *ev = &sink->events[sink->event_count++];
        ev->kind = EVT_UNKNOWN;
        ev->decoder_index = 0U;
        ev->proto_id = 0U;
        ev->len = len;
        if ((len <= sizeof(ev->data)) && (bytes != NULL)) {
            memcpy(ev->data, bytes, len);
        }
    }

    if ((sink->unknown_len + len <= sizeof(sink->unknown_stream)) && (bytes != NULL)) {
        memcpy(&sink->unknown_stream[sink->unknown_len], bytes, len);
    }
    sink->unknown_len += len;
}

static void reset_sink(void)
{
    memset(&s_sink, 0, sizeof(s_sink));
    s_sink.dmx = &s_dmx;
    memset(s_guarded_buf, TEST_CANARY_BYTE, sizeof(s_guarded_buf));
    s_bulk_step_calls = 0U;
}

static rtk_demux_config_t make_cfg(const rtk_demux_decoder_t *const *decs, uint8_t count, size_t buf_size)
{
    rtk_demux_config_t cfg = {
        .decoders = decs,
        .decoder_count = count,
        .buf = s_guarded_buf,
        .buf_size = buf_size,
        .on_frame = on_frame_cb,
        .on_unknown = on_unknown_cb,
        .user = &s_sink,
    };
    return cfg;
}

void setUp(void)
{
    reset_sink();
}

void tearDown(void) {}

/* -------------------------------------------------------------------------- */
/* 测试用例                                                                     */
/* -------------------------------------------------------------------------- */

static void test_init_validation_and_poison_on_fail(void)
{
    const rtk_demux_decoder_t *const decs_ok[] = {&s_dec_fixed4};
    const rtk_demux_decoder_t *const decs_null_item[] = {&s_dec_fixed4, NULL};
    rtk_demux_decoder_t bad_dec = s_dec_fixed4;
    const rtk_demux_decoder_t *const bad_list[] = {&bad_dec};
    rtk_demux_config_t cfg = make_cfg(decs_ok, 1U, TEST_BUF_CAP);
    static const uint8_t s_frame[] = {0xA1U, 0x10U, 0x20U, 0x91U};

    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(NULL, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, NULL));

    cfg.decoders = NULL;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    cfg = make_cfg(decs_ok, 0U, TEST_BUF_CAP);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    cfg = make_cfg(decs_ok, RTK_DEMUX_MAX_DECODERS + 1U, TEST_BUF_CAP);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    cfg = make_cfg(decs_ok, 1U, TEST_BUF_CAP);
    cfg.buf = NULL;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    cfg = make_cfg(decs_ok, 1U, 0U);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    cfg = make_cfg(decs_ok, 1U, s_dec_fixed4.max_frame - 1U);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    /* 后项解码器 max_frame 大于 buf_size 也必须拒绝 */
    {
        const rtk_demux_decoder_t *const decs_two[] = {&s_dec_fixed4, &s_dec_bulk};
        cfg = make_cfg(decs_two, 2U, s_dec_bulk.max_frame - 1U);
        TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));
    }

    cfg = make_cfg(decs_ok, 1U, TEST_BUF_CAP);
    cfg.on_frame = NULL;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    cfg = make_cfg(decs_null_item, 2U, TEST_BUF_CAP);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    /* 解码器字段非法 */
    cfg = make_cfg(bad_list, 1U, TEST_BUF_CAP);
    bad_dec = s_dec_fixed4;
    bad_dec.name = NULL;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    bad_dec = s_dec_fixed4;
    bad_dec.max_frame = 0U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    bad_dec = s_dec_fixed4;
    bad_dec.first_byte = NULL;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    bad_dec = s_dec_fixed4;
    bad_dec.step = NULL;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));

    /* 先成功 init，再故意用非法配置 re-init 失败后，实例应不可用直到再次成功 init */
    cfg = make_cfg(decs_ok, 1U, TEST_BUF_CAP);
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    cfg.decoder_count = 0U;
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_feed(&s_dmx, s_frame, sizeof(s_frame)));

    /* 重新有效 init 后恢复可用 */
    cfg = make_cfg(decs_ok, 1U, TEST_BUF_CAP);
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_frame, sizeof(s_frame)));
    TEST_ASSERT_EQUAL_UINT32(1U, rtk_demux_stats(&s_dmx)->decoder[0].frames);
}

static void test_feed_bad_args_and_zero_len(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_fixed4};
    rtk_demux_config_t cfg = make_cfg(decs, 1U, TEST_BUF_CAP);
    static const uint8_t s_dummy = 0x00U;

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_feed(NULL, &s_dummy, 1U));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_feed(&s_dmx, NULL, 1U));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_ARG, rtk_demux_abort(NULL));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, &s_dummy, 0U));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(0U, s_sink.event_count);
}

static void test_idle_unknown_and_null_unknown_cb(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_fixed4};
    rtk_demux_config_t cfg = make_cfg(decs, 1U, TEST_BUF_CAP);
    static const uint8_t s_stream[] = {0x11U, 0x22U, 0xA1U, 0x01U, 0x02U, 0xA2U, 0x33U, 0x44U};
    static const uint8_t s_unk[] = {0x11U, 0x22U, 0x33U, 0x44U};
    const rtk_demux_stats_t *st = NULL;

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_stream, sizeof(s_stream)));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));

    /* 流顺序：未知字节 {0x11,0x22} 在帧之前，{0x33,0x44} 在帧之后 */
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.event_count);
    TEST_ASSERT_EQUAL(EVT_UNKNOWN, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL(EVT_FRAME, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[1].len);
    TEST_ASSERT_EQUAL_HEX16(s_dec_fixed4.proto_id, s_sink.events[1].proto_id);
    TEST_ASSERT_EQUAL(EVT_UNKNOWN, s_sink.events[2].kind);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_unk), s_sink.unknown_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_unk, s_sink.unknown_stream, sizeof(s_unk));

    st = rtk_demux_stats(&s_dmx);
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(4U, st->decoder[0].bytes);
    TEST_ASSERT_EQUAL_UINT32(4U, st->unknown_bytes);

    /* on_unknown == NULL 时仅计数不崩溃 */
    reset_sink();
    cfg.on_unknown = NULL;
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_stream, sizeof(s_stream)));
    TEST_ASSERT_EQUAL_UINT32(4U, rtk_demux_stats(&s_dmx)->unknown_bytes);
    TEST_ASSERT_EQUAL_UINT32(1U, rtk_demux_stats(&s_dmx)->decoder[0].frames);
}

static void test_bulk_accumulation_and_split_chunks(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_bulk};
    rtk_demux_config_t cfg = make_cfg(decs, 1U, TEST_BUF_CAP);
    uint8_t frame[40];

    frame[0] = 0xA2U;
    frame[1] = 40U;
    for (size_t i = 2U; i < 39U; i++) {
        frame[i] = (uint8_t)i; /* i < 39，窄化安全 */
    }
    frame[39] = 0xEEU;

    /* 整块喂入：step 只应在 len=1, len=2, len=40 被调用 3 次 */
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_UINT32(3U, s_bulk_step_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count);
    TEST_ASSERT_EQUAL_UINT(sizeof(frame), s_sink.events[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame, s_sink.events[0].data, sizeof(frame));

    /* 分块喂入（10 + 10 + 20），step 依然只在凑满 need 时触发（共 3 次） */
    reset_sink();
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, frame, 10U));
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(0U, s_sink.event_count);
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, &frame[10], 10U));
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, &frame[20], 20U));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT32(3U, s_bulk_step_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count);
}

static void test_same_first_byte_priority_and_midbuffer_replay(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_aa_p0, &s_dec_aa_p1, &s_dec_aa_p2, &s_dec_fixed4};
    rtk_demux_config_t cfg = make_cfg(decs, 4U, TEST_BUF_CAP);
    /* 1) 命中 p0（6 字节）：AA 22 00 44 00 55（即使满足 p1 前缀，p0 优先且成功） */
    static const uint8_t s_match_p0[] = {0xAAU, 0x22U, 0x00U, 0x44U, 0x00U, 0x55U};
    /* 2) p0 收满 6 字节后在末尾拒（[5]!=0x55），回放给 p1 在 p=4 成帧，剩余 2 字节 {0x77, 0x88} 变未知 */
    static const uint8_t s_match_p1_unk[] = {0xAAU, 0x22U, 0x99U, 0x44U, 0x77U, 0x88U};
    /* 3a) p0 收满 6 字节拒，回放 p1 拒，再回放 p2 在 p=3 成帧，剩余 3 字节恰好拼后续 1 字节成 fixed4 帧！ */
    static const uint8_t s_match_p2_then_fixed4[] = {0xAAU, 0x33U, 0x66U, 0xA1U, 0x05U, 0x06U, 0xA2U};
    /* 3b) p2 成帧后，剩余 3 字节加后续 3 字节让 p0 拒后再回放成 p1 帧 + 2 字节未知 */
    static const uint8_t s_match_p2_then_p1[] = {0xAAU, 0x33U, 0x66U, 0xAAU, 0x22U, 0x99U, 0x44U, 0x77U, 0x88U};
    const rtk_demux_stats_t *st = NULL;

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_match_p0, sizeof(s_match_p0)));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count);
    TEST_ASSERT_EQUAL_UINT8(0U, s_sink.events[0].decoder_index);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->decoder[1].rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->resyncs);

    reset_sink();
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_match_p1_unk, sizeof(s_match_p1_unk)));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count);
    TEST_ASSERT_EQUAL(EVT_FRAME, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events[0].decoder_index);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_match_p1_unk, s_sink.events[0].data, 4U);
    TEST_ASSERT_EQUAL(EVT_UNKNOWN, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknown_len);
    TEST_ASSERT_EQUAL_HEX8(0x77U, s_sink.unknown_stream[0]);
    TEST_ASSERT_EQUAL_HEX8(0x88U, s_sink.unknown_stream[1]);
    st = rtk_demux_stats(&s_dmx);
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[1].frames);
    TEST_ASSERT_EQUAL_UINT32(0U, st->resyncs);

    /* 3a: p0 拒 -> p1 拒 -> p2 在 p=3 成帧 -> buf[3..6) 与第 7 字节拼成 fixed4 帧 */
    reset_sink();
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_match_p2_then_fixed4, sizeof(s_match_p2_then_fixed4)));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events[0].decoder_index);
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.events[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_match_p2_then_fixed4, s_sink.events[0].data, 3U);
    TEST_ASSERT_EQUAL_UINT8(3U, s_sink.events[1].decoder_index);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_match_p2_then_fixed4[3], s_sink.events[1].data, 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->unknown_bytes);

    /* 3b: p2 在 p=3 成帧 -> 后续再次 AA 冲突由 p0 拒后回放 p1 成帧并输出尾部未知字节 */
    reset_sink();
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_match_p2_then_p1, sizeof(s_match_p2_then_p1)));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.event_count);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events[0].decoder_index);
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.events[0].len);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events[1].decoder_index);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_match_p2_then_p1[3], s_sink.events[1].data, 4U);
    TEST_ASSERT_EQUAL(EVT_UNKNOWN, s_sink.events[2].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknown_len);
}

static void test_all_reject_resync_recovers_nested_frame(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_fixed4};
    rtk_demux_config_t cfg = make_cfg(decs, 1U, TEST_BUF_CAP);
    /* 假帧 A1 99 A1 05（校验和 0xA1^0x99^0xA1 = 0x99 != 0x05），其中第 2 个 A1 是真帧起点：
     * A1 05 06 (0xA1^0x05^0x06 = 0xA2) */
    static const uint8_t s_stream[] = {0xA1U, 0x99U, 0xA1U, 0x05U, 0x06U, 0xA2U};
    static const uint8_t s_exp_unk[] = {0xA1U, 0x99U};
    const rtk_demux_stats_t *st = NULL;

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_stream, sizeof(s_stream)));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count);
    TEST_ASSERT_EQUAL(EVT_UNKNOWN, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknown_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_exp_unk, s_sink.unknown_stream, sizeof(s_exp_unk));
    TEST_ASSERT_EQUAL(EVT_FRAME, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_stream[2], s_sink.events[1].data, 4U);

    st = rtk_demux_stats(&s_dmx);
    TEST_ASSERT_EQUAL_UINT32(1U, st->resyncs);
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(2U, st->unknown_bytes);
}

static void test_contract_violations_and_overflow(void)
{
    static const uint8_t s_trigger[] = {0xBAU, 0x01U};
    const rtk_demux_stats_t *st = NULL;

    /* 1) need == 0 契约违规（buf_size 充足） */
    {
        const rtk_demux_decoder_t *const decs[] = {&s_dec_need_zero};
        rtk_demux_config_t cfg = make_cfg(decs, 1U, TEST_BUF_CAP);

        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_trigger, sizeof(s_trigger)));
        st = rtk_demux_stats(&s_dmx);
        TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(0U, st->overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, st->resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, st->unknown_bytes);
    }

    /* 2) len + need > max_frame 契约违规（但 <= buf_size） */
    reset_sink();
    {
        const rtk_demux_decoder_t *const decs[] = {&s_dec_exceed_max};
        rtk_demux_config_t cfg = make_cfg(decs, 1U, TEST_BUF_CAP);

        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_trigger, sizeof(s_trigger)));
        st = rtk_demux_stats(&s_dmx);
        TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(0U, st->overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, st->resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, st->unknown_bytes);
    }

    /* 3) len + need > buf_size 触发 overflow（buf_size == 16，哨兵不得被踩） */
    reset_sink();
    {
        const rtk_demux_decoder_t *const decs[] = {&s_dec_overflow_buf};
        rtk_demux_config_t cfg = make_cfg(decs, 1U, 16U);

        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_trigger, sizeof(s_trigger)));
        st = rtk_demux_stats(&s_dmx);
        TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, st->overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, st->resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, st->unknown_bytes);
        for (size_t i = 16U; i < sizeof(s_guarded_buf); i++) {
            TEST_ASSERT_EQUAL_HEX8(TEST_CANARY_BYTE, s_guarded_buf[i]);
        }
    }

    /* 4) *need == SIZE_MAX 防整数回绕溢出 */
    reset_sink();
    {
        const rtk_demux_decoder_t *const decs[] = {&s_dec_overflow_sizemax};
        rtk_demux_config_t cfg = make_cfg(decs, 1U, 16U);

        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_trigger, sizeof(s_trigger)));
        st = rtk_demux_stats(&s_dmx);
        TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, st->overflows);
        TEST_ASSERT_EQUAL_UINT32(2U, st->unknown_bytes);
    }

    /* 5) 契约违规视为 REJECT，同首字节次优先级解码器仍可接管成帧 */
    reset_sink();
    {
        const rtk_demux_decoder_t *const decs[] = {&s_dec_need_zero, &s_dec_ba_valid2};
        rtk_demux_config_t cfg = make_cfg(decs, 2U, TEST_BUF_CAP);
        static const uint8_t s_ok[] = {0xBAU, 0x77U};

        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_ok, sizeof(s_ok)));
        st = rtk_demux_stats(&s_dmx);
        TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[1].frames);
        TEST_ASSERT_EQUAL_UINT32(0U, st->resyncs);
        TEST_ASSERT_EQUAL_UINT32(0U, st->unknown_bytes);
    }
}

static void test_reentry_from_callbacks(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_fixed4};
    rtk_demux_config_t cfg = make_cfg(decs, 1U, TEST_BUF_CAP);
    static const uint8_t s_frame[] = {0xA1U, 0x05U, 0x0AU, 0xAEU};
    static const uint8_t s_unk[] = {0x55U};

    /* on_frame 内重入 */
    s_sink.reentry_cfg = &cfg;
    s_sink.test_reentry_in_frame = true;
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_frame, sizeof(s_frame)));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_feed_err);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_abort_err);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_init_err);

    /* 退出回调后应恢复正常可调用 */
    s_sink.test_reentry_in_frame = false;
    s_sink.test_reentry_in_unknown = true;
    s_sink.reentry_feed_err = RTK_DEMUX_OK;
    s_sink.reentry_abort_err = RTK_DEMUX_OK;
    s_sink.reentry_init_err = RTK_DEMUX_OK;
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_unk, sizeof(s_unk)));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_feed_err);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_abort_err);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_init_err);

    /* abort 回调内重入（on_unknown 与 on_frame 两种回调） */
    s_sink.reentry_feed_err = RTK_DEMUX_OK;
    s_sink.reentry_abort_err = RTK_DEMUX_OK;
    s_sink.reentry_init_err = RTK_DEMUX_OK;
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_frame, 2U));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx));
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_feed_err);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_abort_err);
    TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_init_err);

    {
        const rtk_demux_decoder_t *const decs_conflict[] = {&s_dec_aa_p0, &s_dec_aa_p1};
        rtk_demux_config_t cfg_c = make_cfg(decs_conflict, 2U, TEST_BUF_CAP);
        static const uint8_t s_half_p1[] = {0xAAU, 0x22U, 0x99U, 0x44U};

        s_sink.test_reentry_in_unknown = false;
        s_sink.test_reentry_in_frame = true;
        s_sink.reentry_cfg = &cfg_c;
        s_sink.reentry_feed_err = RTK_DEMUX_OK;
        s_sink.reentry_abort_err = RTK_DEMUX_OK;
        s_sink.reentry_init_err = RTK_DEMUX_OK;
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg_c));
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_half_p1, sizeof(s_half_p1)));
        TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx));
        TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_feed_err);
        TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_abort_err);
        TEST_ASSERT_EQUAL(RTK_DEMUX_ERR_REENTRY, s_sink.reentry_init_err);
        TEST_ASSERT_EQUAL_UINT32(1U, rtk_demux_stats(&s_dmx)->decoder[1].frames);
    }

    /* 结束后仍可用 */
    s_sink.test_reentry_in_frame = false;
    s_sink.test_reentry_in_unknown = false;
    s_sink.reentry_cfg = &cfg;
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_frame, sizeof(s_frame)));
    TEST_ASSERT_EQUAL_UINT32(1U, rtk_demux_stats(&s_dmx)->decoder[0].frames);
}

static void test_abort_busy_and_recovery(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_aa_p0, &s_dec_aa_p1, &s_dec_fixed4};
    rtk_demux_config_t cfg = make_cfg(decs, 3U, TEST_BUF_CAP);
    /* 半帧内嵌真帧（情况 A）：p0 等第 6 字节，缓冲了 AA 99 A1 01 02 A2（6 字节？先只喂 5 字节无 AA p1），
     * 这里用 AA 99 A1 01 02 A2 改写：p0 需要 6 字节，喂前 5 字节 AA 99 A1 01 02 实际上差 1 字节 A2 不在其中；
     * 换成：喂 AA A1 01 02 A2（5 字节 < 6），p0 在等第 6 字节；abort 后 AA 拒，重扫发现 A1 01 02 A2 完整帧！ */
    static const uint8_t s_nested_in_half[] = {0xAAU, 0xA1U, 0x01U, 0x02U, 0xA2U};
    /* 情况 B：同首字节回退在 abort 期间生效：喂 AA 22 99 44 77（5 字节 < 6），p0 等第 6 字节；
     * abort 时 p0 拒，同起点回放 p1 恢复 4 字节帧，剩余 0x77 输出为未知 */
    static const uint8_t s_replay_in_abort[] = {0xAAU, 0x22U, 0x99U, 0x44U, 0x77U};
    static const uint8_t s_partial[] = {0xA1U, 0x07U};

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx));
    TEST_ASSERT_EQUAL_UINT32(0U, rtk_demux_stats(&s_dmx)->aborts);

    /* 纯半帧 abort */
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_partial, sizeof(s_partial)));
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT32(1U, rtk_demux_stats(&s_dmx)->aborts);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_partial), s_sink.unknown_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_partial, s_sink.unknown_stream, sizeof(s_partial));

    /* 半帧中嵌套了另一解码器完整帧 */
    reset_sink();
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_nested_in_half, sizeof(s_nested_in_half)));
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count);
    TEST_ASSERT_EQUAL(EVT_UNKNOWN, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.unknown_len);
    TEST_ASSERT_EQUAL_HEX8(0xAAU, s_sink.unknown_stream[0]);
    TEST_ASSERT_EQUAL(EVT_FRAME, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events[1].decoder_index);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&s_nested_in_half[1], s_sink.events[1].data, 4U);

    /* 同首字节候选在 abort 时回放成帧 */
    reset_sink();
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_replay_in_abort, sizeof(s_replay_in_abort)));
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_abort(&s_dmx));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count);
    TEST_ASSERT_EQUAL(EVT_FRAME, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events[0].decoder_index);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_replay_in_abort, s_sink.events[0].data, 4U);
    TEST_ASSERT_EQUAL(EVT_UNKNOWN, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.unknown_len);
    TEST_ASSERT_EQUAL_HEX8(0x77U, s_sink.unknown_stream[0]);
}

static void test_reset_stats_midframe_and_eight_decoders(void)
{
    const rtk_demux_decoder_t *const decs[RTK_DEMUX_MAX_DECODERS] = {
        &s_dec_slot_0, &s_dec_slot_1, &s_dec_slot_2, &s_dec_slot_3,
        &s_dec_slot_4, &s_dec_slot_5, &s_dec_slot_6, &s_dec_slot_7,
    };
    rtk_demux_config_t cfg = make_cfg(decs, RTK_DEMUX_MAX_DECODERS, TEST_BUF_CAP);
    static const uint8_t s_f0[] = {0xC0U, 0x3FU};
    static const uint8_t s_f7[] = {0xC7U, 0x38U};
    static const uint8_t s_bad[] = {0xC3U, 0x00U};
    const rtk_demux_stats_t *st = NULL;

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg));
    st = rtk_demux_stats(&s_dmx);
    TEST_ASSERT_NOT_NULL(st);

    /* 先产生若干统计 */
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_f0, sizeof(s_f0)));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_bad, sizeof(s_bad)));
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[3].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, st->resyncs);
    TEST_ASSERT_EQUAL_UINT32(2U, st->unknown_bytes);

    /* 喂入 slot7 的第 1 字节，处于半帧时 reset_stats 必须保留解析状态 */
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, &s_f7[0], 1U));
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    rtk_demux_reset_stats(&s_dmx);
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT32(0U, st->decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(0U, st->decoder[3].rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, st->resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, st->unknown_bytes);

    /* 喂入第 2 字节完成 slot7 帧 */
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, &s_f7[1], 1U));
    TEST_ASSERT_FALSE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_EQUAL_UINT32(1U, st->decoder[7].frames);
    TEST_ASSERT_EQUAL_UINT32(2U, st->decoder[7].bytes);
    TEST_ASSERT_EQUAL_UINT8(7U, s_sink.events[s_sink.event_count - 1U].decoder_index);
}

static void test_two_instances_independent(void)
{
    const rtk_demux_decoder_t *const decs[] = {&s_dec_fixed4};
    rtk_demux_t dmx_b;
    uint8_t buf_b[TEST_BUF_CAP];
    test_sink_t sink_b;
    rtk_demux_config_t cfg_a = make_cfg(decs, 1U, TEST_BUF_CAP);
    rtk_demux_config_t cfg_b = {
        .decoders = decs,
        .decoder_count = 1U,
        .buf = buf_b,
        .buf_size = sizeof(buf_b),
        .on_frame = on_frame_cb,
        .on_unknown = on_unknown_cb,
        .user = &sink_b,
    };
    static const uint8_t s_fa[] = {0xA1U, 0x01U, 0x02U, 0xA2U};
    static const uint8_t s_fb[] = {0xA1U, 0x10U, 0x20U, 0x91U};

    memset(&sink_b, 0, sizeof(sink_b));
    sink_b.dmx = &dmx_b;
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&s_dmx, &cfg_a));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_init(&dmx_b, &cfg_b));

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, s_fa, 2U));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&dmx_b, s_fb, 2U));
    TEST_ASSERT_TRUE(rtk_demux_busy(&s_dmx));
    TEST_ASSERT_TRUE(rtk_demux_busy(&dmx_b));

    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&s_dmx, &s_fa[2], 2U));
    TEST_ASSERT_EQUAL(RTK_DEMUX_OK, rtk_demux_feed(&dmx_b, &s_fb[2], 2U));

    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count);
    TEST_ASSERT_EQUAL_UINT(1U, sink_b.event_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_fa, s_sink.events[0].data, 4U);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_fb, sink_b.events[0].data, 4U);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_validation_and_poison_on_fail);
    RUN_TEST(test_feed_bad_args_and_zero_len);
    RUN_TEST(test_idle_unknown_and_null_unknown_cb);
    RUN_TEST(test_bulk_accumulation_and_split_chunks);
    RUN_TEST(test_same_first_byte_priority_and_midbuffer_replay);
    RUN_TEST(test_all_reject_resync_recovers_nested_frame);
    RUN_TEST(test_contract_violations_and_overflow);
    RUN_TEST(test_reentry_from_callbacks);
    RUN_TEST(test_abort_busy_and_recovery);
    RUN_TEST(test_reset_stats_midframe_and_eight_decoders);
    RUN_TEST(test_two_instances_independent);
    return UNITY_END();
}
