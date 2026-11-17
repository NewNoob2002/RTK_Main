// 手机/报警器 SPP 帧编解码的 C 实现（ADR-0003：协议核心用 C，C++ 外观委托到 proto/Frame.hpp）。
// 解析器每会话一个实例、由链路任务独占调用，非线程安全；无阻塞、无分配，ISR 安全。
// 不使用堆。CRC-32 用逐位反射算法（与 zlib 等价）：单帧 ≤ 280 B，比 1 KB 查表更省 Flash。

#include "DevProtoFrame.h"

/* 本地字节宽度与 CRC 多项式（头文件未定义的实现细节）。 */
#define DEV_PROTO_U16_BYTES   (2U)
#define DEV_PROTO_U32_BYTES   (4U)
#define DEV_PROTO_BYTE_BITS   (8U)
#define DEV_PROTO_CRC32_POLY  (0xEDB88320UL)
#define DEV_PROTO_PRIV_SYNC_A (0U)
#define DEV_PROTO_PRIV_SYNC_B (1U)
#define DEV_PROTO_PRIV_SYNC_C (2U)

/* 流内位置：头长字节、payload 长度字段收完后的 pos。 */
#define DEV_PROTO_POS_HDR_LEN (DEV_PROTO_OFS_HDR_LEN + 1U)
#define DEV_PROTO_POS_MSG_LEN (DEV_PROTO_OFS_MSG_LEN + DEV_PROTO_U16_BYTES)

/* 头内两段保留字节的起点与长度。 */
#define DEV_PROTO_OFS_RESERVED_A (DEV_PROTO_OFS_MSG_ID + DEV_PROTO_U16_BYTES)
#define DEV_PROTO_LEN_RESERVED_A (DEV_PROTO_OFS_MSG_LEN - DEV_PROTO_OFS_RESERVED_A)
#define DEV_PROTO_OFS_RESERVED_B (DEV_PROTO_OFS_MSG_LEN + DEV_PROTO_U16_BYTES)
#define DEV_PROTO_LEN_RESERVED_B (DEV_PROTO_OFS_SENDER - DEV_PROTO_OFS_RESERVED_B)

/**
 * @brief  从 buf[0] 起写 len 个 0
 * @param[out] buf: 目标缓冲
 * @param  len: 清零的字节数
 * @note   调用方保证 buf 至少有 len 字节；无阻塞、ISR 安全
 */
static void zero_bytes(uint8_t *buf, size_t len)
{
    for (size_t idx = 0U; idx < len; idx++) {
        buf[idx] = 0U;
    }
}

/**
 * @brief  从 buf 读一个小端 u16
 * @param  buf: 至少 2 字节的只读缓冲
 * @retval 读出的 16 位值（与主机字节序无关）
 * @note   调用方保证长度足够；无阻塞、ISR 安全
 */
static uint16_t read_u16_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < DEV_PROTO_U16_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (DEV_PROTO_BYTE_BITS * idx);
    }
    /* 最大 0xFFFF，截断安全 */
    return (uint16_t)value;
}

/**
 * @brief  从 buf 读一个小端 u32
 * @param  buf: 至少 4 字节的只读缓冲
 * @retval 读出的 32 位值（与主机字节序无关）
 * @note   调用方保证长度足够；无阻塞、ISR 安全
 */
static uint32_t read_u32_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < DEV_PROTO_U32_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (DEV_PROTO_BYTE_BITS * idx);
    }
    return value;
}

/**
 * @brief  把 u16 以小端写入 buf
 * @param[out] buf: 目标缓冲
 * @param  value: 待写入的值
 * @note   调用方保证 buf 至少 2 字节；无阻塞、ISR 安全
 */
static void write_u16_le(uint8_t *buf, uint16_t value)
{
    for (uint32_t idx = 0U; idx < DEV_PROTO_U16_BYTES; idx++) {
        /* 只取低 8 位，显式窄化安全 */
        buf[idx] = (uint8_t)(value >> (DEV_PROTO_BYTE_BITS * idx));
    }
}

