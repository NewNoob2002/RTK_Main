/**
 * @file    cmd_disp.c
 * @brief   手机/报警器命令分发（纯 C）。
 *
 * 协议见 docs/protocol/phone_protocol.md §3–§5。应答帧统一经
 * proto_frame_build 写入 p_reply->buf；配置块只保存在 RAM。
 * 5.4 保留 ID 与未知 ID 一律不回复，由 is_known_id() 的白名单保证。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <stddef.h>
#include <string.h>

#include "cmd_disp.h"

/* 通用结果码 payload。 */
#define CMD_RESULT_LEN (2U)
#define CMD_BYTE_OK (0x01U)
#define CMD_BYTE_FAIL (0x02U)

/* §4 消息 ID。 */
#define CMD_ID_DEV_INFO (0x0001U)
#define CMD_ID_DEV_STATE (0x0002U)
#define CMD_ID_BATTERY (0x0003U)
#define CMD_ID_GGA (0x0004U)
#define CMD_ID_SAT (0x0005U)
#define CMD_ID_SAT_TRACK (0x0006U)
#define CMD_ID_WORK_MODE (0x0007U)
#define CMD_ID_COM (0x000DU)
#define CMD_ID_RADIO (0x0014U)
#define CMD_ID_LOG (0x0015U)

/* 0x01 设备信息字段偏移（§4，字段间含保留空隙）。 */
#define CMD_DEV_OFS_PRODUCT (0U)
#define CMD_DEV_OFS_SN (8U)
#define CMD_DEV_OFS_HW_VER (24U)
#define CMD_DEV_OFS_FW_VER (32U)
#define CMD_DEV_OFS_BOARD (56U)
#define CMD_DEV_OFS_BOARD_SN (64U)
#define CMD_DEV_OFS_BOARD_FW (80U)
#define CMD_DEV_OFS_RADIO_TYPE (96U)
#define CMD_DEV_OFS_RADIO_FW (104U)

/* 0x02 设备状态常量字节与偏移。 */
#define CMD_DS_OFS_0 (0U)
#define CMD_DS_OFS_1 (1U)
#define CMD_DS_OFS_2 (2U)
#define CMD_DS_OFS_3 (3U)
#define CMD_DS_OFS_4 (4U)
#define CMD_DS_OFS_9 (9U)
#define CMD_DS_OFS_10 (10U)
#define CMD_DS_OFS_11 (11U)
#define CMD_DS_OFS_13 (13U)
#define CMD_DS_VAL_0 (0x03U)
#define CMD_DS_VAL_1 (0x01U)
#define CMD_DS_VAL_2 (0x81U)
#define CMD_DS_VAL_3 (0x83U)
#define CMD_DS_VAL_4 (0x81U)
#define CMD_DS_VAL_9 (0x81U)
#define CMD_DS_VAL_10 (0x81U)
#define CMD_DS_VAL_11 (0x81U)
#define CMD_DS_VAL_13 (0x01U)

/* 0x03 电池/存储偏移。 */
#define CMD_BAT_OFS_SOURCE (0U)
#define CMD_BAT_OFS_PCT (2U)
#define CMD_BAT_OFS_TEMP (12U)

/* 0x0D COM 口偏移与常量。 */
#define CMD_COM_OFS_0 (0U)
#define CMD_COM_OFS_1 (1U)
#define CMD_COM_OFS_BAUD (4U)
#define CMD_COM_OFS_8 (8U)
#define CMD_COM_OFS_27 (27U)
#define CMD_COM_OFS_218 (28U)
#define CMD_COM_PARAM_218 (218U)
#define CMD_COM_FLAG (0x01U)

/* 0x14 电台配置里的模式字节。 */
#define CMD_RADIO_OFS_MODE (17U)
#define CMD_RADIO_MODE_DEFAULT (0x05U)

/* 0x42 测试报警 payload。 */
#define CMD_TEST_SET_LEN (3U)
#define CMD_TEST_OFS_ACTION (0U)
#define CMD_TEST_OFS_STATE (1U)
#define CMD_TEST_OFS_LEVEL (2U)
#define CMD_TEST_ACTION_SET (0x01U)
#define CMD_TEST_ACTION_CLEAR (0x00U)

