/**
 * @file    proto_frame.c
 * @brief   手机/报警器 SPP 帧编解码（纯 C，无 IDF 依赖）。
 *
 * CRC-32 使用逐位算法（反射多项式 0xEDB88320），不依赖 IDF
 * esp_rom_crc32_le，也不引入 1 KB 查表；单帧上限 280 B，开销可忽略。
 * 所有多字节字段按小端逐字节组装，不对缓冲区做强转。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <stddef.h>

#include "proto_frame.h"

/* 头内偏移与字节宽度（头文件未定义的本地常量）。 */
#define PROTO_OFS_SYNC_A (0U)
#define PROTO_OFS_SYNC_B (1U)
#define PROTO_OFS_SYNC_C (2U)
#define PROTO_U16_BYTES (2U)
#define PROTO_U32_BYTES (4U)
#define PROTO_BYTE_BITS (8U)
#define PROTO_CRC32_POLY (0xEDB88320UL)

/* 流内位置：头长字节、payload 长度字段收完后的 pos。 */
#define PROTO_POS_HDR_LEN (PROTO_OFS_HDR_LEN + 1U)
#define PROTO_POS_MSG_LEN (PROTO_OFS_MSG_LEN + PROTO_U16_BYTES)

/* 头内两段保留字节的起点与长度（均为无符号编译期常量）。 */
#define PROTO_OFS_RESERVED_A (PROTO_OFS_MSG_ID + PROTO_U16_BYTES)
#define PROTO_LEN_RESERVED_A (PROTO_OFS_MSG_LEN - PROTO_OFS_RESERVED_A)
#define PROTO_OFS_RESERVED_B (PROTO_OFS_MSG_LEN + PROTO_U16_BYTES)
#define PROTO_LEN_RESERVED_B (PROTO_OFS_SENDER - PROTO_OFS_RESERVED_B)

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

    for (uint32_t idx = 0U; idx < PROTO_U16_BYTES; idx++)
    {
        /* u8 提升到 u32 无损 */
        value |= ((uint32_t) p_buf[idx]) << (PROTO_BYTE_BITS * idx);
    }
    /* 最大 0xFFFF，u16 截断安全 */
    return (uint16_t) value;
}

static uint32_t
read_u32_le (uint8_t const * p_buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < PROTO_U32_BYTES; idx++)
    {
        /* u8 提升到 u32 无损 */
        value |= ((uint32_t) p_buf[idx]) << (PROTO_BYTE_BITS * idx);
    }
    return value;
}

static void
write_u16_le (uint8_t * p_buf, uint16_t value)
{
    for (uint32_t idx = 0U; idx < PROTO_U16_BYTES; idx++)
    {
        /* 取低 8 位，u8 截断安全 */
        p_buf[idx] = (uint8_t) (value >> (PROTO_BYTE_BITS * idx));
    }
}

static void
write_u32_le (uint8_t * p_buf, uint32_t value)
{
    for (uint32_t idx = 0U; idx < PROTO_U32_BYTES; idx++)
    {
        /* 取低 8 位，u8 截断安全 */
        p_buf[idx] = (uint8_t) (value >> (PROTO_BYTE_BITS * idx));
    }
}

/* 单字节 CRC-32 更新（反射算法，与 zlib crc32 等价）。 */
static uint32_t
crc32_update (uint32_t crc, uint8_t byte)
{
    /* u8 提升到 u32 无损 */
    crc ^= (uint32_t) byte;

    for (uint32_t bit = 0U; bit < PROTO_BYTE_BITS; bit++)
    {
        if (0U != (crc & 1U))
        {
            crc = (crc >> 1U) ^ PROTO_CRC32_POLY;
        }
        else
        {
            crc >>= 1U;
        }
    }
    return crc;
}

/* 清空组帧状态，保留错误计数器。 */
static void
parser_reset (proto_parser_t * p_parser)
{
    p_parser->pos    = 0U;
    p_parser->expect = 0U;
}

