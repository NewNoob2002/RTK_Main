/**
 * @file    bt_spp.c
 * @brief   Bluedroid 经典蓝牙 SPP 多会话服务端封装（driver/bus 层）。
 *
 * 线程模型见 bt_spp.h：BTC 回调只拷贝数据、更新拥塞标志、投递事件；
 * 链路任务独占 bt_spp_poll_event()/bt_spp_read()/bt_spp_write()。
 * 会话表与拥塞标志使用 portMUX 临界区保护，回调中不做阻塞操作。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"

#include "bt_spp.h"

/* 日志 tag（任务要求固定为 "bt_spp"）。 */
#define BT_SPP_TAG "bt_spp"

#define BT_SPP_SERVER_NAME "SPP_SERVER"
#define BT_SPP_EVT_QUEUE_LEN (16U)
#define BT_SPP_BDA_LEN (6U)
#define BT_SPP_STREAM_TRIGGER (1U)
#define BT_SPP_PIN_LEN (4U)
#define BT_SPP_PIN_DIGIT '0'

/* 每个会话一个拥塞解除事件位（最多 BT_SPP_MAX_SESSIONS 个）。 */
#define BT_SPP_CONG_BIT(idx) ((EventBits_t) (1UL << (idx)))

typedef struct
{
    bool                 b_in_use;
    uint32_t             handle;
    uint8_t              addr[BT_SPP_BDA_LEN];
    bool                 b_congested;
    uint8_t              idx;
    StreamBufferHandle_t h_rx;
    StaticStreamBuffer_t rx_struct;
    uint8_t              rx_storage[BT_SPP_RX_BUF_SIZE];
} bt_spp_session_t;

static bt_spp_session_t g_sessions[BT_SPP_MAX_SESSIONS];
static portMUX_TYPE     g_session_mux = portMUX_INITIALIZER_UNLOCKED;

static StaticQueue_t g_evt_queue_struct;
static uint8_t g_evt_queue_storage[BT_SPP_EVT_QUEUE_LEN * sizeof(bt_spp_evt_t)];

static StaticEventGroup_t g_cong_group_struct;

/* 句柄放在结构体内，避免全局指针命名规则与 BARR-C 句柄前缀冲突。 */
typedef struct
{
    QueueHandle_t      h_evt_queue;
    EventGroupHandle_t h_cong_group;
} bt_spp_rtos_t;

static bt_spp_rtos_t g_rtos;

static char     g_device_name[BT_SPP_NAME_MAX];
static uint32_t g_evt_dropped;
static bool     g_b_initialized;

static bt_spp_session_t *
bt_spp_session_find (uint32_t handle)
{
    bt_spp_session_t * p_found = NULL;

    portENTER_CRITICAL(&g_session_mux);
    for (uint32_t idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if ((g_sessions[idx].b_in_use) && (g_sessions[idx].handle == handle))
        {
            p_found = &g_sessions[idx];
            break;
        }
    }
    portEXIT_CRITICAL(&g_session_mux);
    return p_found;
}

/* 只在 BTC 任务中调用；字段写完后再置 in_use，读者不会看到半初始化会话。 */
static bt_spp_session_t *
bt_spp_session_alloc (uint32_t handle, uint8_t const * p_addr)
{
    bt_spp_session_t * p_slot = NULL;
    uint32_t           idx    = 0U;

    portENTER_CRITICAL(&g_session_mux);
    for (idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if (!g_sessions[idx].b_in_use)
        {
            p_slot = &g_sessions[idx];
            break;
        }
    }
    portEXIT_CRITICAL(&g_session_mux);

    if (NULL == p_slot)
    {
        return NULL;
    }

    (void) xStreamBufferReset(p_slot->h_rx);
    p_slot->handle = handle;
    for (uint32_t byte = 0U; byte < BT_SPP_BDA_LEN; byte++)
    {
        p_slot->addr[byte] = p_addr[byte];
    }
    p_slot->b_congested = false;
    /* idx < BT_SPP_MAX_SESSIONS(2)，u8 截断安全 */
    p_slot->idx = (uint8_t) idx;

    portENTER_CRITICAL(&g_session_mux);
    p_slot->b_in_use = true;
    portEXIT_CRITICAL(&g_session_mux);
    return p_slot;
}