/* 0x44 配对设置/查询。 */
#define CMD_PAIR_SET_LEN (1U)
#define CMD_PAIR_OFS_COUNT (0U)
#define CMD_PAIR_OFS_FIRST (1U)
#define CMD_PAIR_QUERY_MAX                                                     \
    (CMD_PAIR_OFS_FIRST + (LINK_ADDR_LEN * LINK_MAX_ALARMS))

/* 本阶段占位/默认值。 */
#define CMD_DEFAULT_BAUD (115200U)
#define CMD_PHONE_HANDLE_MAX (1U)
#define CMD_ONE (1U)
#define CMD_BYTE_2 (2U)
#define CMD_BYTE_3 (3U)
#define CMD_SHIFT_8 (8U)
#define CMD_SHIFT_16 (16U)
#define CMD_SHIFT_24 (24U)

/* 从 idx = 0 起清零，避免以非字面量作循环初值时触发初始化检查。 */
static void
zero_bytes (uint8_t * p_buf, size_t len)
{
    for (size_t idx = 0U; idx < len; idx++)
    {
        p_buf[idx] = 0U;
    }
}

/* 按字段宽度逐字节拷贝（不要求源以 0 结尾）。 */
static void
copy_field (uint8_t * p_dst, char const * p_src, size_t width)
{
    for (size_t idx = 0U; idx < width; idx++)
    {
        /* char 的位模式按无符号字节写入 */
        p_dst[idx] = (uint8_t) p_src[idx];
    }
}

/* 按字段宽度拷贝以 0 结尾的源串：长度取 min(strlen, width)，其余补 0。
 * 读取不越过源串的结尾 0，避免从短字面量按固定宽度读越界。 */
static void
copy_str (char * p_dst, char const * p_src, size_t width)
{
    size_t idx = 0U;

    while ((idx < width) && ('\0' != p_src[idx]))
    {
        p_dst[idx] = p_src[idx];
        idx++;
    }
    while (idx < width)
    {
        p_dst[idx] = '\0';
        idx++;
    }
}

static void
write_u16_le (uint8_t * p_buf, uint16_t value)
{
    p_buf[0] = (uint8_t) value;
    /* 高 8 位，u8 截断安全 */
    p_buf[1] = (uint8_t) (value >> CMD_SHIFT_8);
}

static void
write_u32_le (uint8_t * p_buf, uint32_t value)
{
    /* 每个字节取低 8 位，u8 截断安全 */
    p_buf[0]          = (uint8_t) value;
    p_buf[1]          = (uint8_t) (value >> CMD_SHIFT_8);
    p_buf[CMD_BYTE_2] = (uint8_t) (value >> CMD_SHIFT_16);
    p_buf[CMD_BYTE_3] = (uint8_t) (value >> CMD_SHIFT_24);
}

/* 组应答帧；payload_len ≤ 136，u16 截断安全。 */
static cmd_result_t
build_reply (uint16_t msg_id, uint8_t req_type, uint8_t const * p_payload,
             size_t payload_len, cmd_reply_t * p_reply)
{
    size_t      out_len = 0U;
    proto_err_t err     = PROTO_OK;

    err = proto_frame_build(p_reply->buf, sizeof(p_reply->buf), msg_id,
                            PROTO_SENDER_DEV, proto_response_type(req_type),
                            p_payload, (uint16_t) payload_len, &out_len);
    if (PROTO_OK != err)
    {
        p_reply->len = 0U;
        return CMD_RES_NO_REPLY;
    }

    p_reply->len = out_len;
    return CMD_RES_REPLY;
}

static cmd_result_t
reply_result (uint16_t msg_id, uint8_t req_type, uint8_t value,
              cmd_reply_t * p_reply)
{
    uint8_t payload[CMD_RESULT_LEN];

    payload[0] = value;
    payload[1] = value;
    return build_reply(msg_id, req_type, payload, sizeof(payload), p_reply);
}