/* 同步阶段消费一个字节；返回 true 表示该字节已由同步阶段处理。 */
static bool
parser_sync_byte (proto_parser_t * p_parser, uint8_t byte)
{
    bool b_syncing = true;

    switch (p_parser->pos)
    {
        case PROTO_OFS_SYNC_A:
            if (PROTO_SYNC_A == byte)
            {
                p_parser->buf[PROTO_OFS_SYNC_A] = byte;
                p_parser->pos                   = PROTO_OFS_SYNC_B;
            }
            break;

        case PROTO_OFS_SYNC_B:
            if (PROTO_SYNC_B == byte)
            {
                p_parser->buf[PROTO_OFS_SYNC_B] = byte;
                p_parser->pos                   = PROTO_OFS_SYNC_C;
            }
            else if (PROTO_SYNC_A == byte)
            {
                p_parser->buf[PROTO_OFS_SYNC_A] = byte;
            }
            else
            {
                p_parser->pos = 0U;
            }
            break;

        case PROTO_OFS_SYNC_C:
            if (PROTO_SYNC_C == byte)
            {
                p_parser->buf[PROTO_OFS_SYNC_C] = byte;
                p_parser->pos++;
            }
            else if (PROTO_SYNC_A == byte)
            {
                p_parser->buf[PROTO_OFS_SYNC_A] = byte;
                p_parser->pos                   = PROTO_OFS_SYNC_B;
            }
            else
            {
                p_parser->pos = 0U;
            }
            break;

        default:
            b_syncing = false;
            break;
    }
    return b_syncing;
}

/* 头长与 payload 长度检查；非错误时返回 PROTO_PARSE_NONE。 */
static proto_parse_result_t
parser_check_lengths (proto_parser_t * p_parser)
{
    if (PROTO_POS_HDR_LEN == p_parser->pos)
    {
        if (PROTO_HEADER_LEN != p_parser->buf[PROTO_OFS_HDR_LEN])
        {
            p_parser->len_errors++;
            parser_reset(p_parser);
            return PROTO_PARSE_LEN_ERR;
        }
    }
    else if (PROTO_POS_MSG_LEN == p_parser->pos)
    {
        uint16_t payload_len = read_u16_le(&p_parser->buf[PROTO_OFS_MSG_LEN]);

        if (payload_len > PROTO_MAX_PAYLOAD)
        {
            p_parser->len_errors++;
            parser_reset(p_parser);
            return PROTO_PARSE_LEN_ERR;
        }
        /* 20 + ≤256 + 4 ≤ 280，u16 无损 */
        p_parser->expect =
            (uint16_t) (PROTO_HEADER_LEN + payload_len + PROTO_CRC_LEN);
    }
    else
    {
        /* 其他位置无需检查。 */
    }
    return PROTO_PARSE_NONE;
}

/* 一帧收完时校验 CRC 并填充结果；帧未收完返回 PROTO_PARSE_NONE。 */
static proto_parse_result_t
parser_finish (proto_parser_t * p_parser, proto_frame_t * p_frame)
{
    size_t   crc_ofs  = 0U;
    uint32_t crc_calc = 0U;
    uint32_t crc_rx   = 0U;

    if (p_parser->pos != p_parser->expect)
    {
        return PROTO_PARSE_NONE;
    }

    /* expect ≥ 20 + 4，无符号差非负 */
    crc_ofs  = (size_t) (p_parser->expect - PROTO_CRC_LEN);
    crc_calc = proto_crc32(0U, p_parser->buf, crc_ofs);
    crc_rx   = read_u32_le(&p_parser->buf[crc_ofs]);

    if (crc_calc != crc_rx)
    {
        p_parser->crc_errors++;
        parser_reset(p_parser);
        return PROTO_PARSE_CRC_ERR;
    }

    p_frame->msg_id   = read_u16_le(&p_parser->buf[PROTO_OFS_MSG_ID]);
    p_frame->sender   = p_parser->buf[PROTO_OFS_SENDER];
    p_frame->type     = p_parser->buf[PROTO_OFS_TYPE];
    p_frame->protocol = p_parser->buf[PROTO_OFS_PROTOCOL];
    p_frame->interval = p_parser->buf[PROTO_OFS_INTERVAL];
    /* payload_len ≤ 256，u16 截断安全 */
    p_frame->payload_len = (uint16_t) (crc_ofs - PROTO_HEADER_LEN);
    p_frame->p_payload   = &p_parser->buf[PROTO_HEADER_LEN];
    parser_reset(p_parser);
    return PROTO_PARSE_FRAME;
}