/**
 * @brief  把 u32 以小端写入 buf
 * @param[out] buf: 目标缓冲
 * @param  value: 待写入的值
 * @note   调用方保证 buf 至少 4 字节；无阻塞、ISR 安全
 */
static void write_u32_le(uint8_t *buf, uint32_t value)
{
    for (uint32_t idx = 0U; idx < DEV_PROTO_U32_BYTES; idx++) {
        /* 只取低 8 位，显式窄化安全 */
        buf[idx] = (uint8_t)(value >> (DEV_PROTO_BYTE_BITS * idx));
    }
}

/**
 * @brief  用一个字节更新反射 CRC-32 的中间值
 * @param  crc:  反射算法的内部状态（dev_proto_crc32 中的 ~crc 值）
 * @param  byte: 新到的数据字节
 * @retval 更新后的内部状态（仍为取反形式，由 dev_proto_crc32 最后再取反）
 * @note   与 zlib 的逐位实现等价；无阻塞、ISR 安全
 */
static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
    /* uint8_t 提升到 uint32_t 无损 */
    crc ^= (uint32_t)byte;

    for (uint32_t bit = 0U; bit < DEV_PROTO_BYTE_BITS; bit++) {
        if ((crc & 1U) != 0U) {
            crc = (crc >> 1U) ^ DEV_PROTO_CRC32_POLY;
        } else {
            crc >>= 1U;
        }
    }
    return crc;
}

/**
 * @brief  丢弃半帧进度（pos/expect 归零），保留错误计数
 * @param  parser: 解析器实例（调用方保证非空）
 * @note   已收字节留在 parser->buf 中但不再被引用；无阻塞、ISR 安全
 */
static void parser_clear(dev_proto_parser_t *parser)
{
    parser->pos = 0U;
    parser->expect = 0U;
}

/**
 * @brief  同步阶段消费一个字节，负责 AA 44 18 的重叠匹配
 * @param  parser: 解析器实例，pos 表示已匹配的同步字节数
 * @param  byte:   新收到的线路字节
 * @retval true  该字节已由同步阶段处理，调用方不要写入帧缓冲
 * @retval false 同步已完成，调用方应把该字节写入 parser->buf
 * @note   同步中途收到 0xAA 时保留为新帧首字节；无阻塞、ISR 安全
 */
static bool parser_sync_byte(dev_proto_parser_t *parser, uint8_t byte)
{
    bool syncing = true;

    switch (parser->pos) {
    case DEV_PROTO_PRIV_SYNC_A:
        if (byte == DEV_PROTO_SYNC_A) {
            parser->buf[DEV_PROTO_PRIV_SYNC_A] = byte;
            parser->pos = DEV_PROTO_PRIV_SYNC_B;
        }
        break;

    case DEV_PROTO_PRIV_SYNC_B:
        if (byte == DEV_PROTO_SYNC_B) {
            parser->buf[DEV_PROTO_PRIV_SYNC_B] = byte;
            parser->pos = DEV_PROTO_PRIV_SYNC_C;
        } else if (byte == DEV_PROTO_SYNC_A) {
            parser->buf[DEV_PROTO_PRIV_SYNC_A] = byte;
        } else {
            parser->pos = 0U;
        }
        break;

    case DEV_PROTO_PRIV_SYNC_C:
        if (byte == DEV_PROTO_SYNC_C) {
            parser->buf[DEV_PROTO_PRIV_SYNC_C] = byte;
            parser->pos++;
        } else if (byte == DEV_PROTO_SYNC_A) {
            parser->buf[DEV_PROTO_PRIV_SYNC_A] = byte;
            parser->pos = DEV_PROTO_PRIV_SYNC_B;
        } else {
            parser->pos = 0U;
        }
        break;

    default:
        syncing = false;
        break;
    }
    return syncing;
}

/**
 * @brief  在头长字节与 payload 长度字段收完时做长度检查
 * @param  parser: 解析器实例
 * @retval DEV_PROTO_PARSE_NONE    尚未到检查点，或长度合法
 * @retval DEV_PROTO_PARSE_LEN_ERR 头长字节 != 0x14 或 payload_len 超限；已计数并丢弃半帧
 * @note   检查点由 parser->pos 判定，其他位置直接返回 NONE；无阻塞、ISR 安全
 */