static cmd_result_t
reply_ok (uint16_t msg_id, uint8_t req_type, cmd_reply_t * p_reply)
{
    return reply_result(msg_id, req_type, CMD_BYTE_OK, p_reply);
}

static cmd_result_t
reply_fail (uint16_t msg_id, uint8_t req_type, cmd_reply_t * p_reply)
{
    return reply_result(msg_id, req_type, CMD_BYTE_FAIL, p_reply);
}

static cmd_result_t
handle_dev_info (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
                 cmd_reply_t * p_reply)
{
    uint8_t payload[CMD_DEV_INFO_LEN];

    zero_bytes(payload, sizeof(payload));

    copy_field(&payload[CMD_DEV_OFS_PRODUCT], p_ctx->info.product,
               sizeof(p_ctx->info.product));
    copy_field(&payload[CMD_DEV_OFS_SN], p_ctx->info.sn,
               sizeof(p_ctx->info.sn));
    copy_field(&payload[CMD_DEV_OFS_HW_VER], p_ctx->info.hw_ver,
               sizeof(p_ctx->info.hw_ver));
    copy_field(&payload[CMD_DEV_OFS_FW_VER], p_ctx->info.fw_ver,
               sizeof(p_ctx->info.fw_ver));
    copy_field(&payload[CMD_DEV_OFS_BOARD], p_ctx->info.board,
               sizeof(p_ctx->info.board));
    copy_field(&payload[CMD_DEV_OFS_BOARD_SN], p_ctx->info.board_sn,
               sizeof(p_ctx->info.board_sn));
    copy_field(&payload[CMD_DEV_OFS_BOARD_FW], p_ctx->info.board_fw,
               sizeof(p_ctx->info.board_fw));
    copy_field(&payload[CMD_DEV_OFS_RADIO_TYPE], p_ctx->info.radio_type,
               sizeof(p_ctx->info.radio_type));
    copy_field(&payload[CMD_DEV_OFS_RADIO_FW], p_ctx->info.radio_fw,
               sizeof(p_ctx->info.radio_fw));

    return build_reply(p_frame->msg_id, p_frame->type, payload, sizeof(payload),
                       p_reply);
}

static cmd_result_t
handle_dev_state (proto_frame_t const * p_frame, cmd_reply_t * p_reply)
{
    uint8_t payload[CMD_DEV_STATE_LEN];

    zero_bytes(payload, sizeof(payload));
    payload[CMD_DS_OFS_0]  = CMD_DS_VAL_0;
    payload[CMD_DS_OFS_1]  = CMD_DS_VAL_1;
    payload[CMD_DS_OFS_2]  = CMD_DS_VAL_2;
    payload[CMD_DS_OFS_3]  = CMD_DS_VAL_3;
    payload[CMD_DS_OFS_4]  = CMD_DS_VAL_4;
    payload[CMD_DS_OFS_9]  = CMD_DS_VAL_9;
    payload[CMD_DS_OFS_10] = CMD_DS_VAL_10;
    payload[CMD_DS_OFS_11] = CMD_DS_VAL_11;
    payload[CMD_DS_OFS_13] = CMD_DS_VAL_13;

    return build_reply(p_frame->msg_id, p_frame->type, payload, sizeof(payload),
                       p_reply);
}

static cmd_result_t
handle_battery (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
                cmd_reply_t * p_reply)
{
    uint8_t payload[CMD_BATTERY_LEN];

    zero_bytes(payload, sizeof(payload));
    payload[CMD_BAT_OFS_SOURCE] = (p_ctx->info.b_on_battery) ? CMD_ONE : 0U;
    payload[CMD_BAT_OFS_PCT]    = p_ctx->info.battery_pct;
    /* int8_t -5 → 0xFB，按补码位模式写入 */
    payload[CMD_BAT_OFS_TEMP] = (uint8_t) p_ctx->info.battery_temp_c;

    return build_reply(p_frame->msg_id, p_frame->type, payload, sizeof(payload),
                       p_reply);
}

static cmd_result_t
handle_ok_reply (proto_frame_t const * p_frame, cmd_reply_t * p_reply)
{
    return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
}

