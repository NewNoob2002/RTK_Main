// 手机/报警器 SPP 帧编解码的 C 接口（冻结，ADR-0003）；RTK 固件经 proto/Frame.hpp 调用，报警器固件直接用 C。
// 解析器每会话一个实例、单一所有者，非线程安全；无阻塞、无分配，ISR 安全。
// 不使用堆；线路格式见 docs/protocol/phone_protocol.md §1–§3。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEV_PROTO_SYNC_A      (0xAAU)
#define DEV_PROTO_SYNC_B      (0x44U)
#define DEV_PROTO_SYNC_C      (0x18U)
#define DEV_PROTO_HEADER_LEN  (20U)
#define DEV_PROTO_CRC_LEN     (4U)
#define DEV_PROTO_MAX_PAYLOAD (256U)
#define DEV_PROTO_MAX_FRAME   (DEV_PROTO_HEADER_LEN + DEV_PROTO_MAX_PAYLOAD + DEV_PROTO_CRC_LEN)

/* 头部字段偏移 */
#define DEV_PROTO_OFS_HDR_LEN  (3U)
#define DEV_PROTO_OFS_MSG_ID   (4U)
#define DEV_PROTO_OFS_MSG_LEN  (12U)
#define DEV_PROTO_OFS_SENDER   (16U)
#define DEV_PROTO_OFS_TYPE     (17U)
#define DEV_PROTO_OFS_PROTOCOL (18U)
#define DEV_PROTO_OFS_INTERVAL (19U)

#define DEV_PROTO_SENDER_APP   (0x00U)
#define DEV_PROTO_SENDER_DEV   (0x01U)
#define DEV_PROTO_SENDER_PANEL (0x02U)
#define DEV_PROTO_SENDER_ALARM (0x03U)

#define DEV_PROTO_TYPE_QUERY     (0x00U)
#define DEV_PROTO_TYPE_QUERY_RES (0x01U)
#define DEV_PROTO_TYPE_SET       (0x02U)
#define DEV_PROTO_TYPE_SET_RES   (0x03U)
#define DEV_PROTO_TYPE_PUSH      (0x04U)
#define DEV_PROTO_TYPE_ERR_REP   (0x05U)

typedef enum {
    DEV_PROTO_OK = 0,
    DEV_PROTO_ERR_ARG,   /* 空指针，或 payload_len > DEV_PROTO_MAX_PAYLOAD */
    DEV_PROTO_ERR_SPACE, /* 输出缓冲区不足 */
} dev_proto_err_t;

/* 解析出的一帧。payload 指向解析器内部缓冲区，只在下一次 dev_proto_parser_feed() 前有效。 */
typedef struct {
    uint16_t msg_id;
    uint8_t sender;
    uint8_t type;
    uint8_t protocol;
    uint8_t interval;
    uint16_t payload_len;
    const uint8_t *payload;
} dev_proto_frame_t;

typedef enum {
    DEV_PROTO_PARSE_NONE = 0, /* 需要更多字节 */
    DEV_PROTO_PARSE_FRAME,    /* 一帧完整且 CRC 正确 */
    DEV_PROTO_PARSE_CRC_ERR,  /* 一帧收完但 CRC 错误，已丢弃 */
    DEV_PROTO_PARSE_LEN_ERR,  /* 头长字节 != 0x14 或 payload_len 超限，已丢弃 */
} dev_proto_parse_result_t;

/* 每会话一个实例，常驻。成员仅供实现使用，调用方只读两个错误计数。
 * 全零即为合法的初始状态（C++ 外观依赖这一点做 constexpr 值初始化）。 */
typedef struct {
    uint8_t buf[DEV_PROTO_MAX_FRAME];
    uint16_t pos;
    uint16_t expect; /* 本帧总长，头部长度字段收完前为 0 */
    uint32_t crc_errors;
    uint32_t len_errors;
} dev_proto_parser_t;

/**
 * @brief  计算标准 CRC-32（IEEE，反射多项式 0xEDB88320，zlib 兼容），可分段累计
 * @param  crc:  上一段数据的 CRC；首段传 0
 * @param  data: 待计算数据（只读）；为 NULL 时直接返回 crc
 * @param  len:  data 的字节数；为 0 时返回 crc
 * @retval 累计后的 CRC-32；crc32(crc32(0, a), b) == crc32(a || b)
 * @note   dev_proto_crc32(0, "123456789", 9) == 0xCBF43926；纯函数、无阻塞、ISR 安全
 */
