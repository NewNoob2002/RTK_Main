// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
// 平台无关的流分流引擎；缓冲区回放与输入累积共用调用方提供的存储。
#include "rtk_demux.h"

#include <string.h>

static bool s_config_valid(const rtk_demux_config_t *cfg)
{
    if ((cfg == NULL) || (cfg->decoders == NULL) || (cfg->buf == NULL) || (cfg->on_frame == NULL) ||
        (cfg->decoder_count == 0U) || (cfg->decoder_count > RTK_DEMUX_MAX_DECODERS)) {
        return false;
    }
    for (uint8_t idx = 0U; idx < cfg->decoder_count; ++idx) {
        const rtk_demux_decoder_t *decoder = cfg->decoders[idx];
        if ((decoder == NULL) || (decoder->name == NULL) || (decoder->first_byte == NULL) || (decoder->step == NULL) ||
            (decoder->max_frame == 0U) || (decoder->max_frame > cfg->buf_size)) {
            return false;
        }
    }
    return true;
}

static uint8_t s_decoder_bit(uint8_t idx)
{
    // idx < 8，单个位最多为 0x80，可安全收窄到 uint8_t。
    return (uint8_t)(1U << idx);
}

static uint8_t s_next_decoder(const rtk_demux_t *dmx, uint8_t first)
{
    for (uint8_t idx = 0U; idx < dmx->cfg.decoder_count; ++idx) {
        const uint8_t bit = s_decoder_bit(idx);
        if (((dmx->first_mask[first] & bit) != 0U) && ((dmx->tried_mask & bit) == 0U)) {
            return idx;
        }
    }
    return RTK_DEMUX_MAX_DECODERS;
}

static void s_idle(rtk_demux_t *dmx)
{
    dmx->active = RTK_DEMUX_MAX_DECODERS;
    dmx->tried_mask = 0U;
    dmx->need = 0U;
}

static void s_unknown(rtk_demux_t *dmx, const uint8_t *bytes, size_t len)
{
    if (len != 0U) {
        // 统计按 uint32_t 模 2^32 累加；即使一次输入大于 UINT32_MAX，低 32 位仍正确。
        dmx->stats.unknown_bytes += (uint32_t)len;
        if (dmx->cfg.on_unknown != NULL) {
            dmx->in_callback = true;
            dmx->cfg.on_unknown(dmx->cfg.user, bytes, len);
            dmx->in_callback = false;
        }
    }
}

static void s_frame(rtk_demux_t *dmx, const uint8_t *frame, size_t len)
{
    rtk_demux_decoder_stats_t *stats = &dmx->stats.decoder[dmx->active];
    ++stats->frames;
    // len <= max_frame <= UINT16_MAX，转换到 uint32_t 无损。
    stats->bytes += (uint32_t)len;
    dmx->in_callback = true;
    dmx->cfg.on_frame(dmx->cfg.user, dmx->cfg.decoders[dmx->active], dmx->active, frame, len);
    dmx->in_callback = false;
}

static void s_consume(rtk_demux_t *dmx, size_t count)
{
    dmx->len -= count;
    if (dmx->len != 0U) {
        memmove(dmx->cfg.buf, dmx->cfg.buf + count, dmx->len);
    }
}

// start 之前是待合并输出的未知字节；prefix 只按 step 给出的 need 递增，禁止跳过回放中的帧尾。
// 返回时缓冲区只保留一个待补全的候选，need 为尚缺字节数；aborting 时所有半帧也进入拒绝路径。
static void s_replay(rtk_demux_t *dmx, bool aborting)
{
    size_t start = 0U;
    size_t prefix = dmx->len;
    bool reject_current = aborting && (dmx->active != RTK_DEMUX_MAX_DECODERS);
    while (start < dmx->len) {
        if (dmx->active >= RTK_DEMUX_MAX_DECODERS) {
            while ((start < dmx->len) && (dmx->first_mask[dmx->cfg.buf[start]] == 0U)) {
                ++start;
            }
            if (start == dmx->len) {
                break;
            }
            dmx->tried_mask = 0U;
            dmx->active = s_next_decoder(dmx, dmx->cfg.buf[start]);
            if (dmx->active >= RTK_DEMUX_MAX_DECODERS) {
                s_idle(dmx);
                ++start;
                continue;
            }
            prefix = 1U;
        }

        const rtk_demux_decoder_t *decoder = dmx->cfg.decoders[dmx->active];
        size_t need = 0U;
        const rtk_demux_step_t step =
            reject_current ? RTK_DEMUX_STEP_REJECT : decoder->step(dmx->cfg.buf + start, prefix, &need);
        reject_current = false;
        if (step == RTK_DEMUX_STEP_FRAME) {
            // 两次回调均须结束后才能移动 buf，保证未知字节与帧的内容、顺序稳定。
            s_unknown(dmx, dmx->cfg.buf, start);
            s_frame(dmx, dmx->cfg.buf + start, prefix);
            s_consume(dmx, start + prefix);
            s_idle(dmx);
            start = 0U;
        } else {
            if (step == RTK_DEMUX_STEP_NEED_MORE) {
                // 用减法检查容量，避免恶意 need == SIZE_MAX 时加法回绕。
                const bool overflow = need > (dmx->cfg.buf_size - prefix);
                if (overflow) {
                    ++dmx->stats.overflows;
                }
                if ((need != 0U) && (need <= (decoder->max_frame - prefix)) && !overflow) {
                    const size_t remaining = dmx->len - start - prefix;
                    if (need <= remaining) {
                        prefix += need;
                        continue;
                    }
                    if (!aborting) {
                        dmx->need = need - remaining;
                        s_unknown(dmx, dmx->cfg.buf, start);
                        s_consume(dmx, start);
                        return;
                    }
                }
            }

            // 包括未知 step 枚举值及 NEED_MORE 契约违规，均使候选集合严格缩小。
            ++dmx->stats.decoder[dmx->active].rejects;
            dmx->tried_mask |= s_decoder_bit(dmx->active);
            dmx->active = s_next_decoder(dmx, dmx->cfg.buf[start]);
            if (dmx->active >= RTK_DEMUX_MAX_DECODERS) {
                s_idle(dmx);
                ++dmx->stats.resyncs;
                ++start;
            } else {
                prefix = 1U;
            }
        }
    }
    s_unknown(dmx, dmx->cfg.buf, dmx->len);
    dmx->len = 0U;
    s_idle(dmx);
}

