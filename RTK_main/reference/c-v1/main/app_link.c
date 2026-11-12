/**
 * @file    app_link.c
 * @brief   链路任务：SPP 事件、会话角色、命令分发、报警状态推送。
 *
 * 单一所有者：link_mgr/cmd_ctx/alarm_src 与解析器表只在链路任务内访问。
 * 控制台通过静态命令队列提交请求。时间统一由 app_now_ms() 提供，
 * u32 毫秒约 49.7 天回绕，所有比较使用无符号差值。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "alarm_msg.h"
#include "app_link.h"
#include "bt_spp.h"
#include "cmd_disp.h"
#include "link_mgr.h"
#include "proto_frame.h"
#include "store_nvs.h"

#define APP_LINK_TAG "app"
#define APP_LINK_STACK_BYTES (6144U)
#define APP_LINK_TASK_PRIO (10U)
#define APP_LINK_CORE (1)
#define APP_LINK_CMD_QUEUE_LEN (8U)
#define APP_EVT_POLL_MS (20U)
#define APP_TICK_PERIOD_MS (100U)
#define APP_WRITE_TIMEOUT_MS (200U)
#define APP_RX_CHUNK (64U)
#define APP_US_PER_MS (1000U)
#define APP_STATUS_SEQ_INIT (0U)
#define APP_ALARM_LOCAL_IDLE (0U)

typedef struct
{
    bool           b_used;
    uint32_t       handle;
    proto_parser_t parser;
} app_parser_slot_t;

typedef struct
{
    QueueHandle_t h_cmd_queue;
    TaskHandle_t  h_link_task;
} app_rtos_t;

link_mgr_t  g_app_link;
cmd_ctx_t   g_app_ctx;
alarm_src_t g_app_alarm;

static app_parser_slot_t g_parsers[BT_SPP_MAX_SESSIONS];
static app_rtos_t        g_rtos;

static StaticQueue_t g_cmd_queue_struct;
static uint8_t g_cmd_queue_storage[APP_LINK_CMD_QUEUE_LEN * sizeof(app_cmd_t)];

static StaticTask_t g_link_task_struct;
static StackType_t  g_link_stack[APP_LINK_STACK_BYTES / sizeof(StackType_t)];

static uint32_t g_rx_drop_total;
static uint16_t g_status_seq = APP_STATUS_SEQ_INIT;
static bool     g_b_running;
static uint8_t  g_alarm_battery     = ALARM_VALUE_UNKNOWN;
static uint8_t  g_alarm_local_state = APP_ALARM_LOCAL_IDLE;
static bool     g_b_alarm_hb_valid;

link_policy_t
app_link_policy (void)
{
#if CONFIG_RTK_LINK_POLICY_PHONE_PREEMPT
    return LINK_POLICY_PHONE_PREEMPT;
#else
    return LINK_POLICY_CONCURRENT;
#endif
}

char const *
app_link_policy_name (void)
{
#if CONFIG_RTK_LINK_POLICY_PHONE_PREEMPT
    return "PHONE_PREEMPT";
#else
    return "CONCURRENT";
#endif
}

/* esp_timer_get_time() 为微秒；除 1000 得毫秒。u32 约 49.7 天回绕。 */
static uint32_t
app_now_ms (void)
{
    /* 结果按 2^32 取模，回绕由无符号差值比较处理 */
    return (uint32_t) (esp_timer_get_time() / APP_US_PER_MS);
}

static app_parser_slot_t *
parser_alloc (uint32_t handle)
{
    for (uint32_t idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if (!g_parsers[idx].b_used)
        {
            g_parsers[idx].b_used = true;
            g_parsers[idx].handle = handle;
            proto_parser_init(&g_parsers[idx].parser);
            return &g_parsers[idx];
        }
    }
    return NULL;
}

static app_parser_slot_t *
parser_find (uint32_t handle)
{
    for (uint32_t idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if ((g_parsers[idx].b_used) && (g_parsers[idx].handle == handle))
        {
            return &g_parsers[idx];
        }
    }
    return NULL;
}

static void
parser_free (uint32_t handle)
{
    app_parser_slot_t * p_slot = parser_find(handle);

    if (NULL != p_slot)
    {
        p_slot->b_used = false;
    }
}

