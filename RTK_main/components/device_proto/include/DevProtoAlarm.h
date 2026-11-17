// 报警器扩展消息 0x40（状态）/0x41（心跳）的 C 编解码接口（冻结，ADR-0003），RTK 与报警器两端共用。
// 无状态纯函数，任意任务可调用；无阻塞、无分配，ISR 安全。
// 不使用堆；线路格式见 docs/protocol/phone_protocol.md §5。
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

/**
 * @brief  按 §5 布局编码 0x40 状态（16 字节，多字节字段小端）
 * @param  status: 待编码的状态字段
 * @param[out] out: 输出缓冲
 * @param  out_size: out 的容量，单位字节
 * @retval 16 成功，写入 DEV_PROTO_ALARM_STATUS_LEN 字节
 * @retval 0  status/out 为 NULL 或 out_size < 16，此时不写入任何字节
 * @note   保留字节与尾部 4 字节写 0；无阻塞、无分配、ISR 安全
 */
size_t dev_proto_alarm_encode_status(const dev_proto_alarm_status_t *status, uint8_t *out,
                                     size_t out_size);

/**
 * @brief  按 §5 布局解码 0x40 状态
 * @param  data: 收到的 payload（只读）
 * @param  len:  data 的字节数，至少 16
 * @param[out] status: 解析结果；返回 false 时不改动
 * @retval true  长度足够，status 已填充
 * @retval false data/status 为 NULL 或 len < 16
 * @note   多余字节忽略；无阻塞、无分配、ISR 安全
 */
bool dev_proto_alarm_decode_status(const uint8_t *data, size_t len,
                                   dev_proto_alarm_status_t *status);

/**
 * @brief  按 §5 布局编码 0x41 心跳（8 字节：seq、电量、本地状态、4 字节 0）
 * @param  hb: 待编码的心跳字段
 * @param[out] out: 输出缓冲
 * @param  out_size: out 的容量，单位字节
 * @retval 8 成功，写入 DEV_PROTO_ALARM_HEARTBEAT_LEN 字节
 * @retval 0 hb/out 为 NULL 或 out_size < 8，此时不写入任何字节
 * @note   无阻塞、无分配、ISR 安全
 */
size_t dev_proto_alarm_encode_heartbeat(const dev_proto_alarm_heartbeat_t *hb, uint8_t *out,
                                        size_t out_size);

/**
 * @brief  按 §5 布局解码 0x41 心跳
 * @param  data: 收到的 payload（只读）
 * @param  len:  data 的字节数，至少 8
 * @param[out] hb: 解析结果；返回 false 时不改动
 * @retval true  长度足够，hb 已填充
 * @retval false data/hb 为 NULL 或 len < 8
 * @note   多余字节忽略；无阻塞、无分配、ISR 安全
 */
bool dev_proto_alarm_decode_heartbeat(const uint8_t *data, size_t len,
                                      dev_proto_alarm_heartbeat_t *hb);

#ifdef __cplusplus
}
#endif
