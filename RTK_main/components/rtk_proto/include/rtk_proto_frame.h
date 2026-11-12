// 手机/报警器 SPP 帧编解码，C 接口。RTK 固件（经 C++ 外观 rtk/proto/frame.hpp）与报警器固件（C）共用。
// 线路格式见 docs/protocol/phone_protocol.md §1–§3。
// 线程安全：否（解析器每会话一个实例，单一所有者）。ISR 安全：是（无阻塞、无分配）。
// 冻结接口（ADR-0003）。实现方不得修改。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTK_PROTO_SYNC_A      (0xAAU)
#define RTK_PROTO_SYNC_B      (0x44U)
#define RTK_PROTO_SYNC_C      (0x18U)
#define RTK_PROTO_HEADER_LEN  (20U)
#define RTK_PROTO_CRC_LEN     (4U)
#define RTK_PROTO_MAX_PAYLOAD (256U)
#define RTK_PROTO_MAX_FRAME   (RTK_PROTO_HEADER_LEN + RTK_PROTO_MAX_PAYLOAD + RTK_PROTO_CRC_LEN)

/* 头部字段偏移 */
#define RTK_PROTO_OFS_HDR_LEN  (3U)
#define RTK_PROTO_OFS_MSG_ID   (4U)
#define RTK_PROTO_OFS_MSG_LEN  (12U)
#define RTK_PROTO_OFS_SENDER   (16U)
#define RTK_PROTO_OFS_TYPE     (17U)
#define RTK_PROTO_OFS_PROTOCOL (18U)
#define RTK_PROTO_OFS_INTERVAL (19U)

#define RTK_PROTO_SENDER_APP   (0x00U)
#define RTK_PROTO_SENDER_DEV   (0x01U)
#define RTK_PROTO_SENDER_PANEL (0x02U)
#define RTK_PROTO_SENDER_ALARM (0x03U)

#define RTK_PROTO_TYPE_QUERY     (0x00U)
#define RTK_PROTO_TYPE_QUERY_RES (0x01U)
#define RTK_PROTO_TYPE_SET       (0x02U)
#define RTK_PROTO_TYPE_SET_RES   (0x03U)
#define RTK_PROTO_TYPE_PUSH      (0x04U)
#define RTK_PROTO_TYPE_ERR_REP   (0x05U)

typedef enum {
    RTK_PROTO_OK = 0,
    RTK_PROTO_ERR_ARG,   /* 空指针，或 payload_len > RTK_PROTO_MAX_PAYLOAD */
    RTK_PROTO_ERR_SPACE, /* 输出缓冲区不足 */
} rtk_proto_err_t;

/* 解析出的一帧。payload 指向解析器内部缓冲区，只在下一次 rtk_proto_parser_feed() 前有效。 */
typedef struct {
    uint16_t msg_id;
    uint8_t sender;
    uint8_t type;
    uint8_t protocol;
    uint8_t interval;
    uint16_t payload_len;
    const uint8_t *payload;
} rtk_proto_frame_t;

typedef enum {
    RTK_PROTO_PARSE_NONE = 0, /* 需要更多字节 */
    RTK_PROTO_PARSE_FRAME,    /* 一帧完整且 CRC 正确 */
    RTK_PROTO_PARSE_CRC_ERR,  /* 一帧收完但 CRC 错误，已丢弃 */
    RTK_PROTO_PARSE_LEN_ERR,  /* 头长字节 != 0x14 或 payload_len 超限，已丢弃 */
} rtk_proto_parse_result_t;

/* 每会话一个实例，常驻。成员仅供实现使用，调用方只读两个错误计数。
 * 全零即为合法的初始状态（C++ 外观依赖这一点做 constexpr 值初始化）。 */
typedef struct {
    uint8_t buf[RTK_PROTO_MAX_FRAME];
    uint16_t pos;
    uint16_t expect; /* 本帧总长，头部长度字段收完前为 0 */
    uint32_t crc_errors;
    uint32_t len_errors;
} rtk_proto_parser_t;

/**
 * 标准 CRC-32（IEEE，反射多项式 0xEDB88320，zlib 兼容），可分段累计：
 * crc32(crc32(0, a), b) == crc32(a || b)；rtk_proto_crc32(0, "123456789", 9) == 0xCBF43926。
 * data 为 NULL 或 len 为 0 时返回 crc 本身。
 */
uint32_t rtk_proto_crc32(uint32_t crc, const uint8_t *data, size_t len);

/** 请求 type 映射到应答 type：0x00→0x01，0x02→0x03，其他→0x05。 */
uint8_t rtk_proto_response_type(uint8_t req_type);

/**
 * 组一帧：头（预留字节、protocol、interval 置 0）+ payload + CRC（小端）。
 * payload_len 为 0 时 payload 可为 NULL。成功时 *out_len = 20 + payload_len + 4。
 * out/out_len 为 NULL、payload 为 NULL 且 payload_len != 0、payload_len 超限 → RTK_PROTO_ERR_ARG；
 * out_size 不足 → RTK_PROTO_ERR_SPACE。失败时不写 *out_len，out 内容未定义。
 */
rtk_proto_err_t rtk_proto_frame_build(uint8_t *out, size_t out_size, uint16_t msg_id, uint8_t sender, uint8_t type,
                                      const uint8_t *payload, uint16_t payload_len, size_t *out_len);

/** 清零全部状态（含错误计数）。parser 为 NULL 时不做任何事。 */
void rtk_proto_parser_init(rtk_proto_parser_t *parser);

/** 丢弃半帧，保留错误计数。parser 为 NULL 时不做任何事。 */
void rtk_proto_parser_reset(rtk_proto_parser_t *parser);

/**
 * 逐字节喂入。规则：
 * - 同步阶段遇到不匹配字节时，若该字节是 0xAA 则作为新帧起点，否则丢弃并重新寻找 0xAA。
 * - 头长字节 != 0x14 或 payload_len > RTK_PROTO_MAX_PAYLOAD：返回 LEN_ERR，len_errors+1，
 *   回到寻找 0xAA（不回扫已收字节）。
 * - 一帧收完：CRC 正确返回 FRAME 并填 *frame；否则 CRC_ERR，crc_errors+1。两者都回到寻找 0xAA。
 * parser 或 frame 为 NULL 时返回 NONE 且不改变状态。
 */
rtk_proto_parse_result_t rtk_proto_parser_feed(rtk_proto_parser_t *parser, uint8_t byte, rtk_proto_frame_t *frame);

#ifdef __cplusplus
}
#endif
