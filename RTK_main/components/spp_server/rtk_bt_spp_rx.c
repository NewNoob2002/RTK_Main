/* 接收路径：BTC 回调把数据写进该会话的 StreamBuffer 并投递事件，链路任务用 poll_event/read 取走。
 * rtk_bt_spp_handle_data / rtk_bt_spp_evt_post 在 BTC 回调线程调用：只拷贝、非阻塞投递；
 * rtk_bt_spp_poll_event / rtk_bt_spp_read / rtk_bt_spp_dropped_events 只允许链路任务（单一所有者）调用。 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#include "esp_log.h"
#include "esp_spp_api.h"

#include "rtk_bt_spp_priv.h"

void rtk_bt_spp_handle_data(esp_spp_cb_param_t *param)
{
    rtk_bt_spp_session_t *session = NULL;
    size_t len = 0U;
    size_t written = 0U;

    if (param->data_ind.status != ESP_SPP_SUCCESS) {
        return;
    }
    if (param->data_ind.data == NULL) {
        return;
    }

    session = rtk_bt_spp_session_find(param->data_ind.handle);
    if (session == NULL) {
        return;
    }

    /* data_ind.len 为 uint16_t，扩宽到 size_t 无损 */
    len = (size_t)param->data_ind.len;
    written = xStreamBufferSend(session->rx, param->data_ind.data, len, 0);
    if (written != 0U) {
        rtk_bt_spp_evt_post(RTK_BT_SPP_EVT_DATA, param->data_ind.handle, NULL, 0U);
    }
    if (written < len) {
        /* 丢弃字节 ≤ 65535，uint32_t 扩宽无损 */
        const uint32_t dropped = (uint32_t)(len - written);

        rtk_bt_spp_evt_post(RTK_BT_SPP_EVT_RX_DROP, param->data_ind.handle, NULL, dropped);
        ESP_LOGW(RTK_BT_SPP_TAG, "rx overflow handle=%" PRIu32 " dropped=%" PRIu32, param->data_ind.handle, dropped);
    }
}

/* 回调单生产者：DATA/RX_DROP 合并，生命周期由槽接纳背压保证队列容量。 */
void rtk_bt_spp_evt_post(rtk_bt_spp_evt_type_t type, uint32_t handle, const uint8_t *addr, uint32_t dropped)
{
    rtk_bt_spp_evt_t evt;

    if (s_spp.rtos.evt_queue == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_spp_mux);
    if ((type == RTK_BT_SPP_EVT_RX_DROP) || (type == RTK_BT_SPP_EVT_DATA)) {
        for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
            rtk_bt_spp_session_t *session = &s_spp.sessions[idx];
            if (session->in_use && (session->handle == handle)) {
                bool *pending = (type == RTK_BT_SPP_EVT_RX_DROP) ? &session->drop_pending : &session->data_pending;
                if (type == RTK_BT_SPP_EVT_RX_DROP) {
                    /* 饱和累计，避免长时间不 poll 时回绕成小数；保留每个 handle 的归属。 */
                    session->rx_dropped =
                        (dropped > (UINT32_MAX - session->rx_dropped)) ? UINT32_MAX : (session->rx_dropped + dropped);
                }
                if (*pending) {
                    taskEXIT_CRITICAL(&s_spp_mux);
                    return;
                }
                *pending = true;
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    evt.type = type;
    evt.handle = handle;
    evt.dropped = dropped;
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_ADDR_LEN; idx++) {
        evt.addr[idx] = (addr != NULL) ? addr[idx] : 0U;
    }

    if (xQueueSend(s_spp.rtos.evt_queue, &evt, 0) != pdPASS) {
        taskENTER_CRITICAL(&s_spp_mux);
        s_spp.evt_dropped++;
        taskEXIT_CRITICAL(&s_spp_mux);
        ESP_LOGW(RTK_BT_SPP_TAG, "event queue full, total dropped=%" PRIu32, s_spp.evt_dropped);
    }
}

bool rtk_bt_spp_poll_event(rtk_bt_spp_evt_t *evt, uint32_t timeout_ms)
{
    if ((evt == NULL) || (!s_spp.initialized) || (s_spp.rtos.evt_queue == NULL)) {
        return false;
    }

    if (xQueueReceive(s_spp.rtos.evt_queue, evt, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if (s_spp.sessions[idx].lifecycle_pending && (s_spp.sessions[idx].handle == evt->handle)) {
            if (evt->type == RTK_BT_SPP_EVT_CLOSE) {
                s_spp.sessions[idx].lifecycle_pending = false;
            } else if (evt->type == RTK_BT_SPP_EVT_DATA) {
                s_spp.sessions[idx].data_pending = false;
            } else if (evt->type == RTK_BT_SPP_EVT_RX_DROP) {
                evt->dropped = s_spp.sessions[idx].rx_dropped;
                s_spp.sessions[idx].rx_dropped = 0U;
                s_spp.sessions[idx].drop_pending = false;
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return true;
}

size_t rtk_bt_spp_read(uint32_t handle, uint8_t *buf, size_t size)
{
    rtk_bt_spp_session_t *session = NULL;
    size_t got = 0U;

    if ((buf == NULL) || (size == 0U)) {
        return 0U;
    }

    /* 登记期间槽不会被复用，也不会被 CLOSE 后的新连接 reset（QA-E-REVIEW 第 1 项）。 */
    session = rtk_bt_spp_session_acquire(handle);
    if (session == NULL) {
        return 0U;
    }

    got = xStreamBufferReceive(session->rx, buf, size, 0);
    rtk_bt_spp_session_release(session);
    return got;
}

uint32_t rtk_bt_spp_dropped_events(void)
{
    uint32_t dropped = 0U;

    taskENTER_CRITICAL(&s_spp_mux);
    dropped = s_spp.evt_dropped;
    taskEXIT_CRITICAL(&s_spp_mux);
    return dropped;
}
