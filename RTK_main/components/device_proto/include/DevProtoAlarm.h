// 报警器扩展消息 0x40（状态）/0x41（心跳）的 C 编解码，RTK 与报警器两端共用。
// 线路格式见 docs/protocol/phone_protocol.md §5。线程安全：是（无状态）。ISR 安全：是。
// 冻结接口（ADR-0003）。实现方不得修改。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEV_PROTO_ALARM_MSG_STATUS    (0x0040U)
#define DEV_PROTO_ALARM_MSG_HEARTBEAT (0x0041U)
#define DEV_PROTO_ALARM_MSG_TEST      (0x0042U)
#define DEV_PROTO_ALARM_MSG_FENCE     (0x0043U)
#define DEV_PROTO_ALARM_MSG_PAIR      (0x0044U)

#define DEV_PROTO_ALARM_STATUS_LEN     (16U)
#define DEV_PROTO_ALARM_HEARTBEAT_LEN  (8U)
#define DEV_PROTO_ALARM_PUSH_PERIOD_MS (1000U)

#define DEV_PROTO_ALARM_STATE_NORMAL  (0x00U)
#define DEV_PROTO_ALARM_STATE_ALERT   (0x01U)
#define DEV_PROTO_ALARM_STATE_NO_FIX  (0x02U)
#define DEV_PROTO_ALARM_STATE_UNKNOWN (0xFFU)
#define DEV_PROTO_ALARM_LEVEL_MAX     (3U)
#define DEV_PROTO_ALARM_VALUE_UNKNOWN (0xFFU)

#define DEV_PROTO_ALARM_FLAG_PHONE_ONLINE  (0x01U)
#define DEV_PROTO_ALARM_FLAG_TEST_OVERRIDE (0x02U)

typedef struct {
    uint16_t seq;
    uint8_t state;
    uint8_t level;
    uint8_t fix_quality;
    uint8_t battery_pct;
    uint8_t flags;
    uint32_t uptime_ms;
} dev_proto_alarm_status_t;

typedef struct {
    uint16_t seq;
    uint8_t battery_pct;
    uint8_t local_state; /* 0 空闲，1 鸣响中，2 已静音 */
} dev_proto_alarm_heartbeat_t;

/** 按 §5 写 16 字节，返回写入字节数；参数为 NULL 或 out_size < 16 返回 0 且不写入。 */
size_t dev_proto_alarm_encode_status(const dev_proto_alarm_status_t *status, uint8_t *out, size_t out_size);

/** len < 16 或指针为 NULL 返回 false；多余字节忽略。 */
bool dev_proto_alarm_decode_status(const uint8_t *data, size_t len, dev_proto_alarm_status_t *status);

/** 写 8 字节心跳（seq、电量、本地状态、4 字节 0），返回写入字节数；参数非法或空间不足返回 0 且不写入。 */
size_t dev_proto_alarm_encode_heartbeat(const dev_proto_alarm_heartbeat_t *hb, uint8_t *out, size_t out_size);

/** len < 8 或指针为 NULL 返回 false；多余字节忽略。 */
bool dev_proto_alarm_decode_heartbeat(const uint8_t *data, size_t len, dev_proto_alarm_heartbeat_t *hb);

#ifdef __cplusplus
}
#endif