static void
bt_spp_session_free (uint32_t handle)
{
    bool    b_found = false;
    uint8_t idx     = 0U;

    portENTER_CRITICAL(&g_session_mux);
    for (idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if ((g_sessions[idx].b_in_use) && (g_sessions[idx].handle == handle))
        {
            g_sessions[idx].b_in_use    = false;
            g_sessions[idx].b_congested = false;
            b_found                     = true;
            break;
        }
    }
    portEXIT_CRITICAL(&g_session_mux);

    if (b_found)
    {
        (void) xEventGroupSetBits(g_rtos.h_cong_group, BT_SPP_CONG_BIT(idx));
    }
}

static void
bt_spp_session_set_congested (uint32_t handle, bool congested)
{
    bool    b_found = false;
    uint8_t idx     = 0U;

    portENTER_CRITICAL(&g_session_mux);
    for (idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if ((g_sessions[idx].b_in_use) && (g_sessions[idx].handle == handle))
        {
            g_sessions[idx].b_congested = congested;
            b_found                     = true;
            break;
        }
    }
    portEXIT_CRITICAL(&g_session_mux);

    if (b_found)
    {
        if (congested)
        {
            (void) xEventGroupClearBits(g_rtos.h_cong_group,
                                        BT_SPP_CONG_BIT(idx));
        }
        else
        {
            (void) xEventGroupSetBits(g_rtos.h_cong_group,
                                      BT_SPP_CONG_BIT(idx));
        }
    }
}

static void
bt_spp_evt_post (bt_spp_evt_type_t type, uint32_t handle,
                 uint8_t const * p_addr, uint32_t dropped)
{
    bt_spp_evt_t evt;

    evt.type    = type;
    evt.handle  = handle;
    evt.dropped = dropped;
    for (uint32_t idx = 0U; idx < BT_SPP_BDA_LEN; idx++)
    {
        evt.addr[idx] = (NULL != p_addr) ? p_addr[idx] : 0U;
    }

    /* 回调上下文中不得阻塞：队列满只计数并记日志。 */
    if (pdPASS != xQueueSend(g_rtos.h_evt_queue, &evt, 0))
    {
        g_evt_dropped++;
        ESP_LOGW(BT_SPP_TAG, "event queue full, total dropped=%" PRIu32,
                 g_evt_dropped);
    }
}

/* 初始化失败时按逆序释放已初始化部分。 */
static void
bt_spp_unwind (bool b_bluedroid_enabled, bool b_bluedroid_inited,
               bool b_controller_enabled, bool b_controller_inited)
{
    esp_err_t err = ESP_OK;

    if (b_bluedroid_enabled)
    {
        err = esp_bluedroid_disable();
        if (ESP_OK != err)
        {
            ESP_LOGW(BT_SPP_TAG, "bluedroid disable: %s", esp_err_to_name(err));
        }
    }
    if (b_bluedroid_inited)
    {
        err = esp_bluedroid_deinit();
        if (ESP_OK != err)
        {
            ESP_LOGW(BT_SPP_TAG, "bluedroid deinit: %s", esp_err_to_name(err));
        }
    }
    if (b_controller_enabled)
    {
        err = esp_bt_controller_disable();
        if (ESP_OK != err)
        {
            ESP_LOGW(BT_SPP_TAG, "controller disable: %s",
                     esp_err_to_name(err));
        }
    }
    if (b_controller_inited)
    {
        err = esp_bt_controller_deinit();
        if (ESP_OK != err)
        {
            ESP_LOGW(BT_SPP_TAG, "controller deinit: %s", esp_err_to_name(err));
        }
    }
}