uint32_t dev_proto_crc32(uint32_t crc, const uint8_t *data, size_t len);

/**
 * @brief  把请求 type 映射为应答 type
 * @param  req_type: 请求帧的 type 字节
 * @retval DEV_PROTO_TYPE_QUERY_RES 请求为 QUERY
 * @retval DEV_PROTO_TYPE_SET_RES   请求为 SET
 * @retval DEV_PROTO_TYPE_ERR_REP   其他值（含本身已是应答的 type）
 * @note   纯函数、无阻塞、ISR 安全
 */
uint8_t dev_proto_response_type(uint8_t req_type);

/**
 * @brief  组一帧：头（预留字节与 protocol/interval 置 0）+ payload + CRC（小端）
 * @param  out:         输出缓冲
 * @param  out_size:    out 的容量，单位字节
 * @param  msg_id:      消息号，写入偏移 4（u16 小端）
 * @param  sender:      sender 字段字节
 * @param  type:        type 字段字节
 * @param  payload:     负载数据；payload_len 为 0 时可为 NULL
 * @param  payload_len: 负载字节数，必须 ≤ DEV_PROTO_MAX_PAYLOAD
 * @param[out] out_len: 成功时写入帧总长 20 + payload_len + 4；失败时不改动
 * @retval DEV_PROTO_OK        成功，帧已完整写入 out
 * @retval DEV_PROTO_ERR_ARG   out/out_len 为 NULL，payload 为 NULL 且 payload_len != 0，或 payload_len 超限
 * @retval DEV_PROTO_ERR_SPACE out_size 不足
 * @note   失败时 out 内容未定义；无阻塞、无分配、ISR 安全
 */
dev_proto_err_t dev_proto_frame_build(uint8_t *out, size_t out_size, uint16_t msg_id,
                                      uint8_t sender, uint8_t type, const uint8_t *payload,
                                      uint16_t payload_len, size_t *out_len);

/**
 * @brief  清零解析器全部状态（含错误计数）
 * @param  parser: 解析器实例；为 NULL 时不做任何事
 * @note   启动或新建会话时调用；无阻塞、无分配、ISR 安全
 */
void dev_proto_parser_init(dev_proto_parser_t *parser);

/**
 * @brief  丢弃半帧进度、保留错误计数
 * @param  parser: 解析器实例；为 NULL 时不做任何事
 * @note   重新对齐到同步字节时调用；无阻塞、无分配、ISR 安全
 */
void dev_proto_parser_reset(dev_proto_parser_t *parser);

/**
 * @brief  逐字节喂入解析器，凑满一帧时返回结果并填充 frame
 * @param  parser: 解析器实例（每会话一个，只能由同一任务串行调用）
 * @param  byte:   新收到的线路字节
 * @param[out] frame: 返回 DEV_PROTO_PARSE_FRAME 时填充；其他返回值不改动
 * @retval DEV_PROTO_PARSE_NONE    需要更多字节
 * @retval DEV_PROTO_PARSE_FRAME   一帧完整且 CRC 正确，frame 已填充
 * @retval DEV_PROTO_PARSE_CRC_ERR 一帧收完但 CRC 错误，已丢弃
 * @retval DEV_PROTO_PARSE_LEN_ERR 头长字节 != 0x14 或 payload_len 超限，已丢弃
 * @note   同步阶段遇到不匹配字节时，若该字节是 0xAA 则作为新帧起点，否则丢弃并重新寻找 0xAA；
 *         头长/长度错误时 len_errors+1，CRC 错误时 crc_errors+1，两者都回到寻找 0xAA（不回扫已收字节）。
 *         frame.payload 指向解析器内部缓冲，有效期到下一次 feed。parser 或 frame 为 NULL 时返回 NONE
 *         且不改变状态。无阻塞、无分配、ISR 安全。
 */
dev_proto_parse_result_t dev_proto_parser_feed(dev_proto_parser_t *parser, uint8_t byte,
                                               dev_proto_frame_t *frame);

#ifdef __cplusplus
}
#endif
