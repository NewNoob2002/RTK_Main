// 报警器扩展消息 0x40/0x41 的 C 编解码（ADR-0003：协议核心用 C，C++ 外观委托到这里）。
// 由 reference/c-v1/components/alarm/alarm_msg.c 的编解码部分移植：符号改为 rtk_proto_alarm_*，
// 布局改为 ESP-IDF Style，去掉 BARR-C 的 p_ 前缀；新增 rtk_proto_alarm_decode_status() 与
// rtk_proto_alarm_encode_heartbeat()。alarm_src_* 不移植（已由 C++ AlarmSource 承担）。
// 多字节字段逐字节小端读写，不对结构体做强转。布局见 docs/protocol/phone_protocol.md §5。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk_proto_alarm.h"

/* 0x40 payload 偏移。 */
#define RTK_PROTO_ALARM_OFS_SEQ      (0U)
#define RTK_PROTO_ALARM_OFS_STATE    (2U)
#define RTK_PROTO_ALARM_OFS_LEVEL    (3U)
#define RTK_PROTO_ALARM_OFS_FIX      (4U)
#define RTK_PROTO_ALARM_OFS_BATTERY  (5U)
#define RTK_PROTO_ALARM_OFS_FLAGS    (6U)
#define RTK_PROTO_ALARM_OFS_RESERVED (7U)
#define RTK_PROTO_ALARM_OFS_UPTIME   (8U)
#define RTK_PROTO_ALARM_OFS_TAIL     (12U)

/* 0x41 payload 偏移。 */
#define RTK_PROTO_ALARM_HB_OFS_SEQ     (0U)
#define RTK_PROTO_ALARM_HB_OFS_BATTERY (2U)
#define RTK_PROTO_ALARM_HB_OFS_STATE   (3U)

#define RTK_PROTO_ALARM_U16_BYTES (2U)
#define RTK_PROTO_ALARM_U32_BYTES (4U)
#define RTK_PROTO_ALARM_BYTE_BITS (8U)

/* 从 idx = 0 起清零。 */
static void zero_bytes(uint8_t *buf, size_t len)
{
    for (size_t idx = 0U; idx < len; idx++) {
        buf[idx] = 0U;
    }
}

static uint16_t read_u16_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < RTK_PROTO_ALARM_U16_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (RTK_PROTO_ALARM_BYTE_BITS * idx);
    }
    /* 最大 0xFFFF，截断安全 */
    return (uint16_t)value;
}

static uint32_t read_u32_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < RTK_PROTO_ALARM_U32_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (RTK_PROTO_ALARM_BYTE_BITS * idx);
    }
    return value;
}

static void write_u16_le(uint8_t *buf, uint16_t value)
{
    for (uint32_t idx = 0U; idx < RTK_PROTO_ALARM_U16_BYTES; idx++) {
        /* 只取低 8 位，显式窄化安全 */
        buf[idx] = (uint8_t)(value >> (RTK_PROTO_ALARM_BYTE_BITS * idx));
    }
}

static void write_u32_le(uint8_t *buf, uint32_t value)
{
    for (uint32_t idx = 0U; idx < RTK_PROTO_ALARM_U32_BYTES; idx++) {
        /* 只取低 8 位，显式窄化安全 */
        buf[idx] = (uint8_t)(value >> (RTK_PROTO_ALARM_BYTE_BITS * idx));
    }
}

size_t rtk_proto_alarm_encode_status(const rtk_proto_alarm_status_t *status, uint8_t *out, size_t out_size)
{
    if ((status == NULL) || (out == NULL) || (out_size < RTK_PROTO_ALARM_STATUS_LEN)) {
        return 0U;
    }

    write_u16_le(&out[RTK_PROTO_ALARM_OFS_SEQ], status->seq);
    out[RTK_PROTO_ALARM_OFS_STATE] = status->state;
    out[RTK_PROTO_ALARM_OFS_LEVEL] = status->level;
    out[RTK_PROTO_ALARM_OFS_FIX] = status->fix_quality;
    out[RTK_PROTO_ALARM_OFS_BATTERY] = status->battery_pct;
    out[RTK_PROTO_ALARM_OFS_FLAGS] = status->flags;
    out[RTK_PROTO_ALARM_OFS_RESERVED] = 0U;
    write_u32_le(&out[RTK_PROTO_ALARM_OFS_UPTIME], status->uptime_ms);

    /* 尾部保留 4 字节。 */
    zero_bytes(&out[RTK_PROTO_ALARM_OFS_TAIL], RTK_PROTO_ALARM_STATUS_LEN - RTK_PROTO_ALARM_OFS_TAIL);
    return RTK_PROTO_ALARM_STATUS_LEN;
}

bool rtk_proto_alarm_decode_status(const uint8_t *data, size_t len, rtk_proto_alarm_status_t *status)
{
    if ((data == NULL) || (status == NULL) || (len < RTK_PROTO_ALARM_STATUS_LEN)) {
        return false;
    }

    status->seq = read_u16_le(&data[RTK_PROTO_ALARM_OFS_SEQ]);
    status->state = data[RTK_PROTO_ALARM_OFS_STATE];
    status->level = data[RTK_PROTO_ALARM_OFS_LEVEL];
    status->fix_quality = data[RTK_PROTO_ALARM_OFS_FIX];
    status->battery_pct = data[RTK_PROTO_ALARM_OFS_BATTERY];
    status->flags = data[RTK_PROTO_ALARM_OFS_FLAGS];
    status->uptime_ms = read_u32_le(&data[RTK_PROTO_ALARM_OFS_UPTIME]);
    return true;
}

size_t rtk_proto_alarm_encode_heartbeat(const rtk_proto_alarm_heartbeat_t *heartbeat, uint8_t *out, size_t out_size)
{
    if ((heartbeat == NULL) || (out == NULL) || (out_size < RTK_PROTO_ALARM_HEARTBEAT_LEN)) {
        return 0U;
    }

    write_u16_le(&out[RTK_PROTO_ALARM_HB_OFS_SEQ], heartbeat->seq);
    out[RTK_PROTO_ALARM_HB_OFS_BATTERY] = heartbeat->battery_pct;
    out[RTK_PROTO_ALARM_HB_OFS_STATE] = heartbeat->local_state;

    /* 尾部保留 4 字节。 */
    zero_bytes(&out[RTK_PROTO_ALARM_HB_OFS_STATE + 1U],
               RTK_PROTO_ALARM_HEARTBEAT_LEN - (RTK_PROTO_ALARM_HB_OFS_STATE + 1U));
    return RTK_PROTO_ALARM_HEARTBEAT_LEN;
}

bool rtk_proto_alarm_decode_heartbeat(const uint8_t *data, size_t len, rtk_proto_alarm_heartbeat_t *heartbeat)
{
    if ((data == NULL) || (heartbeat == NULL) || (len < RTK_PROTO_ALARM_HEARTBEAT_LEN)) {
        return false;
    }

    heartbeat->seq = read_u16_le(&data[RTK_PROTO_ALARM_HB_OFS_SEQ]);
    heartbeat->battery_pct = data[RTK_PROTO_ALARM_HB_OFS_BATTERY];
    heartbeat->local_state = data[RTK_PROTO_ALARM_HB_OFS_STATE];
    return true;
}