static void
bt_spp_objects_delete (void)
{
    for (uint32_t idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        if (NULL != g_sessions[idx].h_rx)
        {
            vStreamBufferDelete(g_sessions[idx].h_rx);
            g_sessions[idx].h_rx = NULL;
        }
        g_sessions[idx].b_in_use = false;
        g_sessions[idx].handle   = 0U;
    }
    if (NULL != g_rtos.h_evt_queue)
    {
        vQueueDelete(g_rtos.h_evt_queue);
        g_rtos.h_evt_queue = NULL;
    }
    if (NULL != g_rtos.h_cong_group)
    {
        vEventGroupDelete(g_rtos.h_cong_group);
        g_rtos.h_cong_group = NULL;
    }
    g_b_initialized = false;
}

/* SPP 初始化完成：启动服务器、设置设备名与可发现模式。 */
static void
bt_spp_handle_init (esp_spp_cb_param_t * p_param)
{
    esp_err_t err = ESP_OK;

    if (ESP_SPP_SUCCESS != p_param->init.status)
    {
        ESP_LOGE(BT_SPP_TAG, "SPP init failed: %d", p_param->init.status);
        return;
    }

    err = esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0,
                            BT_SPP_SERVER_NAME);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "start srv failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_bt_gap_set_device_name(g_device_name);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "set name failed: %s", esp_err_to_name(err));
    }
    err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                   ESP_BT_GENERAL_DISCOVERABLE);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "set scan mode failed: %s", esp_err_to_name(err));
    }
}

static void
bt_spp_handle_srv_open (esp_spp_cb_param_t * p_param)
{
    uint32_t           handle    = p_param->srv_open.handle;
    bt_spp_session_t * p_session = NULL;

    if (ESP_SPP_SUCCESS != p_param->srv_open.status)
    {
        ESP_LOGE(BT_SPP_TAG, "srv open failed: %d", p_param->srv_open.status);
        return;
    }

    p_session = bt_spp_session_alloc(handle, p_param->srv_open.rem_bda);
    if (NULL == p_session)
    {
        ESP_LOGE(BT_SPP_TAG, "no session slot, disconnect handle=%" PRIu32,
                 handle);
        (void) esp_spp_disconnect(handle);
        return;
    }

    bt_spp_evt_post(BT_SPP_EVT_OPEN, handle, p_param->srv_open.rem_bda, 0U);
}

static void
bt_spp_handle_close (esp_spp_cb_param_t * p_param)
{
    uint32_t handle = p_param->close.handle;

    bt_spp_session_free(handle);
    bt_spp_evt_post(BT_SPP_EVT_CLOSE, handle, NULL, 0U);
}

static void
bt_spp_handle_data (esp_spp_cb_param_t * p_param)
{
    uint32_t           handle    = p_param->data_ind.handle;
    bt_spp_session_t * p_session = NULL;
    size_t             len       = 0U;
    size_t             written   = 0U;

    if (ESP_SPP_SUCCESS != p_param->data_ind.status)
    {
        return;
    }
    if (NULL == p_param->data_ind.data)
    {
        return;
    }

    p_session = bt_spp_session_find(handle);
    if (NULL == p_session)
    {
        return;
    }

    /* data_ind.len 为 uint16_t，扩宽到 size_t 无损 */
    len = (size_t) p_param->data_ind.len;
    written =
        xStreamBufferSend(p_session->h_rx, p_param->data_ind.data, len, 0);
    if (0U != written)
    {
        bt_spp_evt_post(BT_SPP_EVT_DATA, handle, NULL, 0U);
    }
    if (written < len)
    {
        /* 丢失字节 ≤ 65535，u32 无损 */
        uint32_t dropped = (uint32_t) (len - written);

        bt_spp_evt_post(BT_SPP_EVT_RX_DROP, handle, NULL, dropped);
        ESP_LOGW(BT_SPP_TAG, "rx overflow handle=%" PRIu32 " dropped=%" PRIu32,
                 handle, dropped);
    }
}