static cmd_result_t
handle_sat_track (proto_frame_t const * p_frame, cmd_reply_t * p_reply)
{
    uint8_t payload[CMD_SAT_TRACK_LEN];

    if (PROTO_TYPE_QUERY != p_frame->type)
    {
        return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
    }

    zero_bytes(payload, sizeof(payload));
    return build_reply(p_frame->msg_id, p_frame->type, payload, sizeof(payload),
                       p_reply);
}

static cmd_result_t
handle_work_mode (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
                  cmd_reply_t * p_reply)
{
    if (PROTO_TYPE_SET == p_frame->type)
    {
        if (p_frame->payload_len < CMD_WORK_MODE_SET)
        {
            return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
        }
        for (uint32_t idx = 0U; idx < CMD_WORK_MODE_SET; idx++)
        {
            p_ctx->work_mode[idx] = p_frame->p_payload[idx];
        }
        return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
    }

    return build_reply(p_frame->msg_id, p_frame->type, p_ctx->work_mode,
                       sizeof(p_ctx->work_mode), p_reply);
}

static cmd_result_t
handle_com (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
            cmd_reply_t * p_reply)
{
    uint8_t payload[CMD_COM_LEN];

    /* 0x0D 设置按 5.4 行为不回复。 */
    if (PROTO_TYPE_QUERY != p_frame->type)
    {
        return CMD_RES_NO_REPLY;
    }

    zero_bytes(payload, sizeof(payload));
    payload[CMD_COM_OFS_0] = CMD_COM_FLAG;
    payload[CMD_COM_OFS_1] = CMD_COM_FLAG;
    write_u32_le(&payload[CMD_COM_OFS_BAUD], p_ctx->info.com_baud);
    payload[CMD_COM_OFS_8]  = CMD_COM_FLAG;
    payload[CMD_COM_OFS_27] = CMD_COM_FLAG;
    write_u16_le(&payload[CMD_COM_OFS_218], CMD_COM_PARAM_218);

    return build_reply(p_frame->msg_id, p_frame->type, payload, sizeof(payload),
                       p_reply);
}

static cmd_result_t
handle_radio (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
              cmd_reply_t * p_reply)
{
    uint32_t limit = 0U;

    if (PROTO_TYPE_SET == p_frame->type)
    {
        if (p_frame->payload_len < CMD_RADIO_SET_MIN)
        {
            return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
        }
        limit = p_frame->payload_len;
        if (limit > CMD_RADIO_LEN)
        {
            limit = CMD_RADIO_LEN;
        }
        for (uint32_t idx = 0U; idx < limit; idx++)
        {
            p_ctx->radio_cfg[idx] = p_frame->p_payload[idx];
        }
        return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
    }

    return build_reply(p_frame->msg_id, p_frame->type, p_ctx->radio_cfg,
                       sizeof(p_ctx->radio_cfg), p_reply);
}

static cmd_result_t
handle_log (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
            cmd_reply_t * p_reply)
{
    if (PROTO_TYPE_SET == p_frame->type)
    {
        if (p_frame->payload_len < CMD_LOG_LEN)
        {
            return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
        }
        for (uint32_t idx = 0U; idx < CMD_LOG_LEN; idx++)
        {
            p_ctx->log_cfg[idx] = p_frame->p_payload[idx];
        }
        return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
    }

    return build_reply(p_frame->msg_id, p_frame->type, p_ctx->log_cfg,
                       sizeof(p_ctx->log_cfg), p_reply);
}

static cmd_result_t
handle_test_override (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
                      cmd_reply_t * p_reply)
{
    uint8_t action = 0U;

    if (PROTO_TYPE_SET != p_frame->type)
    {
        return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
    }
    if (p_frame->payload_len < CMD_TEST_SET_LEN)
    {
        return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
    }

    action = p_frame->p_payload[CMD_TEST_OFS_ACTION];
    if (CMD_TEST_ACTION_SET == action)
    {
        if (!alarm_src_set_override(p_ctx->p_alarm,
                                    p_frame->p_payload[CMD_TEST_OFS_STATE],
                                    p_frame->p_payload[CMD_TEST_OFS_LEVEL]))
        {
            return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
        }
        return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
    }
    if (CMD_TEST_ACTION_CLEAR == action)
    {
        alarm_src_clear_override(p_ctx->p_alarm);
        return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
    }

    return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
}