static void
execute_actions (link_actions_t const * p_act)
{
    for (uint8_t idx = 0U; idx < p_act->count; idx++)
    {
        esp_err_t err = bt_spp_disconnect(p_act->handles[idx]);

        if (ESP_OK != err)
        {
            ESP_LOGW(APP_LINK_TAG, "disconnect 0x%08" PRIx32 " failed: %s",
                     p_act->handles[idx], esp_err_to_name(err));
        }
    }
}

static void
persist_whitelist (void)
{
    link_addr_t list[LINK_MAX_ALARMS];
    uint8_t     addrs[LINK_MAX_ALARMS][LINK_ADDR_LEN];
    uint8_t count = link_mgr_whitelist_get(&g_app_link, list, LINK_MAX_ALARMS);
    esp_err_t err = ESP_OK;

    for (uint8_t idx = 0U; idx < count; idx++)
    {
        for (uint32_t byte = 0U; byte < LINK_ADDR_LEN; byte++)
        {
            addrs[idx][byte] = list[idx].addr[byte];
        }
    }

    err = store_set_alarms(addrs, count);
    if (ESP_OK != err)
    {
        ESP_LOGE(APP_LINK_TAG, "persist whitelist failed: %s",
                 esp_err_to_name(err));
    }
}

static void
handle_frame (uint32_t handle, proto_frame_t const * p_frame, uint32_t now_ms)
{
    link_actions_t act;
    link_role_t    role_before = link_mgr_role(&g_app_link, handle);
    link_role_t    role_after  = LINK_ROLE_NONE;
    cmd_reply_t    reply;
    cmd_result_t   result     = CMD_RES_NO_REPLY;
    bool           b_promoted = false;

    role_after = link_mgr_on_frame(&g_app_link, handle, p_frame->sender,
                                   p_frame->msg_id, now_ms, &act);
    execute_actions(&act);
    if (LINK_ROLE_NONE == role_after)
    {
        return;
    }

    result = cmd_disp_handle(&g_app_ctx, role_after, p_frame, now_ms, &reply);
    if (CMD_RES_REPLY == result)
    {
        esp_err_t err =
            bt_spp_write(handle, reply.buf, reply.len, APP_WRITE_TIMEOUT_MS);

        if (ESP_OK != err)
        {
            ESP_LOGW(APP_LINK_TAG, "write handle 0x%08" PRIx32 " failed: %s",
                     handle, esp_err_to_name(err));
        }
    }

    if (reply.b_heartbeat)
    {
        g_alarm_battery     = reply.hb.battery_pct;
        g_alarm_local_state = reply.hb.local_state;
        g_b_alarm_hb_valid  = true;
    }

    b_promoted =
        (LINK_ROLE_ALARM == role_after) && ((LINK_ROLE_PHONE == role_before) ||
                                            (LINK_ROLE_PENDING == role_before));
    if ((reply.b_whitelist_changed) || (b_promoted))
    {
        persist_whitelist();
    }
}

static void
handle_open (bt_spp_evt_t const * p_evt, uint32_t now_ms)
{
    link_addr_t    addr;
    link_actions_t act;
    link_role_t    role = LINK_ROLE_NONE;

    for (uint32_t byte = 0U; byte < LINK_ADDR_LEN; byte++)
    {
        addr.addr[byte] = p_evt->addr[byte];
    }

    role = link_mgr_on_open(&g_app_link, p_evt->handle, &addr, now_ms, &act);
    execute_actions(&act);
    if (LINK_ROLE_NONE == role)
    {
        return;
    }

    if (NULL == parser_alloc(p_evt->handle))
    {
        ESP_LOGE(APP_LINK_TAG, "no parser slot for handle 0x%08" PRIx32,
                 p_evt->handle);
        (void) bt_spp_disconnect(p_evt->handle);
    }
}

static void
handle_close (bt_spp_evt_t const * p_evt)
{
    link_mgr_on_close(&g_app_link, p_evt->handle);
    parser_free(p_evt->handle);
}

static void
handle_data (bt_spp_evt_t const * p_evt, uint32_t now_ms)
{
    app_parser_slot_t * p_slot = parser_find(p_evt->handle);
    uint8_t             chunk[APP_RX_CHUNK];
    size_t              count = 0U;

    if (NULL == p_slot)
    {
        return;
    }

    for (;;)
    {
        count = bt_spp_read(p_evt->handle, chunk, sizeof(chunk));
        if (0U == count)
        {
            break;
        }
        for (size_t idx = 0U; idx < count; idx++)
        {
            proto_frame_t        frame;
            proto_parse_result_t result =
                proto_parser_feed(&p_slot->parser, chunk[idx], &frame);

            if (PROTO_PARSE_FRAME == result)
            {
                handle_frame(p_evt->handle, &frame, now_ms);
            }
        }
    }
}