static void
bt_spp_handle_write (esp_spp_cb_param_t * p_param)
{
    if (ESP_SPP_SUCCESS != p_param->write.status)
    {
        ESP_LOGW(BT_SPP_TAG,
                 "write failed: handle=%" PRIu32 " len=%d"
                 " status=%d",
                 p_param->write.handle, p_param->write.len,
                 p_param->write.status);
    }
}

static void
bt_spp_cb (esp_spp_cb_event_t event, esp_spp_cb_param_t * p_param)
{
    switch (event)
    {
        case ESP_SPP_INIT_EVT:
            bt_spp_handle_init(p_param);
            break;

        case ESP_SPP_START_EVT:
            if (ESP_SPP_SUCCESS == p_param->start.status)
            {
                ESP_LOGI(BT_SPP_TAG, "server started, scn=%u",
                         p_param->start.scn);
            }
            else
            {
                ESP_LOGE(BT_SPP_TAG, "server start failed: %d",
                         p_param->start.status);
            }
            break;

        case ESP_SPP_SRV_OPEN_EVT:
            bt_spp_handle_srv_open(p_param);
            break;

        case ESP_SPP_CLOSE_EVT:
            bt_spp_handle_close(p_param);
            break;

        case ESP_SPP_DATA_IND_EVT:
            bt_spp_handle_data(p_param);
            break;

        case ESP_SPP_CONG_EVT:
            bt_spp_session_set_congested(p_param->cong.handle,
                                         p_param->cong.cong);
            break;

        case ESP_SPP_WRITE_EVT:
            bt_spp_handle_write(p_param);
            break;

        case ESP_SPP_UNINIT_EVT:
            ESP_LOGI(BT_SPP_TAG, "SPP uninitialized");
            break;

        default:
            break;
    }
}

/* GAP 回调：SSP IO 能力 NONE 时自动确认；PIN 请求回固定码以兼容旧设备。 */
static void
bt_spp_gap_cb (esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t * p_param)
{
    esp_bt_pin_code_t pin_code = {BT_SPP_PIN_DIGIT, BT_SPP_PIN_DIGIT,
                                  BT_SPP_PIN_DIGIT, BT_SPP_PIN_DIGIT};
    esp_err_t         err      = ESP_OK;

    switch (event)
    {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (ESP_BT_STATUS_SUCCESS == p_param->auth_cmpl.stat)
            {
                ESP_LOGI(BT_SPP_TAG, "auth success: %s",
                         p_param->auth_cmpl.device_name);
            }
            else
            {
                ESP_LOGW(BT_SPP_TAG, "auth failed: %d",
                         p_param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_PIN_REQ_EVT:
            err = esp_bt_gap_pin_reply(p_param->pin_req.bda, true,
                                       BT_SPP_PIN_LEN, pin_code);
            if (ESP_OK != err)
            {
                ESP_LOGW(BT_SPP_TAG, "pin reply: %s", esp_err_to_name(err));
            }
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            err = esp_bt_gap_ssp_confirm_reply(p_param->cfm_req.bda, true);
            if (ESP_OK != err)
            {
                ESP_LOGW(BT_SPP_TAG, "ssp confirm: %s", esp_err_to_name(err));
            }
            break;

        case ESP_BT_GAP_MODE_CHG_EVT:
            break;

        default:
            break;
    }
}

typedef struct
{
    bool b_controller_inited;
    bool b_controller_enabled;
    bool b_bluedroid_inited;
    bool b_bluedroid_enabled;
} bt_spp_hw_state_t;

static void
bt_spp_hw_state_clear (bt_spp_hw_state_t * p_state)
{
    p_state->b_controller_inited  = false;
    p_state->b_controller_enabled = false;
    p_state->b_bluedroid_inited   = false;
    p_state->b_bluedroid_enabled  = false;
}

/* 控制器 + Bluedroid。失败时在内部完成逆序释放。 */
static esp_err_t
bt_spp_hw_init (bt_spp_hw_state_t * p_state)
{
    esp_err_t                  err    = ESP_OK;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t     bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();

    err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "mem release: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_init(&bt_cfg);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "controller init: %s", esp_err_to_name(err));
        return err;
    }
    p_state->b_controller_inited = true;

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "controller enable: %s", esp_err_to_name(err));
        bt_spp_unwind(p_state->b_bluedroid_enabled, p_state->b_bluedroid_inited,
                      p_state->b_controller_enabled,
                      p_state->b_controller_inited);
        bt_spp_hw_state_clear(p_state);
        return err;
    }
    p_state->b_controller_enabled = true;

    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "bluedroid init: %s", esp_err_to_name(err));
        bt_spp_unwind(p_state->b_bluedroid_enabled, p_state->b_bluedroid_inited,
                      p_state->b_controller_enabled,
                      p_state->b_controller_inited);
        bt_spp_hw_state_clear(p_state);
        return err;
    }
    p_state->b_bluedroid_inited = true;

    err = esp_bluedroid_enable();
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "bluedroid enable: %s", esp_err_to_name(err));
        bt_spp_unwind(p_state->b_bluedroid_enabled, p_state->b_bluedroid_inited,
                      p_state->b_controller_enabled,
                      p_state->b_controller_inited);
        bt_spp_hw_state_clear(p_state);
        return err;
    }
    p_state->b_bluedroid_enabled = true;

    return ESP_OK;
}