static cmd_result_t
handle_fence (proto_frame_t const * p_frame, cmd_reply_t * p_reply)
{
    /* 围栏本阶段预留：查询/设置均回失败。 */
    return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
}

static cmd_result_t
handle_pair (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame, uint32_t now_ms,
             cmd_reply_t * p_reply)
{
    uint8_t     payload[CMD_PAIR_QUERY_MAX];
    link_addr_t list[LINK_MAX_ALARMS];
    uint32_t    count = 0U;

    if (NULL == p_ctx->p_link)
    {
        return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
    }

    if (PROTO_TYPE_SET == p_frame->type)
    {
        if (p_frame->payload_len < CMD_PAIR_SET_LEN)
        {
            return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
        }
        if (CMD_TEST_ACTION_SET == p_frame->p_payload[0])
        {
            link_mgr_pair_open(p_ctx->p_link, now_ms);
            return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
        }
        if (CMD_TEST_ACTION_CLEAR == p_frame->p_payload[0])
        {
            link_mgr_whitelist_clear(p_ctx->p_link);
            p_reply->b_whitelist_changed = true;
            return reply_ok(p_frame->msg_id, p_frame->type, p_reply);
        }
        return reply_fail(p_frame->msg_id, p_frame->type, p_reply);
    }

    zero_bytes(payload, sizeof(payload));
    /* 返回数量 ≤ LINK_MAX_ALARMS(4)，转换为 uint32 仅扩宽 */
    count =
        (uint32_t) link_mgr_whitelist_get(p_ctx->p_link, list, LINK_MAX_ALARMS);
    /* count ≤ 4，u8 截断安全 */
    payload[CMD_PAIR_OFS_COUNT] = (uint8_t) count;
    for (uint32_t idx = 0U; idx < count; idx++)
    {
        for (uint32_t byte = 0U; byte < LINK_ADDR_LEN; byte++)
        {
            payload[CMD_PAIR_OFS_FIRST + (idx * LINK_ADDR_LEN) + byte] =
                list[idx].addr[byte];
        }
    }

    /* 1 + 4×6 = 25，u16 截断安全 */
    return build_reply(p_frame->msg_id, p_frame->type, payload,
                       (size_t) (CMD_PAIR_OFS_FIRST + (count * LINK_ADDR_LEN)),
                       p_reply);
}

/* 填写 0x40 状态（查询应答与主动上报共用）。 */
static void
fill_status (cmd_ctx_t const * p_ctx, uint16_t seq, uint32_t now_ms,
             alarm_status_t * p_status)
{
    uint32_t handles[CMD_PHONE_HANDLE_MAX];
    uint32_t flags = 0U;

    if ((NULL != p_ctx->p_link) &&
        (0U != link_mgr_handles(p_ctx->p_link, LINK_ROLE_PHONE, handles,
                                CMD_PHONE_HANDLE_MAX)))
    {
        flags |= ALARM_FLAG_PHONE_ONLINE;
    }
    if (alarm_src_is_override(p_ctx->p_alarm))
    {
        flags |= ALARM_FLAG_TEST_OVERRIDE;
    }

    p_status->seq         = seq;
    p_status->state       = alarm_src_state(p_ctx->p_alarm);
    p_status->level       = alarm_src_level(p_ctx->p_alarm);
    p_status->fix_quality = ALARM_VALUE_UNKNOWN;
    p_status->battery_pct = p_ctx->info.battery_pct;
    /* flags 只使用 bit0/bit1，u8 截断安全 */
    p_status->flags     = (uint8_t) flags;
    p_status->uptime_ms = now_ms;
}

