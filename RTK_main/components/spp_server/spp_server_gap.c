/* GAP/SPP 回调分派：把 Bluedroid 事件转给各处理函数，记录认证结果并排队 PIN/CFM 应答。
 * 全部在 BTC 回调线程执行：只拷贝数据、记录状态、置事件位、非阻塞投递；
 * 不解析协议、不打印数据内容、不调用任何可能阻塞投递的 API。 */

#include <inttypes.h>
#include <stdint.h>

#include "esp_bt.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"

#include "spp_server_priv.h"

/* SPP 初始化结果：回调在 portMUX 下写状态、在临界区外置位，启动动作留给 init 任务。
 * 先写状态后置位，等待方拿到事件位后再在同一把锁下读取，因此迟到/早到两种顺序都拿到真实结果。 */
static void spp_server_handle_init(esp_spp_cb_param_t *param)
{
    taskENTER_CRITICAL(&s_spp_mux);
    s_spp.init_status = param->init.status;
    taskEXIT_CRITICAL(&s_spp_mux);

    if (param->init.status != ESP_SPP_SUCCESS) {
        ESP_LOGE(SPP_SERVER_TAG, "SPP init failed: %d", (int)param->init.status);
    }
    if (s_spp.rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_INIT_DONE);
    }
}

static void spp_server_handle_start(esp_spp_cb_param_t *param)
{
    taskENTER_CRITICAL(&s_spp_mux);
    s_spp.start_status = param->start.status;
    taskEXIT_CRITICAL(&s_spp_mux);

    if (param->start.status == ESP_SPP_SUCCESS) {
        ESP_LOGI(SPP_SERVER_TAG, "server started, scn=%u", (unsigned)param->start.scn);
    } else {
        ESP_LOGE(SPP_SERVER_TAG, "server start failed: %d", (int)param->start.status);
    }
    if (s_spp.rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_START_DONE);
    }
}

static void spp_server_handle_srv_open(esp_spp_cb_param_t *param)
{
    const uint32_t handle = param->srv_open.handle;

    if (param->srv_open.status != ESP_SPP_SUCCESS) {
        ESP_LOGE(SPP_SERVER_TAG, "srv open failed: %d", (int)param->srv_open.status);
        return;
    }

    if (spp_server_session_alloc(handle, param->srv_open.rem_bda) != NULL) {
        spp_server_evt_post(SPP_SERVER_EVT_OPEN, handle, param->srv_open.rem_bda, 0U);
        return;
    }

    /* 没有 users==0 且无在途发送的安全空槽（旧读者/写者未退出、上一会话仍占用槽）：
     * 不在回调里调用 esp_spp_disconnect（btc_transfer_context 可能无限阻塞），登记拒绝命令，
     * 由发送任务断开。不做延后接纳，避免 CLOSE 后把已死的连接重新发布为会话。 */
    if (!spp_server_ctrl_add(SPP_SERVER_CTRL_REJECT, handle, NULL)) {
        ESP_LOGE(SPP_SERVER_TAG, "no slot and ctrl table full, handle=%" PRIu32, handle);
    }
}

static void spp_server_handle_close(esp_spp_cb_param_t *param)
{
    const uint32_t handle = param->close.handle;

    spp_server_ctrl_drop_reject(handle);
    /* 未发布 OPEN 的拒绝连接没有应用生命周期，不能合成幽灵 CLOSE。 */
    if (spp_server_session_free(handle)) {
        spp_server_evt_post(SPP_SERVER_EVT_CLOSE, handle, NULL, 0U);
    }
}

void spp_server_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    switch (event) {
    case ESP_SPP_INIT_EVT:
        spp_server_handle_init(param);
        break;

    case ESP_SPP_START_EVT:
        spp_server_handle_start(param);
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        spp_server_handle_srv_open(param);
        break;

    case ESP_SPP_CLOSE_EVT:
        spp_server_handle_close(param);
        break;

    case ESP_SPP_DATA_IND_EVT:
        spp_server_handle_data(param);
        break;

    case ESP_SPP_CONG_EVT:
        spp_server_session_set_congested(param->cong.handle, param->cong.cong);
        break;

    case ESP_SPP_WRITE_EVT:
        spp_server_handle_write(param);
        break;

    case ESP_SPP_UNINIT_EVT:
        ESP_LOGI(SPP_SERVER_TAG, "SPP uninitialized");
        if (s_spp.rtos.evt_group != NULL) {
            (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_UNINIT_DONE);
        }
        break;

    default:
        break;
    }
}

/* GAP 回调：只记录身份/认证结果；PIN/CFM 应答经 btc_transfer_context 投递（可能在 BTC 队列上
 * 无限等待），因此复制地址到静态控制表、非阻塞唤醒发送任务，在任务上下文里应答。 */
void spp_server_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(SPP_SERVER_TAG, "auth success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(SPP_SERVER_TAG, "auth failed: %d", (int)param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT:
        if (!spp_server_ctrl_add(SPP_SERVER_CTRL_PIN_REQ, 0U, param->pin_req.bda)) {
            ESP_LOGE(SPP_SERVER_TAG, "pin req dropped: ctrl table full");
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        if (!spp_server_ctrl_add(SPP_SERVER_CTRL_CFM_REQ, 0U, param->cfm_req.bda)) {
            ESP_LOGE(SPP_SERVER_TAG, "ssp confirm dropped: ctrl table full");
        }
        break;

    default:
        /* 其余事件（含 ESP_BT_GAP_MODE_CHG_EVT）无需处理。 */
        break;
    }
}