/* GAP/SPP 回调、SSP 参数与 SPP 初始化。失败时由调用方统一 unwind。 */
static esp_err_t
bt_spp_profile_init (void)
{
    esp_err_t         err      = ESP_OK;
    esp_bt_io_cap_t   iocap    = ESP_BT_IO_CAP_NONE;
    esp_bt_pin_code_t pin_code = {0};
    esp_spp_cfg_t     spp_cfg  = {0};

    err = esp_bt_gap_register_callback(bt_spp_gap_cb);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "gap callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_spp_register_callback(bt_spp_cb);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "spp callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap,
                                        sizeof(iocap));
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "set iocap: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "set pin: %s", esp_err_to_name(err));
        return err;
    }

    spp_cfg.mode              = ESP_SPP_MODE_CB;
    spp_cfg.enable_l2cap_ertm = true;
    spp_cfg.tx_buffer_size    = 0U;
    err                       = esp_spp_enhanced_init(&spp_cfg);
    if (ESP_OK != err)
    {
        ESP_LOGE(BT_SPP_TAG, "spp init: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t
bt_spp_init (char const * p_name)
{
    esp_err_t         err      = ESP_OK;
    bt_spp_hw_state_t hw_state = {false, false, false, false};
    size_t            name_len = 0U;

    if (NULL == p_name)
    {
        return ESP_ERR_INVALID_ARG;
    }
    name_len = strlen(p_name);
    if ((0U == name_len) || (name_len >= BT_SPP_NAME_MAX))
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t idx = 0U; idx < name_len; idx++)
    {
        if (('\n' == p_name[idx]) || ('\r' == p_name[idx]))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (g_b_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(g_sessions, 0, sizeof(g_sessions));
    g_rtos.h_evt_queue =
        xQueueCreateStatic(BT_SPP_EVT_QUEUE_LEN, sizeof(bt_spp_evt_t),
                           g_evt_queue_storage, &g_evt_queue_struct);
    g_rtos.h_cong_group = xEventGroupCreateStatic(&g_cong_group_struct);
    for (uint32_t idx = 0U; idx < BT_SPP_MAX_SESSIONS; idx++)
    {
        /* idx < 2，u8 截断安全 */
        g_sessions[idx].idx  = (uint8_t) idx;
        g_sessions[idx].h_rx = xStreamBufferCreateStatic(
            BT_SPP_RX_BUF_SIZE, BT_SPP_STREAM_TRIGGER,
            g_sessions[idx].rx_storage, &g_sessions[idx].rx_struct);
    }
    if ((NULL == g_rtos.h_evt_queue) || (NULL == g_rtos.h_cong_group) ||
        (NULL == g_sessions[0].h_rx) || (NULL == g_sessions[1].h_rx))
    {
        bt_spp_objects_delete();
        return ESP_ERR_NO_MEM;
    }

    for (size_t idx = 0U; idx < BT_SPP_NAME_MAX; idx++)
    {
        g_device_name[idx] = (idx < name_len) ? p_name[idx] : '\0';
    }

    err = bt_spp_hw_init(&hw_state);
    if (ESP_OK != err)
    {
        bt_spp_objects_delete();
        return err;
    }

    err = bt_spp_profile_init();
    if (ESP_OK != err)
    {
        bt_spp_unwind(hw_state.b_bluedroid_enabled, hw_state.b_bluedroid_inited,
                      hw_state.b_controller_enabled,
                      hw_state.b_controller_inited);
        bt_spp_objects_delete();
        return err;
    }

    g_b_initialized = true;
    return ESP_OK;
}

bool
bt_spp_poll_event (bt_spp_evt_t * p_evt, uint32_t timeout_ms)
{
    if ((NULL == p_evt) || (NULL == g_rtos.h_evt_queue))
    {
        return false;
    }

    return (pdTRUE == xQueueReceive(g_rtos.h_evt_queue, p_evt,
                                    pdMS_TO_TICKS(timeout_ms)));
}

size_t
bt_spp_read (uint32_t handle, uint8_t * p_buf, size_t size)
{
    bt_spp_session_t * p_session = NULL;

    if ((NULL == p_buf) || (0U == size))
    {
        return 0U;
    }

    p_session = bt_spp_session_find(handle);
    if (NULL == p_session)
    {
        return 0U;
    }

    return xStreamBufferReceive(p_session->h_rx, p_buf, size, 0);
}

static esp_err_t
bt_spp_wait_uncongested (bt_spp_session_t * p_session, uint32_t timeout_ms)
{
    TickType_t start         = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        bool       b_congested = false;
        TickType_t elapsed     = 0U;

        portENTER_CRITICAL(&g_session_mux);
        b_congested = p_session->b_congested;
        portEXIT_CRITICAL(&g_session_mux);

        if (!b_congested)
        {
            return ESP_OK;
        }

        elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout_ticks)
        {
            return ESP_ERR_TIMEOUT;
        }
        (void) xEventGroupWaitBits(g_rtos.h_cong_group,
                                   BT_SPP_CONG_BIT(p_session->idx), pdFALSE,
                                   pdTRUE, timeout_ticks - elapsed);
    }
}

esp_err_t
bt_spp_write (uint32_t handle, uint8_t const * p_data, size_t len,
              uint32_t timeout_ms)
{
    bt_spp_session_t * p_session = NULL;
    esp_err_t          err       = ESP_OK;

    if ((NULL == p_data) || (0U == len))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > (size_t) INT_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    p_session = bt_spp_session_find(handle);
    if (NULL == p_session)
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = bt_spp_wait_uncongested(p_session, timeout_ms);
    if (ESP_OK != err)
    {
        return err;
    }

    /* IDF 形参非 const；btc_spp_arg_deep_copy 会立即深拷贝，回调返回后
     * p_data 不再被引用。len ≤ INT_MAX 已在上面检查。 */
    return esp_spp_write(handle, (int) len, (uint8_t *) p_data);
}

esp_err_t
bt_spp_disconnect (uint32_t handle)
{
    return esp_spp_disconnect(handle);
}

esp_err_t
bt_spp_get_local_addr (uint8_t p_addr[BT_SPP_BDA_LEN])
{
    uint8_t const * p_local = NULL;

    if (NULL == p_addr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    p_local = esp_bt_dev_get_address();
    if (NULL == p_local)
    {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t idx = 0U; idx < BT_SPP_BDA_LEN; idx++)
    {
        p_addr[idx] = p_local[idx];
    }
    return ESP_OK;
}

/*** end of file ***/
