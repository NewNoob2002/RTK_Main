/**
 * @file    cmd_disp.h
 * @brief   手机/报警器命令分发（纯 C）。
 *
 * 输入一帧已校验的帧与发送方角色，输出要回给同一会话的应答帧。
 * 契约见 docs/protocol/phone_protocol.md §3–§5。冻结接口，实现方不得修改。
 */

#ifndef CMD_DISP_H
#define CMD_DISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_msg.h"
#include "link_mgr.h"
#include "proto_frame.h"

#define CMD_WORK_MODE_LEN (48U)
#define CMD_WORK_MODE_SET (44U)
#define CMD_RADIO_LEN (28U)
#define CMD_RADIO_SET_MIN (15U)
#define CMD_LOG_LEN (50U)
#define CMD_DEV_INFO_LEN (136U)
#define CMD_DEV_STATE_LEN (43U)
#define CMD_BATTERY_LEN (16U)
#define CMD_COM_LEN (32U)
#define CMD_SAT_TRACK_LEN (12U)
#define CMD_STR8_LEN (8U)   /* 0x01 应答中 8 字节字符串字段 */
#define CMD_STR16_LEN (16U) /* 0x01 应答中 16 字节字符串字段 */

/* 设备信息（由 app 层填写；GNSS/电台/电池字段本阶段为预留占位）。
 * 字符串字段不要求以 0 结尾，应答时按字段宽度拷贝。 */
typedef struct
{
    char     product[CMD_STR8_LEN];
    char     sn[CMD_STR16_LEN];
    char     hw_ver[CMD_STR8_LEN];
    char     fw_ver[CMD_STR16_LEN];
    char     board[CMD_STR8_LEN];
    char     board_sn[CMD_STR16_LEN];
    char     board_fw[CMD_STR16_LEN];
    char     radio_type[CMD_STR8_LEN];
    char     radio_fw[CMD_STR16_LEN];
    bool     b_on_battery;
    uint8_t  battery_pct; /* ALARM_VALUE_UNKNOWN = 未知 */
    int8_t   battery_temp_c;
    uint32_t com_baud; /* 0x0D 应答用，默认 115200 */
} cmd_dev_info_t;

/* 分发上下文。配置块只保存在 RAM（持久化预留）。 */
typedef struct
{
    cmd_dev_info_t info;
    uint8_t        work_mode[CMD_WORK_MODE_LEN];
    uint8_t        radio_cfg[CMD_RADIO_LEN];
    uint8_t        log_cfg[CMD_LOG_LEN];
    alarm_src_t *  p_alarm;
    link_mgr_t *   p_link;
} cmd_ctx_t;

typedef enum
{
    CMD_RES_REPLY = 0, /* p_reply 中有应答帧，发回同一会话 */
    CMD_RES_NO_REPLY,  /* 已处理或按协议不回复 */
    CMD_RES_DENIED     /* 该角色无权执行此命令，不回复 */
} cmd_result_t;

typedef struct
{
    uint8_t           buf[PROTO_MAX_FRAME];
    size_t            len;
    bool              b_whitelist_changed; /* 为 true 时 app 层需持久化白名单 */
    bool              b_heartbeat;         /* 收到 0x41 心跳，hb 有效 */
    alarm_heartbeat_t hb;
} cmd_reply_t;

/**
 * 初始化上下文：info 清零后填默认值（product="E1-Lite"，
 * hw_ver="V1.1.0"，board="UM980"，radio_type="5002D"，
 * battery_pct=ALARM_VALUE_UNKNOWN，b_on_battery=true，com_baud=115200），
 * 配置块清零后 radio_cfg[17]=0x05。p_alarm/p_link 原样保存。
 */
void cmd_disp_init (cmd_ctx_t * p_ctx, alarm_src_t * p_alarm,
                    link_mgr_t * p_link);

/**
 * 处理一帧。p_reply 每次先清零（len=0，标志为 false）。
 * 权限：
 * - PHONE：§4 全部命令，以及 0x42、0x43、0x44。
 * - ALARM：0x41（PUSH，解析心跳，NO_REPLY）、0x40 查询（QUERY_RES 状态）。
 * - 其他角色/组合：CMD_RES_DENIED。
 * 0x40 查询应答中的 seq 填 0，uptime_ms 填 now_ms，
 * flags 的 bit0 按 p_link 中是否存在 PHONE 会话填写。
 * 未知 ID、5.4 保留 ID：CMD_RES_NO_REPLY。
 * 任一指针为 NULL：CMD_RES_NO_REPLY。
 */
cmd_result_t cmd_disp_handle (cmd_ctx_t * p_ctx, link_role_t role,
                              proto_frame_t const * p_frame, uint32_t now_ms,
                              cmd_reply_t * p_reply);

/**
 * 组 0x40 主动上报帧（type=PUSH，sender=DEV）。seq 由调用方维护，
 * battery_pct 来自 info，fix_quality 本阶段固定 ALARM_VALUE_UNKNOWN。
 * 成功返回 true 并写 p_reply->buf/len。
 */
bool cmd_build_status_push (cmd_ctx_t const * p_ctx, uint16_t seq,
                            uint32_t now_ms, cmd_reply_t * p_reply);

#endif /* CMD_DISP_H */

/*** end of file ***/
