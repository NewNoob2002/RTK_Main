// 报警器扩展消息 0x40/0x41 的 C 编解码实现（ADR-0003：协议核心用 C，C++ 外观委托到 proto/AlarmCodec.hpp）。
// 无状态纯函数，任意任务可调用，非阻塞、无分配，ISR 安全。
// 不使用堆；线路格式见 docs/protocol/phone_protocol.md §5。

#include "DevProtoAlarm.h"

/* 0x40 payload 偏移。 */
#define DEV_PROTO_ALARM_OFS_SEQ      (0U)
#define DEV_PROTO_ALARM_OFS_STATE    (2U)
#define DEV_PROTO_ALARM_OFS_LEVEL    (3U)
#define DEV_PROTO_ALARM_OFS_FIX      (4U)
#define DEV_PROTO_ALARM_OFS_BATTERY  (5U)
#define DEV_PROTO_ALARM_OFS_FLAGS    (6U)
#define DEV_PROTO_ALARM_OFS_RESERVED (7U)
#define DEV_PROTO_ALARM_OFS_UPTIME   (8U)
#define DEV_PROTO_ALARM_OFS_TAIL     (12U)

/* 0x41 payload 偏移。 */
#define DEV_PROTO_ALARM_HB_OFS_SEQ     (0U)
#define DEV_PROTO_ALARM_HB_OFS_BATTERY (2U)
#define DEV_PROTO_ALARM_HB_OFS_STATE   (3U)

#define DEV_PROTO_ALARM_U16_BYTES (2U)
#define DEV_PROTO_ALARM_U32_BYTES (4U)
#define DEV_PROTO_ALARM_BYTE_BITS (8U)

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

    for (uint32_t idx = 0U; idx < DEV_PROTO_ALARM_U16_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (DEV_PROTO_ALARM_BYTE_BITS * idx);
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

    for (uint32_t idx = 0U; idx < DEV_PROTO_ALARM_U32_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (DEV_PROTO_ALARM_BYTE_BITS * idx);
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
    for (uint32_t idx = 0U; idx < DEV_PROTO_ALARM_U16_BYTES; idx++) {
        /* 只取低 8 位，显式窄化安全 */
        buf[idx] = (uint8_t)(value >> (DEV_PROTO_ALARM_BYTE_BITS * idx));
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
    for (uint32_t idx = 0U; idx < DEV_PROTO_ALARM_U32_BYTES; idx++) {
        /* 只取低 8 位，显式窄化安全 */
        buf[idx] = (uint8_t)(value >> (DEV_PROTO_ALARM_BYTE_BITS * idx));
    }
}

size_t dev_proto_alarm_encode_status(const dev_proto_alarm_status_t *status, uint8_t *out,
                                     size_t out_size)
{
    if ((status == NULL) || (out == NULL) || (out_size < DEV_PROTO_ALARM_STATUS_LEN)) {
        return 0U;
    }

    write_u16_le(&out[DEV_PROTO_ALARM_OFS_SEQ], status->seq);
    out[DEV_PROTO_ALARM_OFS_STATE] = status->state;
    out[DEV_PROTO_ALARM_OFS_LEVEL] = status->level;
    out[DEV_PROTO_ALARM_OFS_FIX] = status->fix_quality;
    out[DEV_PROTO_ALARM_OFS_BATTERY] = status->battery_pct;
    out[DEV_PROTO_ALARM_OFS_FLAGS] = status->flags;
    out[DEV_PROTO_ALARM_OFS_RESERVED] = 0U;
    write_u32_le(&out[DEV_PROTO_ALARM_OFS_UPTIME], status->uptime_ms);

    /* 尾部保留 4 字节。 */
    zero_bytes(&out[DEV_PROTO_ALARM_OFS_TAIL],
               DEV_PROTO_ALARM_STATUS_LEN - DEV_PROTO_ALARM_OFS_TAIL);
    return DEV_PROTO_ALARM_STATUS_LEN;
}

bool dev_proto_alarm_decode_status(const uint8_t *data, size_t len,
                                   dev_proto_alarm_status_t *status)
{
    if ((data == NULL) || (status == NULL) || (len < DEV_PROTO_ALARM_STATUS_LEN)) {
        return false;
    }

    status->seq = read_u16_le(&data[DEV_PROTO_ALARM_OFS_SEQ]);
    status->state = data[DEV_PROTO_ALARM_OFS_STATE];
    status->level = data[DEV_PROTO_ALARM_OFS_LEVEL];
    status->fix_quality = data[DEV_PROTO_ALARM_OFS_FIX];
    status->battery_pct = data[DEV_PROTO_ALARM_OFS_BATTERY];
    status->flags = data[DEV_PROTO_ALARM_OFS_FLAGS];
    status->uptime_ms = read_u32_le(&data[DEV_PROTO_ALARM_OFS_UPTIME]);
    return true;
}

/**
 * @brief  按 §5 布局编码 0x41 心跳（8 字节）
 * @param  heartbeat: 待编码的心跳字段
 * @param[out] out: 输出缓冲
 * @param  out_size: out 的容量，单位字节
 * @retval 8 成功，尾部 4 字节写 0
 * @retval 0 heartbeat/out 为 NULL 或 out_size < 8，此时不写入
 * @note   无阻塞、无分配、ISR 安全
 */
size_t dev_proto_alarm_encode_heartbeat(const dev_proto_alarm_heartbeat_t *heartbeat, uint8_t *out,
                                        size_t out_size)
{
    if ((heartbeat == NULL) || (out == NULL) || (out_size < DEV_PROTO_ALARM_HEARTBEAT_LEN)) {
        return 0U;
    }

    write_u16_le(&out[DEV_PROTO_ALARM_HB_OFS_SEQ], heartbeat->seq);
    out[DEV_PROTO_ALARM_HB_OFS_BATTERY] = heartbeat->battery_pct;
    out[DEV_PROTO_ALARM_HB_OFS_STATE] = heartbeat->local_state;

    /* 尾部保留 4 字节。 */
    zero_bytes(&out[DEV_PROTO_ALARM_HB_OFS_STATE + 1U],
               DEV_PROTO_ALARM_HEARTBEAT_LEN - (DEV_PROTO_ALARM_HB_OFS_STATE + 1U));
    return DEV_PROTO_ALARM_HEARTBEAT_LEN;
}

/**
 * @brief  按 §5 布局解码 0x41 心跳
 * @param  data: 收到的 payload（只读）
 * @param  len: data 的字节数，至少 8
 * @param[out] heartbeat: 解析结果；返回 false 时不改动
 * @retval true  长度足够，heartbeat 已填充
 * @retval false data/heartbeat 为 NULL 或 len < 8
 * @note   多余字节忽略；无阻塞、无分配、ISR 安全
 */
bool dev_proto_alarm_decode_heartbeat(const uint8_t *data, size_t len,
                                      dev_proto_alarm_heartbeat_t *heartbeat)
{
    if ((data == NULL) || (heartbeat == NULL) || (len < DEV_PROTO_ALARM_HEARTBEAT_LEN)) {
        return false;
    }

    heartbeat->seq = read_u16_le(&data[DEV_PROTO_ALARM_HB_OFS_SEQ]);
    heartbeat->battery_pct = data[DEV_PROTO_ALARM_HB_OFS_BATTERY];
    heartbeat->local_state = data[DEV_PROTO_ALARM_HB_OFS_STATE];
    return true;
}
