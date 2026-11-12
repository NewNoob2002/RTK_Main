/**
 * @file    alarm_msg.h
 * @brief   报警器扩展消息（0x40/0x41）编解码与报警状态源（纯 C）。
 *
 * 契约见 docs/protocol/phone_protocol.md §5。冻结接口，实现方不得修改。
 */

#ifndef ALARM_MSG_H
#define ALARM_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALARM_MSG_STATUS_ID (0x0040U)
#define ALARM_MSG_HEARTBEAT_ID (0x0041U)
#define ALARM_MSG_TEST_ID (0x0042U)
#define ALARM_MSG_FENCE_ID (0x0043U)
#define ALARM_MSG_PAIR_ID (0x0044U)

#define ALARM_STATUS_LEN (16U)
#define ALARM_HEARTBEAT_LEN (8U)
#define ALARM_PUSH_PERIOD_MS (1000U)

#define ALARM_STATE_NORMAL (0x00U)
#define ALARM_STATE_ALERT (0x01U)
#define ALARM_STATE_NO_FIX (0x02U)
#define ALARM_STATE_UNKNOWN (0xFFU)
#define ALARM_LEVEL_MAX (3U)
#define ALARM_VALUE_UNKNOWN (0xFFU)

#define ALARM_FLAG_PHONE_ONLINE (0x01U)
#define ALARM_FLAG_TEST_OVERRIDE (0x02U)

typedef struct
{
    uint16_t seq;
    uint8_t  state;
    uint8_t  level;
    uint8_t  fix_quality;
    uint8_t  battery_pct;
    uint8_t  flags;
    uint32_t uptime_ms;
} alarm_status_t;

typedef struct
{
    uint16_t seq;
    uint8_t  battery_pct;
    uint8_t  local_state;
} alarm_heartbeat_t;

/* 报警状态源：测试覆盖优先，其次围栏结果（围栏模块预留）。 */
typedef struct
{
    bool    b_override;
    uint8_t override_state;
    uint8_t override_level;
    uint8_t fence_state; /* 初始 ALARM_STATE_UNKNOWN */
} alarm_src_t;

/** 按 §5 布局写 16 字节，返回写入字节数；参数非法或空间不足返回 0。 */
size_t alarm_msg_encode_status (alarm_status_t const * p_status,
                                uint8_t * p_out, size_t out_size);

/** len < ALARM_HEARTBEAT_LEN 或空指针返回 false；多余字节忽略。 */
bool alarm_msg_decode_heartbeat (uint8_t const * p_data, size_t len,
                                 alarm_heartbeat_t * p_hb);

void alarm_src_init (alarm_src_t * p_src);

/**
 * 设置测试覆盖。state 必须是 NORMAL/ALERT/NO_FIX 之一，
 * level ≤ ALARM_LEVEL_MAX，否则返回 false 且不改变状态。
 */
bool alarm_src_set_override (alarm_src_t * p_src, uint8_t state, uint8_t level);
void alarm_src_clear_override (alarm_src_t * p_src);

/** 围栏模块（预留）写入结果；接受任意值。 */
void alarm_src_set_fence (alarm_src_t * p_src, uint8_t state);

/** 当前状态：覆盖生效取覆盖值，否则取 fence_state。 */
uint8_t alarm_src_state (alarm_src_t const * p_src);

/** 当前等级：覆盖生效取覆盖等级；否则 ALERT 为 2，其他为 0。 */
uint8_t alarm_src_level (alarm_src_t const * p_src);

bool alarm_src_is_override (alarm_src_t const * p_src);

#endif /* ALARM_MSG_H */

/*** end of file ***/