static void
handle_event (bt_spp_evt_t const * p_evt, uint32_t now_ms)
{
    switch (p_evt->type)
    {
        case BT_SPP_EVT_OPEN:
            handle_open(p_evt, now_ms);
            break;

        case BT_SPP_EVT_CLOSE:
            handle_close(p_evt);
            break;

        case BT_SPP_EVT_DATA:
            handle_data(p_evt, now_ms);
            break;

        case BT_SPP_EVT_RX_DROP:
            g_rx_drop_total += p_evt->dropped;
            ESP_LOGW(APP_LINK_TAG,
                     "rx drop handle 0x%08" PRIx32 " dropped=%" PRIu32,
                     p_evt->handle, p_evt->dropped);
            break;

        default:
            break;
    }
}

static void
push_alarm_status (uint32_t now_ms)
{
    uint32_t    handles[LINK_MAX_SESSIONS];
    uint8_t     count = link_mgr_handles(&g_app_link, LINK_ROLE_ALARM, handles,
                                         LINK_MAX_SESSIONS);
    cmd_reply_t reply;

    for (uint8_t idx = 0U; idx < count; idx++)
    {
        if (!cmd_build_status_push(&g_app_ctx, g_status_seq, now_ms, &reply))
        {
            continue;
        }
        (void) bt_spp_write(handles[idx], reply.buf, reply.len,
                            APP_WRITE_TIMEOUT_MS);
    }
    if (0U != count)
    {
        g_status_seq++;
    }
}

static char const *
role_name (link_role_t role)
{
    char const * p_name = "?";

    switch (role)
    {
        case LINK_ROLE_NONE:
            p_name = "NONE";
            break;

        case LINK_ROLE_PHONE:
            p_name = "PHONE";
            break;

        case LINK_ROLE_ALARM:
            p_name = "ALARM";
            break;

        case LINK_ROLE_PENDING:
            p_name = "PENDING";
            break;

        default:
            break;
    }
    return p_name;
}

/* 逐字节打印 MAC；uint8_t 提升为 unsigned 仅用于格式匹配。 */
static void
print_mac (uint8_t const * p_addr)
{
    for (uint32_t idx = 0U; idx < LINK_ADDR_LEN; idx++)
    {
        printf("%s%02X", (0U == idx) ? "" : ":", (unsigned) p_addr[idx]);
    }
}

static void
print_sessions (void)
{
    printf("sessions:\n");
    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        link_session_t const * p_session = &g_app_link.sessions[idx];

        if (!p_session->b_in_use)
        {
            continue;
        }
        printf("  h=0x%08" PRIx32 " role=%s mac=", p_session->handle,
               role_name(p_session->role));
        print_mac(p_session->addr.addr);
        printf("\n");
    }
}

static void
print_whitelist (void)
{
    link_addr_t list[LINK_MAX_ALARMS];
    uint8_t count = link_mgr_whitelist_get(&g_app_link, list, LINK_MAX_ALARMS);

    printf("whitelist count=%u\n", (unsigned) count);
    for (uint8_t idx = 0U; idx < count; idx++)
    {
        printf("  ");
        print_mac(list[idx].addr);
        printf("\n");
    }
}

static void
print_status (void)
{
    uint32_t crc_errors = 0U;
    uint32_t len_errors = 0U;

    print_sessions();
    print_whitelist();

    for (uint32_t idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if (g_parsers[idx].b_used)
        {
            crc_errors += g_parsers[idx].parser.crc_errors;
            len_errors += g_parsers[idx].parser.len_errors;
        }
    }

    /* bool/uint8_t 提升为 unsigned 仅用于打印 */
    printf("pairing=%s alarm_state=0x%02X level=%u override=%s\n",
           link_mgr_is_pairing(&g_app_link) ? "yes" : "no",
           (unsigned) alarm_src_state(&g_app_alarm),
           (unsigned) alarm_src_level(&g_app_alarm),
           alarm_src_is_override(&g_app_alarm) ? "yes" : "no");
    printf("heartbeat valid=%s battery=%u local=%u\n",
           g_b_alarm_hb_valid ? "yes" : "no", (unsigned) g_alarm_battery,
           (unsigned) g_alarm_local_state);
    printf("errors crc=%" PRIu32 " len=%" PRIu32 " rx_drop=%" PRIu32 "\n",
           crc_errors, len_errors, g_rx_drop_total);
    printf("heap free=%" PRIu32 " min=%" PRIu32 " link_stack_hwm=%u\n",
           esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
           (unsigned) uxTaskGetStackHighWaterMark(g_rtos.h_link_task));
}

