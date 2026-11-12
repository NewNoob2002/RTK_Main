// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
// G1 本地自测，不属于交付接口；整流扫描 oracle 独立于引擎的缓冲回放。
#include "rtk_demux.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    rtk_demux_t *dmx;
    const uint8_t *input;
    size_t size;
    size_t used;
    uint8_t tags[1024];
    size_t frame_calls;
    size_t unknown_calls;
    bool reentry;
} capture_t;

static size_t s_step_calls;
static size_t s_first_calls;
static size_t s_bad_need;
static uint32_t s_seed = 0x17C0FFEEU;

static bool first_bang(uint8_t byte)
{
    ++s_first_calls;
    return byte == '!';
}

static bool first_question(uint8_t byte)
{
    ++s_first_calls;
    return byte == '?';
}

static bool first_star(uint8_t byte)
{
    ++s_first_calls;
    return byte == '*';
}

static rtk_demux_step_t long_step(const uint8_t *frame, size_t len, size_t *need)
{
    ++s_step_calls;
    if (len < 6U) {
        *need = 6U - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return frame[5] == 'T' ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static rtk_demux_step_t short_step(const uint8_t *frame, size_t len, size_t *need)
{
    ++s_step_calls;
    if (len == 1U) {
        *need = 1U;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return frame[1] == 'q' ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static rtk_demux_step_t question_step(const uint8_t *frame, size_t len, size_t *need)
{
    ++s_step_calls;
    if (len == 1U) {
        *need = 1U;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[1] != 'x') {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len == 2U) {
        *need = 2U;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    return frame[3] == 'Z' ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static rtk_demux_step_t star_step(const uint8_t *frame, size_t len, size_t *need)
{
    (void)frame;
    (void)len;
    (void)need;
    ++s_step_calls;
    return RTK_DEMUX_STEP_FRAME;
}

static rtk_demux_step_t bad_step(const uint8_t *frame, size_t len, size_t *need)
{
    (void)frame;
    (void)len;
    *need = s_bad_need;
    return RTK_DEMUX_STEP_NEED_MORE;
}

static const rtk_demux_decoder_t s_long = {"long", 1U, 6U, first_bang, long_step};
static const rtk_demux_decoder_t s_short = {"short", 2U, 2U, first_bang, short_step};
static const rtk_demux_decoder_t s_question = {"question", 3U, 4U, first_question, question_step};
static const rtk_demux_decoder_t s_star = {"star", 4U, 1U, first_star, star_step};
static const rtk_demux_decoder_t *const s_decoders[] = {&s_long, &s_short, &s_question, &s_star};

static void check_reentry(capture_t *capture)
{
    if (capture->reentry) {
        assert(rtk_demux_feed(capture->dmx, NULL, 0U) == RTK_DEMUX_ERR_REENTRY);
        assert(rtk_demux_abort(capture->dmx) == RTK_DEMUX_ERR_REENTRY);
        assert(rtk_demux_init(capture->dmx, NULL) == RTK_DEMUX_ERR_REENTRY);
    }
}

static void capture_bytes(capture_t *capture, const uint8_t *bytes, size_t len, uint8_t tag)
{
    assert(len != 0U);
    assert((capture->used + len) <= capture->size);
    assert(memcmp(bytes, capture->input + capture->used, len) == 0);
    memset(capture->tags + capture->used, tag, len);
    capture->used += len;
    check_reentry(capture);
    // 重入拒绝必须保持回调指针的内容有效。
    assert(memcmp(bytes, capture->input + capture->used - len, len) == 0);
}

static void on_frame(void *user, const rtk_demux_decoder_t *decoder, uint8_t idx, const uint8_t *frame, size_t len)
{
    capture_t *capture = user;
    assert(idx < capture->dmx->cfg.decoder_count);
    assert(capture->dmx->cfg.decoders[idx] == decoder);
    assert(decoder->proto_id <= UINT8_MAX);
    // 自测协议编号范围为 1..4。
    capture_bytes(capture, frame, len, (uint8_t)decoder->proto_id);
    ++capture->frame_calls;
}

static void on_unknown(void *user, const uint8_t *bytes, size_t len)
{
    capture_t *capture = user;
    capture_bytes(capture, bytes, len, 0U);
    ++capture->unknown_calls;
}

static rtk_demux_config_t config_for(capture_t *capture, uint8_t *buf, size_t size)
{
    const rtk_demux_config_t cfg = {s_decoders, 4U, buf, size, on_frame, on_unknown, capture};
    return cfg;
}

static uint32_t random_word(void)
{
    s_seed ^= s_seed << 13U;
    s_seed ^= s_seed >> 17U;
    s_seed ^= s_seed << 5U;
    return s_seed;
}

// 整个输入已知时，逐起点、逐候选直接判定；不足的候选等价于最终 abort，随后尝试下一候选。
static void oracle(const uint8_t *input, size_t size, uint8_t *tags)
{
    memset(tags, 0, size);
    size_t pos = 0U;
    while (pos < size) {
        size_t consumed = 1U;
        for (size_t idx = 0U; idx < 4U; ++idx) {
            const rtk_demux_decoder_t *decoder = s_decoders[idx];
            if (!decoder->first_byte(input[pos])) {
                continue;
            }
            size_t prefix = 1U;
            while (prefix <= (size - pos)) {
                size_t need = 0U;
                const rtk_demux_step_t step = decoder->step(input + pos, prefix, &need);
                if (step == RTK_DEMUX_STEP_FRAME) {
                    consumed = prefix;
                    // 自测协议编号范围为 1..4。
                    memset(tags + pos, (uint8_t)decoder->proto_id, prefix);
                    break;
                }
                if (step == RTK_DEMUX_STEP_REJECT) {
                    break;
                }
                prefix += need;
            }
            if (tags[pos] != 0U) {
                break;
            }
        }
        pos += consumed;
    }
}

static void run_stream(const uint8_t *input, size_t size, const uint8_t *expected, size_t mode)
{
    rtk_demux_t dmx = {0};
    uint8_t guarded[10] = {0xC1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0xD2U};
    capture_t capture = {.dmx = &dmx, .input = input, .size = size, .reentry = true};
    const rtk_demux_config_t cfg = config_for(&capture, guarded + 1U, 8U);
    assert(rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_OK);
    s_first_calls = 0U;
    for (size_t pos = 0U; pos < size;) {
        size_t count = mode == 0U ? size : (mode == 1U ? 1U : (random_word() % 17U) + 1U);
        if (count > (size - pos)) {
            count = size - pos;
        }
        assert(rtk_demux_feed(&dmx, NULL, 0U) == RTK_DEMUX_OK);
        assert(rtk_demux_feed(&dmx, input + pos, count) == RTK_DEMUX_OK);
        pos += count;
    }
    assert(rtk_demux_abort(&dmx) == RTK_DEMUX_OK);
    assert(!rtk_demux_busy(&dmx));
    assert(capture.used == size);
    assert(memcmp(capture.tags, expected, size) == 0);
    assert(guarded[0] == 0xC1U);
    assert(guarded[9] == 0xD2U);
    assert(s_first_calls == 0U);
    size_t total = dmx.stats.unknown_bytes;
    for (size_t idx = 0U; idx < 4U; ++idx) {
        total += dmx.stats.decoder[idx].bytes;
    }
    assert(total == size);
}

static void check_api(void)
{
    rtk_demux_t dmx = {0};
    uint8_t buf[8] = {0U};
    const uint8_t input[] = {'a', 'b', '*', '!', 'q'};
    capture_t capture = {.dmx = &dmx, .input = input, .size = sizeof(input), .reentry = true};
    rtk_demux_config_t cfg = config_for(&capture, buf, sizeof(buf));
    assert(rtk_demux_init(NULL, &cfg) == RTK_DEMUX_ERR_ARG);
    assert(rtk_demux_init(&dmx, NULL) == RTK_DEMUX_ERR_ARG);
    assert(rtk_demux_feed(&dmx, input, sizeof(input)) == RTK_DEMUX_ERR_ARG);
    assert(rtk_demux_abort(&dmx) == RTK_DEMUX_ERR_ARG);
    assert(rtk_demux_feed(NULL, NULL, 0U) == RTK_DEMUX_ERR_ARG);
    assert(rtk_demux_abort(NULL) == RTK_DEMUX_ERR_ARG);
    assert(!rtk_demux_busy(NULL));
    assert(rtk_demux_stats(NULL) == NULL);
    rtk_demux_reset_stats(NULL);
    cfg.decoder_count = 0U;
    assert(rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_ERR_ARG);
    cfg.decoder_count = 9U;
    assert(rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_ERR_ARG);
    cfg.decoder_count = 4U;
    cfg.buf_size = 5U;
    assert(rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_ERR_ARG);
    cfg.buf_size = sizeof(buf);
    s_first_calls = 0U;
    assert(rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_OK);
    assert(s_first_calls == (4U * 256U));
    assert(rtk_demux_feed(&dmx, NULL, 1U) == RTK_DEMUX_ERR_ARG);
    assert(rtk_demux_feed(&dmx, input, sizeof(input)) == RTK_DEMUX_OK);
    assert(capture.unknown_calls == 1U);
    assert(capture.frame_calls == 1U);
    assert(rtk_demux_busy(&dmx));
    assert(rtk_demux_abort(&dmx) == RTK_DEMUX_OK);
    assert(capture.frame_calls == 2U);
    assert(dmx.stats.aborts == 1U);
    assert(dmx.stats.decoder[0].rejects == 1U);
    assert(dmx.stats.decoder[1].frames == 1U);
    assert(dmx.stats.decoder[3].frames == 1U);
    assert(dmx.stats.unknown_bytes == 2U);
    assert(rtk_demux_abort(&dmx) == RTK_DEMUX_OK);
    assert(dmx.stats.aborts == 1U);
    assert(rtk_demux_init(&dmx, &dmx.cfg) == RTK_DEMUX_OK);
    capture.used = 0U;
    assert(rtk_demux_feed(&dmx, input, sizeof(input)) == RTK_DEMUX_OK);
    rtk_demux_reset_stats(&dmx);
    assert(rtk_demux_busy(&dmx));
    assert(dmx.stats.unknown_bytes == 0U);
    assert(rtk_demux_abort(&dmx) == RTK_DEMUX_OK);
}

static void check_bad_need(void)
{
    rtk_demux_t dmx = {0};
    uint8_t buf[8] = {0U};
    const uint8_t input[] = {'!'};
    capture_t capture = {.dmx = &dmx, .input = input, .size = sizeof(input)};
    const rtk_demux_decoder_t bad = {"bad", 1U, 2U, first_bang, bad_step};
    const rtk_demux_decoder_t *const decoders[] = {&bad};
    rtk_demux_config_t cfg = config_for(&capture, buf, sizeof(buf));
    cfg.decoders = decoders;
    cfg.decoder_count = 1U;
    const size_t needs[] = {0U, 2U, 8U, SIZE_MAX};
    for (size_t idx = 0U; idx < 4U; ++idx) {
        s_bad_need = needs[idx];
        capture.used = 0U;
        assert(rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_OK);
        assert(rtk_demux_feed(&dmx, input, sizeof(input)) == RTK_DEMUX_OK);
        assert(!rtk_demux_busy(&dmx));
        assert(dmx.stats.decoder[0].rejects == 1U);
        assert(dmx.stats.resyncs == 1U);
        assert(dmx.stats.unknown_bytes == 1U);
        assert(dmx.stats.overflows == (idx >= 2U ? 1U : 0U));
    }
}

static void check_batching(void)
{
    rtk_demux_t dmx = {0};
    uint8_t buf[8] = {0U};
    const uint8_t input[] = {'!', 'q', '*', '*', '*', 'T'};
    capture_t capture = {.dmx = &dmx, .input = input, .size = sizeof(input)};
    const rtk_demux_config_t cfg = config_for(&capture, buf, sizeof(buf));
    assert(rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_OK);
    s_step_calls = 0U;
    for (size_t idx = 0U; idx < sizeof(input); ++idx) {
        assert(rtk_demux_feed(&dmx, input + idx, 1U) == RTK_DEMUX_OK);
    }
    assert(s_step_calls == 2U);
    assert(capture.frame_calls == 1U);
}

int main(void)
{
    check_api();
    check_bad_need();
    check_batching();
    const uint8_t replay[] = {'!', 'q', '*', '?', 'x', 'a', 'Z', '!', 'q', '?', '!', 'q', '*', '?'};
    uint8_t expected[1024] = {0U};
    oracle(replay, sizeof(replay), expected);
    for (size_t mode = 0U; mode < 3U; ++mode) {
        run_stream(replay, sizeof(replay), expected, mode);
    }
    const uint8_t alphabet[] = {'!', 'q', '?', 'x', 'Z', '*', 'T', 0U, 'a', 'b'};
    uint8_t input[1024] = {0U};
    for (size_t trial = 0U; trial < 1000U; ++trial) {
        const size_t size = random_word() % sizeof(input);
        for (size_t idx = 0U; idx < size; ++idx) {
            input[idx] = alphabet[random_word() % sizeof(alphabet)];
        }
        oracle(input, size, expected);
        run_stream(input, size, expected, 0U);
        run_stream(input, size, expected, 1U);
        run_stream(input, size, expected, 2U);
    }
    puts("G1 selftest PASS: API, need violations, replay, batching, reentry, 1000 streams x 3 chunk modes");
    return 0;
}