static dev_proto_parse_result_t parser_check_lengths(dev_proto_parser_t *parser)
{
    if (parser->pos == DEV_PROTO_POS_HDR_LEN) {
        if (parser->buf[DEV_PROTO_OFS_HDR_LEN] != (uint8_t)DEV_PROTO_HEADER_LEN) {
            parser->len_errors++;
            parser_clear(parser);
            return DEV_PROTO_PARSE_LEN_ERR;
        }
    } else if (parser->pos == DEV_PROTO_POS_MSG_LEN) {
        uint16_t payload_len = read_u16_le(&parser->buf[DEV_PROTO_OFS_MSG_LEN]);

        if (payload_len > DEV_PROTO_MAX_PAYLOAD) {
            parser->len_errors++;
            parser_clear(parser);
            return DEV_PROTO_PARSE_LEN_ERR;
        }
        /* 20 + ≤256 + 4 ≤ 280，uint16_t 窄化安全 */
        parser->expect = (uint16_t)(DEV_PROTO_HEADER_LEN + payload_len + DEV_PROTO_CRC_LEN);
    } else {
        /* 其他位置无需检查。 */
    }
    return DEV_PROTO_PARSE_NONE;
}

/**
 * @brief  一帧收完时校验 CRC 并填充结果
 * @param  parser: 解析器实例
 * @param[out] frame: CRC 正确时填充；失败或未收完时不改动
 * @retval DEV_PROTO_PARSE_NONE    帧尚未收完
 * @retval DEV_PROTO_PARSE_FRAME   CRC 正确，frame 已填充（payload 指向 parser->buf）
 * @retval DEV_PROTO_PARSE_CRC_ERR CRC 错误，已计数并丢弃半帧
 * @note   无论成功与否都会回到寻找 0xAA；无阻塞、ISR 安全
 */
static dev_proto_parse_result_t parser_finish(dev_proto_parser_t *parser, dev_proto_frame_t *frame)
{
    size_t crc_ofs = 0U;
    uint32_t crc_calc = 0U;
    uint32_t crc_rx = 0U;

    if (parser->pos != parser->expect) {
        return DEV_PROTO_PARSE_NONE;
    }

    /* expect ≥ 20 + 4，无符号差非负 */
    crc_ofs = (size_t)(parser->expect - DEV_PROTO_CRC_LEN);
    crc_calc = dev_proto_crc32(0U, parser->buf, crc_ofs);
    crc_rx = read_u32_le(&parser->buf[crc_ofs]);

    if (crc_calc != crc_rx) {
        parser->crc_errors++;
        parser_clear(parser);
        return DEV_PROTO_PARSE_CRC_ERR;
    }

    frame->msg_id = read_u16_le(&parser->buf[DEV_PROTO_OFS_MSG_ID]);
    frame->sender = parser->buf[DEV_PROTO_OFS_SENDER];
    frame->type = parser->buf[DEV_PROTO_OFS_TYPE];
    frame->protocol = parser->buf[DEV_PROTO_OFS_PROTOCOL];
    frame->interval = parser->buf[DEV_PROTO_OFS_INTERVAL];
    /* payload 长度 ≤ 256，uint16_t 窄化安全 */
    frame->payload_len = (uint16_t)(crc_ofs - DEV_PROTO_HEADER_LEN);
    frame->payload = &parser->buf[DEV_PROTO_HEADER_LEN];
    parser_clear(parser);
    return DEV_PROTO_PARSE_FRAME;
}

uint32_t dev_proto_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    uint32_t value = 0U;

    if (data == NULL) {
        return crc;
    }

    value = ~crc;
    for (size_t idx = 0U; idx < len; idx++) {
        value = crc32_update(value, data[idx]);
    }
    return ~value;
}