static void
handle_cmd (app_cmd_t const * p_cmd)
{
    esp_err_t err = ESP_OK;

    switch (p_cmd->id)
    {
        case APP_CMD_STATUS:
            print_status();
            break;

        case APP_CMD_ALARM_SET:
            if (alarm_src_set_override(&g_app_alarm, p_cmd->state,
                                       p_cmd->level))
            {
                printf("alarm override set\n");
            }
            else
            {
                printf("alarm override invalid\n");
            }
            break;

        case APP_CMD_ALARM_CLEAR:
            alarm_src_clear_override(&g_app_alarm);
            printf("alarm override cleared\n");
            break;

        case APP_CMD_PAIR:
            link_mgr_pair_open(&g_app_link, app_now_ms());
            printf("pair window opened\n");
            break;

        case APP_CMD_WL_CLEAR:
            link_mgr_whitelist_clear(&g_app_link);
            persist_whitelist();
            printf("whitelist cleared\n");
            break;

        case APP_CMD_SN:
            err = store_set_sn(p_cmd->sn);
            if (ESP_OK == err)
            {
                printf("sn saved, reboot to apply\n");
            }
            else
            {
                printf("sn save failed: %s\n", esp_err_to_name(err));
            }
            break;

        default:
            printf("unknown command\n");
            break;
    }

    if (NULL != p_cmd->reply_task)
    {
        (void) xTaskNotifyGive(p_cmd->reply_task);
    }
}

static void
app_link_task (void * p_arg)
{
    uint32_t last_tick_ms = app_now_ms();
    uint32_t last_push_ms = last_tick_ms;

    (void) p_arg;

    for (;;)
    {
        bt_spp_evt_t evt;
        uint32_t     now_ms = 0U;

        if (bt_spp_poll_event(&evt, APP_EVT_POLL_MS))
        {
            handle_event(&evt, app_now_ms());
        }

        now_ms = app_now_ms();
        if ((uint32_t) (now_ms - last_tick_ms) >= APP_TICK_PERIOD_MS)
        {
            link_actions_t act;

            last_tick_ms = now_ms;
            link_mgr_tick(&g_app_link, now_ms, &act);
            execute_actions(&act);
        }

        if ((uint32_t) (now_ms - last_push_ms) >= ALARM_PUSH_PERIOD_MS)
        {
            last_push_ms = now_ms;
            push_alarm_status(now_ms);
        }

        for (;;)
        {
            app_cmd_t cmd;

            if (pdTRUE != xQueueReceive(g_rtos.h_cmd_queue, &cmd, 0))
            {
                break;
            }
            handle_cmd(&cmd);
        }
    }
}

esp_err_t
app_link_start (void)
{
    if (g_b_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(g_parsers, 0, sizeof(g_parsers));
    g_rtos.h_cmd_queue =
        xQueueCreateStatic(APP_LINK_CMD_QUEUE_LEN, sizeof(app_cmd_t),
                           g_cmd_queue_storage, &g_cmd_queue_struct);
    if (NULL == g_rtos.h_cmd_queue)
    {
        return ESP_ERR_NO_MEM;
    }

    g_rtos.h_link_task = xTaskCreateStaticPinnedToCore(
        app_link_task, "link", APP_LINK_STACK_BYTES / sizeof(StackType_t), NULL,
        APP_LINK_TASK_PRIO, g_link_stack, &g_link_task_struct, APP_LINK_CORE);
    if (NULL == g_rtos.h_link_task)
    {
        vQueueDelete(g_rtos.h_cmd_queue);
        g_rtos.h_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_b_running = true;
    return ESP_OK;
}

bool
app_link_is_running (void)
{
    return g_b_running;
}

bool
app_link_post_cmd (app_cmd_t const * p_cmd, uint32_t timeout_ms)
{
    if ((NULL == p_cmd) || (NULL == g_rtos.h_cmd_queue) || (!g_b_running))
    {
        return false;
    }

    return (pdTRUE ==
            xQueueSend(g_rtos.h_cmd_queue, p_cmd, pdMS_TO_TICKS(timeout_ms)));
}

/*** end of file ***/
