/**
 * @file    proto_frame.h
 * @brief   手机/报警器 SPP 帧编解码（纯 C，无 IDF 依赖）。
 *
 * 契约见 docs/protocol/phone_protocol.md §1–§3。冻结接口，实现方不得修改。
 */

#ifndef PROTO_FRAME_H
#define PROTO_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTO_SYNC_A (0xAAU)
#define PROTO_SYNC_B (0x44U)
#define PROTO_SYNC_C (0x18U)
#define PROTO_HEADER_LEN (20U)
#define PROTO_CRC_LEN (4U)
#define PROTO_MAX_PAYLOAD (256U)
#define PROTO_MAX_FRAME (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN)

/* 头部字段偏移 */
#define PROTO_OFS_HDR_LEN (3U)
#define PROTO_OFS_MSG_ID (4U)
#define PROTO_OFS_MSG_LEN (12U)
#define PROTO_OFS_SENDER (16U)
#define PROTO_OFS_TYPE (17U)
#define PROTO_OFS_PROTOCOL (18U)
#define PROTO_OFS_INTERVAL (19U)

#define PROTO_SENDER_APP (0x00U)
#define PROTO_SENDER_DEV (0x01U)
#define PROTO_SENDER_PANEL (0x02U)
#define PROTO_SENDER_ALARM (0x03U)

#define PROTO_TYPE_QUERY (0x00U)
#define PROTO_TYPE_QUERY_RES (0x01U)
#define PROTO_TYPE_SET (0x02U)
#define PROTO_TYPE_SET_RES (0x03U)
#define PROTO_TYPE_PUSH (0x04U)
#define PROTO_TYPE_ERR_REP (0x05U)

typedef enum
{
    PROTO_OK = 0,
    PROTO_ERR_ARG,  /* 空指针或 payload_len > PROTO_MAX_PAYLOAD */
    PROTO_ERR_SPACE /* 输出缓冲区不足 */
} proto_err_t;

/* 解析出的一帧。p_payload 指向解析器内部缓冲区，
 * 只在下一次 proto_parser_feed() 调用前有效。 */
typedef struct
{
    uint16_t        msg_id;
    uint8_t         sender;
    uint8_t         type;
    uint8_t         protocol;
    uint8_t         interval;
    uint16_t        payload_len;
    uint8_t const * p_payload;
} proto_frame_t;

typedef enum
{
    PROTO_PARSE_NONE = 0, /* 需要更多字节 */
    PROTO_PARSE_FRAME,    /* 得到一帧完整且 CRC 正确的帧 */
    PROTO_PARSE_CRC_ERR,  /* 一帧收完但 CRC 错误，已丢弃 */
    PROTO_PARSE_LEN_ERR   /* 头长字节 != 0x14 或 payload_len 超限，已丢弃 */
} proto_parse_result_t;

/* 每个会话一个实例，常驻，不得每次读数据时重建。
 * 成员仅供实现使用，调用方只读统计计数器。 */
typedef struct
{
    uint8_t  buf[PROTO_MAX_FRAME];
    uint16_t pos;
    uint16_t expect; /* 本帧总长度，头收完前为 0 */
    uint32_t crc_errors;
    uint32_t len_errors;
} proto_parser_t;

/**
 * 标准 CRC-32（zlib 兼容），可分段累计：
 * proto_crc32(proto_crc32(0, a, n), b, m) == crc32(a || b)。
 * proto_crc32(0U, "123456789", 9) == 0xCBF43926。
 * p_data 为 NULL 且 len 为 0 时返回 crc 本身。
 */
uint32_t proto_crc32 (uint32_t crc, uint8_t const * p_data, size_t len);

/**
 * 组一帧：头（预留字节置 0、protocol/interval 置 0）+ payload + CRC。
 * payload_len 为 0 时 p_payload 可为 NULL。
 * 成功时 *p_out_len = 20 + payload_len + 4。失败时不写 *p_out_len，
 * p_out 内容未定义。
 */
proto_err_t proto_frame_build (uint8_t * p_out, size_t out_size,
                               uint16_t msg_id, uint8_t sender, uint8_t type,
                               uint8_t const * p_payload, uint16_t payload_len,
                               size_t * p_out_len);

/** 请求 type 映射到应答 type（§2）：0x00→0x01，0x02→0x03，其他→0x05。 */
uint8_t proto_response_type (uint8_t req_type);

void proto_parser_init (proto_parser_t * p_parser);

/**
 * 逐字节喂入。规则：
 * - 同步阶段遇到不匹配字节时，若该字节本身是 0xAA 则作为新帧起点，
 *   否则丢弃并重新寻找 0xAA。
 * - 头长字节 != 0x14 或 payload_len > PROTO_MAX_PAYLOAD：返回
 *   PROTO_PARSE_LEN_ERR，len_errors+1，回到寻找 0xAA（不回扫已收字节）。
 * - 一帧收完：CRC 正确返回 PROTO_PARSE_FRAME 并填 *p_frame；
 *   否则返回 PROTO_PARSE_CRC_ERR，crc_errors+1。两种情况都回到寻找 0xAA。
 * p_parser 或 p_frame 为 NULL 时返回 PROTO_PARSE_NONE 且不改变状态。
 */
proto_parse_result_t proto_parser_feed (proto_parser_t * p_parser, uint8_t byte,
                                        proto_frame_t * p_frame);

#endif /* PROTO_FRAME_H */

/*** end of file ***/