dev_proto_err_t dev_proto_frame_build(uint8_t *out, size_t out_size, uint16_t msg_id,
                                      uint8_t sender, uint8_t type, const uint8_t *payload,
                                      uint16_t payload_len, size_t *out_len)
{
    size_t need = 0U;

    if ((out == NULL) || (out_len == NULL)) {
        return DEV_PROTO_ERR_ARG;
    }
    if ((payload == NULL) && (payload_len != 0U)) {
        return DEV_PROTO_ERR_ARG;
    }
    if (payload_len > DEV_PROTO_MAX_PAYLOAD) {
        return DEV_PROTO_ERR_ARG;
    }

    /* payload_len ≤ 256，size_t 加法不溢出 */
    need = DEV_PROTO_HEADER_LEN + (size_t)payload_len + DEV_PROTO_CRC_LEN;
    if (out_size < need) {
        return DEV_PROTO_ERR_SPACE;
    }

    out[DEV_PROTO_PRIV_SYNC_A] = DEV_PROTO_SYNC_A;
    out[DEV_PROTO_PRIV_SYNC_B] = DEV_PROTO_SYNC_B;
    out[DEV_PROTO_PRIV_SYNC_C] = DEV_PROTO_SYNC_C;
    /* DEV_PROTO_HEADER_LEN == 20，uint8_t 窄化安全 */
    out[DEV_PROTO_OFS_HDR_LEN] = (uint8_t)DEV_PROTO_HEADER_LEN;
    write_u16_le(&out[DEV_PROTO_OFS_MSG_ID], msg_id);

    zero_bytes(&out[DEV_PROTO_OFS_RESERVED_A], DEV_PROTO_LEN_RESERVED_A);
    write_u16_le(&out[DEV_PROTO_OFS_MSG_LEN], payload_len);
    zero_bytes(&out[DEV_PROTO_OFS_RESERVED_B], DEV_PROTO_LEN_RESERVED_B);

    out[DEV_PROTO_OFS_SENDER] = sender;
    out[DEV_PROTO_OFS_TYPE] = type;
    out[DEV_PROTO_OFS_PROTOCOL] = 0U;
    out[DEV_PROTO_OFS_INTERVAL] = 0U;

    for (uint16_t idx = 0U; idx < payload_len; idx++) {
        out[DEV_PROTO_HEADER_LEN + idx] = payload[idx];
    }

    /* 头 + payload，不含 CRC 自身 */
    write_u32_le(&out[DEV_PROTO_HEADER_LEN + payload_len],
                 dev_proto_crc32(0U, out, (size_t)DEV_PROTO_HEADER_LEN + payload_len));

    *out_len = need;
    return DEV_PROTO_OK;
}

uint8_t dev_proto_response_type(uint8_t req_type)
{
    uint8_t res_type = DEV_PROTO_TYPE_ERR_REP;

    switch (req_type) {
    case DEV_PROTO_TYPE_QUERY:
        res_type = DEV_PROTO_TYPE_QUERY_RES;
        break;

    case DEV_PROTO_TYPE_SET:
        res_type = DEV_PROTO_TYPE_SET_RES;
        break;

    default:
        res_type = DEV_PROTO_TYPE_ERR_REP;
        break;
    }
    return res_type;
}

void dev_proto_parser_init(dev_proto_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }

    zero_bytes(parser->buf, sizeof(parser->buf));
    parser->pos = 0U;
    parser->expect = 0U;
    parser->crc_errors = 0U;
    parser->len_errors = 0U;
}

void dev_proto_parser_reset(dev_proto_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }

    parser_clear(parser);
}

dev_proto_parse_result_t dev_proto_parser_feed(dev_proto_parser_t *parser, uint8_t byte,
                                               dev_proto_frame_t *frame)
{
    dev_proto_parse_result_t result = DEV_PROTO_PARSE_NONE;

    if ((parser == NULL) || (frame == NULL)) {
        return DEV_PROTO_PARSE_NONE;
    }

    if (parser_sync_byte(parser, byte)) {
        return DEV_PROTO_PARSE_NONE;
    }

    parser->buf[parser->pos] = byte;
    parser->pos++;

    result = parser_check_lengths(parser);
    if (result != DEV_PROTO_PARSE_NONE) {
        return result;
    }

    return parser_finish(parser, frame);
}