static cmd_result_t
handle_status_query (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
                     uint32_t now_ms, cmd_reply_t * p_reply)
{
    uint8_t        payload[ALARM_STATUS_LEN];
    alarm_status_t status;

    fill_status(p_ctx, 0U, now_ms, &status);
    if (ALARM_STATUS_LEN !=
        alarm_msg_encode_status(&status, payload, sizeof(payload)))
    {
        return CMD_RES_NO_REPLY;
    }

    return build_reply(p_frame->msg_id, p_frame->type, payload, sizeof(payload),
                       p_reply);
}

static cmd_result_t
handle_heartbeat (proto_frame_t const * p_frame, cmd_reply_t * p_reply)
{
    if (alarm_msg_decode_heartbeat(p_frame->p_payload, p_frame->payload_len,
                                   &p_reply->hb))
    {
        p_reply->b_heartbeat = true;
    }
    /* 心跳/HELLO 按协议不回复。 */
    return CMD_RES_NO_REPLY;
}

static bool
is_known_id (uint16_t msg_id)
{
    switch (msg_id)
    {
        case CMD_ID_DEV_INFO:
        case CMD_ID_DEV_STATE:
        case CMD_ID_BATTERY:
        case CMD_ID_GGA:
        case CMD_ID_SAT:
        case CMD_ID_SAT_TRACK:
        case CMD_ID_WORK_MODE:
        case CMD_ID_COM:
        case CMD_ID_RADIO:
        case CMD_ID_LOG:
        case ALARM_MSG_STATUS_ID:
        case ALARM_MSG_HEARTBEAT_ID:
        case ALARM_MSG_TEST_ID:
        case ALARM_MSG_FENCE_ID:
        case ALARM_MSG_PAIR_ID:
            return true;

        default:
            return false;
    }
}

static bool
is_phone_command (uint16_t msg_id)
{
    switch (msg_id)
    {
        case CMD_ID_DEV_INFO:
        case CMD_ID_DEV_STATE:
        case CMD_ID_BATTERY:
        case CMD_ID_GGA:
        case CMD_ID_SAT:
        case CMD_ID_SAT_TRACK:
        case CMD_ID_WORK_MODE:
        case CMD_ID_COM:
        case CMD_ID_RADIO:
        case CMD_ID_LOG:
        case ALARM_MSG_TEST_ID:
        case ALARM_MSG_FENCE_ID:
        case ALARM_MSG_PAIR_ID:
            return true;

        default:
            return false;
    }
}

static cmd_result_t
dispatch_phone (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
                uint32_t now_ms, cmd_reply_t * p_reply)
{
    cmd_result_t result = CMD_RES_NO_REPLY;

    switch (p_frame->msg_id)
    {
        case CMD_ID_DEV_INFO:
            result = handle_dev_info(p_ctx, p_frame, p_reply);
            break;

        case CMD_ID_DEV_STATE:
            result = handle_dev_state(p_frame, p_reply);
            break;

        case CMD_ID_BATTERY:
            result = handle_battery(p_ctx, p_frame, p_reply);
            break;

        case CMD_ID_GGA:
        case CMD_ID_SAT:
            result = handle_ok_reply(p_frame, p_reply);
            break;

        case CMD_ID_SAT_TRACK:
            result = handle_sat_track(p_frame, p_reply);
            break;

        case CMD_ID_WORK_MODE:
            result = handle_work_mode(p_ctx, p_frame, p_reply);
            break;

        case CMD_ID_COM:
            result = handle_com(p_ctx, p_frame, p_reply);
            break;

        case CMD_ID_RADIO:
            result = handle_radio(p_ctx, p_frame, p_reply);
            break;

        case CMD_ID_LOG:
            result = handle_log(p_ctx, p_frame, p_reply);
            break;

        case ALARM_MSG_TEST_ID:
            result = handle_test_override(p_ctx, p_frame, p_reply);
            break;

        case ALARM_MSG_FENCE_ID:
            result = handle_fence(p_frame, p_reply);
            break;

        case ALARM_MSG_PAIR_ID:
            result = handle_pair(p_ctx, p_frame, now_ms, p_reply);
            break;

        default:
            result = CMD_RES_NO_REPLY;
            break;
    }
    return result;
}