uint32_t
proto_crc32 (uint32_t crc, uint8_t const * p_data, size_t len)
{
    if (NULL == p_data)
    {
        return crc;
    }

    crc = ~crc;
    for (size_t idx = 0U; idx < len; idx++)
    {
        crc = crc32_update(crc, p_data[idx]);
    }
    return ~crc;
}

proto_err_t
proto_frame_build (uint8_t * p_out, size_t out_size, uint16_t msg_id,
                   uint8_t sender, uint8_t type, uint8_t const * p_payload,
                   uint16_t payload_len, size_t * p_out_len)
{
    size_t need = 0U;

    if ((NULL == p_out) || (NULL == p_out_len))
    {
        return PROTO_ERR_ARG;
    }
    if ((NULL == p_payload) && (0U != payload_len))
    {
        return PROTO_ERR_ARG;
    }
    if (payload_len > PROTO_MAX_PAYLOAD)
    {
        return PROTO_ERR_ARG;
    }

    /* payload_len ≤ 256，size_t 加法不溢出 */
    need = PROTO_HEADER_LEN + (size_t) payload_len + PROTO_CRC_LEN;
    if (out_size < need)
    {
        return PROTO_ERR_SPACE;
    }

    p_out[PROTO_OFS_SYNC_A]  = PROTO_SYNC_A;
    p_out[PROTO_OFS_SYNC_B]  = PROTO_SYNC_B;
    p_out[PROTO_OFS_SYNC_C]  = PROTO_SYNC_C;
    p_out[PROTO_OFS_HDR_LEN] = PROTO_HEADER_LEN;
    write_u16_le(&p_out[PROTO_OFS_MSG_ID], msg_id);

    zero_bytes(&p_out[PROTO_OFS_RESERVED_A], PROTO_LEN_RESERVED_A);
    write_u16_le(&p_out[PROTO_OFS_MSG_LEN], payload_len);
    zero_bytes(&p_out[PROTO_OFS_RESERVED_B], PROTO_LEN_RESERVED_B);

    p_out[PROTO_OFS_SENDER]   = sender;
    p_out[PROTO_OFS_TYPE]     = type;
    p_out[PROTO_OFS_PROTOCOL] = 0U;
    p_out[PROTO_OFS_INTERVAL] = 0U;

    for (uint16_t idx = 0U; idx < payload_len; idx++)
    {
        p_out[PROTO_HEADER_LEN + idx] = p_payload[idx];
    }

    /* 头 + payload，不含 CRC 自身 */
    write_u32_le(
        &p_out[PROTO_HEADER_LEN + payload_len],
        proto_crc32(0U, p_out, PROTO_HEADER_LEN + (size_t) payload_len));

    *p_out_len = need;
    return PROTO_OK;
}

uint8_t
proto_response_type (uint8_t req_type)
{
    uint8_t res_type = PROTO_TYPE_ERR_REP;

    switch (req_type)
    {
        case PROTO_TYPE_QUERY:
            res_type = PROTO_TYPE_QUERY_RES;
            break;

        case PROTO_TYPE_SET:
            res_type = PROTO_TYPE_SET_RES;
            break;

        default:
            res_type = PROTO_TYPE_ERR_REP;
            break;
    }
    return res_type;
}

void
proto_parser_init (proto_parser_t * p_parser)
{
    if (NULL == p_parser)
    {
        return;
    }

    p_parser->pos        = 0U;
    p_parser->expect     = 0U;
    p_parser->crc_errors = 0U;
    p_parser->len_errors = 0U;
}

proto_parse_result_t
proto_parser_feed (proto_parser_t * p_parser, uint8_t byte,
                   proto_frame_t * p_frame)
{
    proto_parse_result_t result = PROTO_PARSE_NONE;

    if ((NULL == p_parser) || (NULL == p_frame))
    {
        return PROTO_PARSE_NONE;
    }

    if (parser_sync_byte(p_parser, byte))
    {
        return PROTO_PARSE_NONE;
    }

    p_parser->buf[p_parser->pos] = byte;
    p_parser->pos++;

    result = parser_check_lengths(p_parser);
    if (PROTO_PARSE_NONE != result)
    {
        return result;
    }

    return parser_finish(p_parser, p_frame);
}

/*** end of file ***/
