// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
// 冻结契约缺口复现：init 未规定新实例的初始表示，却要求先识别同实例回调重入。
#include "rtk_demux.h"

#include <string.h>

static bool first(uint8_t byte)
{
    return byte == 1U;
}

static rtk_demux_step_t step(const uint8_t *frame, size_t len, size_t *need)
{
    (void)frame;
    (void)len;
    (void)need;
    return RTK_DEMUX_STEP_FRAME;
}

static void on_frame(void *user, const rtk_demux_decoder_t *decoder, uint8_t idx, const uint8_t *frame, size_t len)
{
    (void)user;
    (void)decoder;
    (void)idx;
    (void)frame;
    (void)len;
}

int main(void)
{
    rtk_demux_t dmx;
    uint8_t buf[1] = {0U};
    const rtk_demux_decoder_t decoder = {"probe", 1U, 1U, first, step};
    const rtk_demux_decoder_t *const decoders[] = {&decoder};
    const rtk_demux_config_t cfg = {decoders, 1U, buf, sizeof(buf), on_frame, NULL, NULL};
    memset(&dmx, 0xA5, sizeof(dmx));
    return rtk_demux_init(&dmx, &cfg) == RTK_DEMUX_OK ? 0 : 1;
}