// 线程安全：同一实例须由调用方串行访问；ISR 安全，无阻塞，first_byte 必须同样满足调用环境约束。
rtk_demux_err_t rtk_demux_init(rtk_demux_t *dmx, const rtk_demux_config_t *cfg)
{
    if (dmx == NULL) {
        return RTK_DEMUX_ERR_ARG;
    }
    if (dmx->in_callback) {
        return RTK_DEMUX_ERR_REENTRY;
    }
    if (!s_config_valid(cfg)) {
        memset(dmx, 0, sizeof(*dmx));
        s_idle(dmx);
        return RTK_DEMUX_ERR_ARG;
    }

    // 支持使用 &dmx->cfg 重新初始化，清空实例前先保留配置。
    const rtk_demux_config_t config = *cfg;
    memset(dmx, 0, sizeof(*dmx));
    dmx->cfg = config;
    s_idle(dmx);
    for (uint8_t idx = 0U; idx < config.decoder_count; ++idx) {
        const uint8_t bit = s_decoder_bit(idx);
        for (size_t byte = 0U; byte < sizeof(dmx->first_mask); ++byte) {
            // 循环范围为 0..255，转换到 uint8_t 无损。
            if (config.decoders[idx]->first_byte((uint8_t)byte)) {
                dmx->first_mask[byte] |= bit;
            }
        }
    }
    return RTK_DEMUX_OK;
}

// 线程安全：同一实例须串行访问；ISR 安全取决于同步执行的用户回调；回调内禁止重入。
rtk_demux_err_t rtk_demux_feed(rtk_demux_t *dmx, const uint8_t *data, size_t len)
{
    if (dmx == NULL) {
        return RTK_DEMUX_ERR_ARG;
    }
    if (dmx->in_callback) {
        return RTK_DEMUX_ERR_REENTRY;
    }
    if ((dmx->cfg.on_frame == NULL) || ((data == NULL) && (len != 0U))) {
        return RTK_DEMUX_ERR_ARG;
    }

    size_t offset = 0U;
    while (offset < len) {
        if (dmx->active == RTK_DEMUX_MAX_DECODERS) {
            const size_t begin = offset;
            while ((offset < len) && (dmx->first_mask[data[offset]] == 0U)) {
                ++offset;
            }
            s_unknown(dmx, data + begin, offset - begin);
            if (offset == len) {
                break;
            }
            dmx->cfg.buf[0] = data[offset];
            dmx->len = 1U;
            ++offset;
            s_replay(dmx, false);
        } else {
            size_t count = len - offset;
            if (count > dmx->need) {
                count = dmx->need;
            }
            memcpy(dmx->cfg.buf + dmx->len, data + offset, count);
            dmx->len += count;
            dmx->need -= count;
            offset += count;
            if (dmx->need == 0U) {
                s_replay(dmx, false);
            }
        }
    }
    return RTK_DEMUX_OK;
}

// 线程安全：同一实例须串行访问；ISR 安全取决于用户回调；拒绝半帧后仍按流顺序回放。
rtk_demux_err_t rtk_demux_abort(rtk_demux_t *dmx)
{
    if (dmx == NULL) {
        return RTK_DEMUX_ERR_ARG;
    }
    if (dmx->in_callback) {
        return RTK_DEMUX_ERR_REENTRY;
    }
    if (dmx->cfg.on_frame == NULL) {
        return RTK_DEMUX_ERR_ARG;
    }
    if (dmx->len != 0U) {
        ++dmx->stats.aborts;
        // 保留当前起点已拒绝的集合，直接拒绝待补全候选，再尝试余下候选与后续起点。
        s_replay(dmx, true);
    }
    return RTK_DEMUX_OK;
}

// 线程安全：读取时须与 feed/abort 串行；ISR 安全，无阻塞。
bool rtk_demux_busy(const rtk_demux_t *dmx)
{
    return (dmx != NULL) && (dmx->len != 0U);
}

// 线程安全：统计快照须由调用方同步；ISR 安全，无阻塞。
const rtk_demux_stats_t *rtk_demux_stats(const rtk_demux_t *dmx)
{
    return (dmx != NULL) ? &dmx->stats : NULL;
}

// 线程安全：须与解析及其他统计访问串行；ISR 安全，无阻塞。
void rtk_demux_reset_stats(rtk_demux_t *dmx)
{
    if (dmx != NULL) {
        memset(&dmx->stats, 0, sizeof(dmx->stats));
    }
}
