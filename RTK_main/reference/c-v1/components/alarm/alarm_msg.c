/**
 * @file    alarm_msg.c
 * @brief   报警器扩展消息（0x40/0x41）编解码与报警状态源（纯 C）。
 *
 * 布局见 docs/protocol/phone_protocol.md §5；多字节字段按小端逐字节写，
 * 不对结构体做强转或整体 memcpy。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <stddef.h>

#include "alarm_msg.h"

/* 0x40 payload 偏移。 */
#define ALARM_OFS_SEQ (0U)
#define ALARM_OFS_STATE (2U)
#define ALARM_OFS_LEVEL (3U)
#define ALARM_OFS_FIX (4U)
#define ALARM_OFS_BATTERY (5U)
#define ALARM_OFS_FLAGS (6U)
#define ALARM_OFS_RESERVED (7U)
#define ALARM_OFS_UPTIME (8U)
#define ALARM_OFS_TAIL (12U)

/* 0x41 payload 偏移。 */
#define ALARM_HB_OFS_SEQ (0U)
#define ALARM_HB_OFS_BATTERY (2U)
#define ALARM_HB_OFS_STATE (3U)

#define ALARM_U16_BYTES (2U)
#define ALARM_U32_BYTES (4U)
#define ALARM_BYTE_BITS (8U)

/* 围栏报警（无测试覆盖）时的默认等级。 */
#define ALARM_FENCE_ALERT_LEVEL (2U)

/* 从 idx = 0 起清零，避免以非字面量作循环初值时触发初始化检查。 */
static void
zero_bytes (uint8_t * p_buf, size_t len)
{
    for (size_t idx = 0U; idx < len; idx++)
    {
        p_buf[idx] = 0U;
    }
}

static uint16_t
read_u16_le (uint8_t const * p_buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < ALARM_U16_BYTES; idx++)
    {
        /* u8 提升到 u32 无损 */
        value |= ((uint32_t) p_buf[idx]) << (ALARM_BYTE_BITS * idx);
    }
    /* 最大 0xFFFF，u16 截断安全 */
    return (uint16_t) value;
}

static void
write_u16_le (uint8_t * p_buf, uint16_t value)
{
    for (uint32_t idx = 0U; idx < ALARM_U16_BYTES; idx++)
    {
        /* 取低 8 位，u8 截断安全 */
        p_buf[idx] = (uint8_t) (value >> (ALARM_BYTE_BITS * idx));
    }
}

static void
write_u32_le (uint8_t * p_buf, uint32_t value)
{
    for (uint32_t idx = 0U; idx < ALARM_U32_BYTES; idx++)
    {
        /* 取低 8 位，u8 截断安全 */
        p_buf[idx] = (uint8_t) (value >> (ALARM_BYTE_BITS * idx));
    }
}

size_t
alarm_msg_encode_status (alarm_status_t const * p_status, uint8_t * p_out,
                         size_t out_size)
{
    if ((NULL == p_status) || (NULL == p_out) || (out_size < ALARM_STATUS_LEN))
    {
        return 0U;
    }

    write_u16_le(&p_out[ALARM_OFS_SEQ], p_status->seq);
    p_out[ALARM_OFS_STATE]    = p_status->state;
    p_out[ALARM_OFS_LEVEL]    = p_status->level;
    p_out[ALARM_OFS_FIX]      = p_status->fix_quality;
    p_out[ALARM_OFS_BATTERY]  = p_status->battery_pct;
    p_out[ALARM_OFS_FLAGS]    = p_status->flags;
    p_out[ALARM_OFS_RESERVED] = 0U;
    write_u32_le(&p_out[ALARM_OFS_UPTIME], p_status->uptime_ms);

    zero_bytes(&p_out[ALARM_OFS_TAIL], ALARM_STATUS_LEN - ALARM_OFS_TAIL);
    return ALARM_STATUS_LEN;
}

bool
alarm_msg_decode_heartbeat (uint8_t const * p_data, size_t len,
                            alarm_heartbeat_t * p_hb)
{
    if ((NULL == p_data) || (NULL == p_hb) || (len < ALARM_HEARTBEAT_LEN))
    {
        return false;
    }

    p_hb->seq         = read_u16_le(&p_data[ALARM_HB_OFS_SEQ]);
    p_hb->battery_pct = p_data[ALARM_HB_OFS_BATTERY];
    p_hb->local_state = p_data[ALARM_HB_OFS_STATE];
    return true;
}

void
alarm_src_init (alarm_src_t * p_src)
{
    if (NULL == p_src)
    {
        return;
    }

    p_src->b_override     = false;
    p_src->override_state = ALARM_STATE_NORMAL;
    p_src->override_level = 0U;
    p_src->fence_state    = ALARM_STATE_UNKNOWN;
}

bool
alarm_src_set_override (alarm_src_t * p_src, uint8_t state, uint8_t level)
{
    if (NULL == p_src)
    {
        return false;
    }
    if (level > ALARM_LEVEL_MAX)
    {
        return false;
    }

    switch (state)
    {
        case ALARM_STATE_NORMAL:
        case ALARM_STATE_ALERT:
        case ALARM_STATE_NO_FIX:
            break;

        default:
            return false;
    }

    p_src->b_override     = true;
    p_src->override_state = state;
    p_src->override_level = level;
    return true;
}

void
alarm_src_clear_override (alarm_src_t * p_src)
{
    if (NULL == p_src)
    {
        return;
    }

    p_src->b_override = false;
}

void
alarm_src_set_fence (alarm_src_t * p_src, uint8_t state)
{
    if (NULL == p_src)
    {
        return;
    }

    p_src->fence_state = state;
}

uint8_t
alarm_src_state (alarm_src_t const * p_src)
{
    if (NULL == p_src)
    {
        return ALARM_STATE_UNKNOWN;
    }
    if (p_src->b_override)
    {
        return p_src->override_state;
    }
    return p_src->fence_state;
}

uint8_t
alarm_src_level (alarm_src_t const * p_src)
{
    if (NULL == p_src)
    {
        return 0U;
    }
    if (p_src->b_override)
    {
        return p_src->override_level;
    }
    if (ALARM_STATE_ALERT == p_src->fence_state)
    {
        return ALARM_FENCE_ALERT_LEVEL;
    }
    return 0U;
}

bool
alarm_src_is_override (alarm_src_t const * p_src)
{
    if (NULL == p_src)
    {
        return false;
    }
    return p_src->b_override;
}

/*** end of file ***/