static cmd_result_t
dispatch_alarm (cmd_ctx_t * p_ctx, proto_frame_t const * p_frame,
                uint32_t now_ms, cmd_reply_t * p_reply)
{
    if ((ALARM_MSG_HEARTBEAT_ID == p_frame->msg_id) &&
        (PROTO_TYPE_PUSH == p_frame->type))
    {
        return handle_heartbeat(p_frame, p_reply);
    }
    if ((ALARM_MSG_STATUS_ID == p_frame->msg_id) &&
        (PROTO_TYPE_QUERY == p_frame->type))
    {
        return handle_status_query(p_ctx, p_frame, now_ms, p_reply);
    }
    return CMD_RES_DENIED;
}

void
cmd_disp_init (cmd_ctx_t * p_ctx, alarm_src_t * p_alarm, link_mgr_t * p_link)
{
    if (NULL == p_ctx)
    {
        return;
    }

    memset(p_ctx, 0, sizeof(*p_ctx));

    /* 短字面量按 min(长度, 字段宽度) 拷贝，其余字节补 0 */
    copy_str(p_ctx->info.product, "E1-Lite", sizeof(p_ctx->info.product));
    copy_str(p_ctx->info.hw_ver, "V1.1.0", sizeof(p_ctx->info.hw_ver));
    copy_str(p_ctx->info.board, "UM980", sizeof(p_ctx->info.board));
    copy_str(p_ctx->info.radio_type, "5002D", sizeof(p_ctx->info.radio_type));

    p_ctx->info.b_on_battery             = true;
    p_ctx->info.battery_pct              = ALARM_VALUE_UNKNOWN;
    p_ctx->info.com_baud                 = CMD_DEFAULT_BAUD;
    p_ctx->radio_cfg[CMD_RADIO_OFS_MODE] = CMD_RADIO_MODE_DEFAULT;
    p_ctx->p_alarm                       = p_alarm;
    p_ctx->p_link                        = p_link;
}

cmd_result_t
cmd_disp_handle (cmd_ctx_t * p_ctx, link_role_t role,
                 proto_frame_t const * p_frame, uint32_t now_ms,
                 cmd_reply_t * p_reply)
{
    cmd_result_t result = CMD_RES_NO_REPLY;

    if ((NULL == p_ctx) || (NULL == p_frame) || (NULL == p_reply))
    {
        return CMD_RES_NO_REPLY;
    }
    memset(p_reply, 0, sizeof(*p_reply));

    if (!is_known_id(p_frame->msg_id))
    {
        return CMD_RES_NO_REPLY;
    }

    switch (role)
    {
        case LINK_ROLE_PHONE:
            if (!is_phone_command(p_frame->msg_id))
            {
                result = CMD_RES_DENIED;
            }
            else
            {
                result = dispatch_phone(p_ctx, p_frame, now_ms, p_reply);
            }
            break;

        case LINK_ROLE_ALARM:
            result = dispatch_alarm(p_ctx, p_frame, now_ms, p_reply);
            break;

        default:
            result = CMD_RES_DENIED;
            break;
    }
    return result;
}

bool
cmd_build_status_push (cmd_ctx_t const * p_ctx, uint16_t seq, uint32_t now_ms,
                       cmd_reply_t * p_reply)
{
    uint8_t        payload[ALARM_STATUS_LEN];
    alarm_status_t status;
    size_t         out_len = 0U;

    if ((NULL == p_ctx) || (NULL == p_reply))
    {
        return false;
    }
    memset(p_reply, 0, sizeof(*p_reply));

    fill_status(p_ctx, seq, now_ms, &status);
    if (ALARM_STATUS_LEN !=
        alarm_msg_encode_status(&status, payload, sizeof(payload)))
    {
        return false;
    }
    if (PROTO_OK != proto_frame_build(p_reply->buf, sizeof(p_reply->buf),
                                      ALARM_MSG_STATUS_ID, PROTO_SENDER_DEV,
                                      PROTO_TYPE_PUSH, payload,
                                      (uint16_t) sizeof(payload), &out_len))
    {
        return false;
    }

    p_reply->len = out_len;
    return true;
}

/*** end of file ***/
