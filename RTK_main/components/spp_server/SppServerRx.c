/* 接收路径：BTC 回调把数据写进该会话的 StreamBuffer 并投递事件，链路任务用 poll_event/read 取走。
 * spp_server_handle_data / spp_server_evt_post 在 BTC 回调线程调用：只拷贝、非阻塞投递；
 * spp_server_poll_event / spp_server_read / spp_server_dropped_events 只允许链路任务（单一所有者）调用。 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#include "esp_spp_api.h"

#include "SppServerPriv.h"

/* 唤醒回调指针放在 s_spp 外，保证 init 前后安装都能跨重连/重试保留。 */
static spp_server_rx_wake_fn s_rx_wake_fn;
static void *s_rx_wake_ctx;

void spp_server_set_rx_wake_hook(spp_server_rx_wake_fn fn, void *ctx)
{
    taskENTER_CRITICAL(&s_spp_mux);
    s_rx_wake_fn = fn;
    s_rx_wake_ctx = ctx;
    taskEXIT_CRITICAL(&s_spp_mux);
}

void spp_server_notify_rx_wake(void)
{
    spp_server_rx_wake_fn fn = NULL;
    void *ctx = NULL;

    taskENTER_CRITICAL(&s_spp_mux);
    fn = s_rx_wake_fn;
    ctx = s_rx_wake_ctx;
    if (fn != NULL) {
        s_spp.rx_wake_calls++;
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (fn != NULL) {
        fn(ctx);
    }
}

void spp_server_handle_data(esp_spp_cb_param_t *param)
{
    spp_server_session_t *session = NULL;
    size_t len = 0U;
    size_t written = 0U;
    size_t avail = 0U;

    if (param->data_ind.status != ESP_SPP_SUCCESS) {
        return;
    }
    if (param->data_ind.data == NULL) {
        return;
    }

    session = spp_server_session_acquire(param->data_ind.handle);
    if (session == NULL) {
        return;
    }

    /* data_ind.len 为 uint16_t，扩宽到 size_t 无损 */
    len = (size_t)param->data_ind.len;
    written = xStreamBufferSend(session->rx, param->data_ind.data, len, 0);
    avail = xStreamBufferBytesAvailable(session->rx);

    taskENTER_CRITICAL(&s_spp_mux);
    if ((uint32_t)avail > session->rx_high_water) {
        session->rx_high_water = (uint32_t)avail;
    }
    if (written != 0U) {
        session->rx_events++;
    }
    if (written < len) {
        session->drop_events++;
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    spp_server_session_release(session);

    if (written != 0U) {
        spp_server_evt_post(SPP_SERVER_EVT_DATA, param->data_ind.handle, NULL, 0U);
    }
    if (written < len) {
        /* 丢弃字节 ≤ 65535，uint32_t 扩宽无损 */
        const uint32_t dropped = (uint32_t)(len - written);

        spp_server_evt_post(SPP_SERVER_EVT_RX_DROP, param->data_ind.handle, NULL, dropped);
        /* a = 本次丢弃字节，b = 缓冲已用字节。 */
        spp_server_trace(SPP_SERVER_TRACE_RX_DROP, param->data_ind.handle, dropped,
                         (uint32_t)avail);
    }
    if ((written != 0U) || (written < len)) {
        /* 必须在退出 portMUX 且发布流数据/事件之后唤醒 kLink，防止唤醒合并后睡死。 */
        spp_server_notify_rx_wake();
    }
}

/* 回调单生产者：DATA/RX_DROP 合并，生命周期由槽接纳背压保证队列容量。 */
void spp_server_evt_post(spp_server_evt_type_t type, uint32_t handle, const uint8_t *addr,
                         uint32_t dropped)
{
    spp_server_evt_t evt;

    if (s_spp.rtos.evt_queue == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_spp_mux);
    if ((type == SPP_SERVER_EVT_RX_DROP) || (type == SPP_SERVER_EVT_DATA)) {
        for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
            spp_server_session_t *session = &s_spp.sessions[idx];
            if (session->in_use && (session->handle == handle)) {
                bool *pending = (type == SPP_SERVER_EVT_RX_DROP) ? &session->drop_pending
                                                                 : &session->data_pending;
                if (type == SPP_SERVER_EVT_RX_DROP) {
                    /* 饱和累计，避免长时间不 poll 时回绕成小数；保留每个 handle 的归属。 */
                    session->rx_dropped = (dropped > (UINT32_MAX - session->rx_dropped))
                                              ? UINT32_MAX
                                              : (session->rx_dropped + dropped);
                    session->rx_dropped_total = (dropped > (UINT32_MAX - session->rx_dropped_total))
                                                    ? UINT32_MAX
                                                    : (session->rx_dropped_total + dropped);
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
    for (uint32_t idx = 0U; idx < SPP_SERVER_ADDR_LEN; idx++) {
        evt.addr[idx] = (addr != NULL) ? addr[idx] : 0U;
    }

    if (xQueueSend(s_spp.rtos.evt_queue, &evt, 0) != pdPASS) {
        uint32_t total_dropped = 0U;

        taskENTER_CRITICAL(&s_spp_mux);
        s_spp.evt_dropped++;
        total_dropped = s_spp.evt_dropped;
        if ((type == SPP_SERVER_EVT_DATA) || (type == SPP_SERVER_EVT_RX_DROP)) {
            for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
                spp_server_session_t *session = &s_spp.sessions[idx];
                if (session->in_use && (session->handle == handle)) {
                    if (type == SPP_SERVER_EVT_DATA) {
                        session->rx_data_missed = true;
                        session->data_pending = false;
                    } else {
                        session->drop_pending = false;
                    }
                    break;
                }
            }
        }
        taskEXIT_CRITICAL(&s_spp_mux);
        /* a = 被丢弃的事件类型，b = 累计丢弃数（只在临界区内读）。 */
        spp_server_trace(SPP_SERVER_TRACE_EVT_DROP, handle, (uint32_t)type, total_dropped);
    }
    if ((type == SPP_SERVER_EVT_OPEN) || (type == SPP_SERVER_EVT_CLOSE)) {
        spp_server_notify_rx_wake();
    }
}

/* 队列满丢失 DATA/RX_DROP 时，由 level 状态在 poll_event 排空前兜底交付。 */
static bool spp_server_poll_rx_level(spp_server_evt_t *evt)
{
    bool found = false;
    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; ++idx) {
        spp_server_session_t *session = &s_spp.sessions[idx];
        if ((session->in_use || session->lifecycle_pending) && (session->rx_dropped > 0U)) {
            *evt = (spp_server_evt_t){
                .type = SPP_SERVER_EVT_RX_DROP,
                .handle = session->handle,
                .dropped = session->rx_dropped,
            };
            session->rx_dropped = 0U;
            session->drop_pending = false;
            found = true;
            break;
        }
        if (session->in_use && session->rx_data_missed) {
            session->rx_data_missed = false;
            session->data_pending = false;
            *evt = (spp_server_evt_t){
                .type = SPP_SERVER_EVT_DATA,
                .handle = session->handle,
            };
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return found;
}

/* TX_SPACE 不占会话生命周期顺序；通知丢失时 level 仍可在阻塞前取出。 */
static bool spp_server_poll_tx_space(spp_server_evt_t *evt)
{
    bool found = false;
    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; ++idx) {
        spp_server_session_t *session = &s_spp.sessions[idx];
        if (session->in_use && session->tx_space) {
            session->tx_space = false;
            *evt = (spp_server_evt_t){.type = SPP_SERVER_EVT_TX_SPACE, .handle = session->handle};
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return found;
}

bool spp_server_poll_event(spp_server_evt_t *evt, uint32_t timeout_ms)
{
    if ((evt == NULL) || (!s_spp.initialized) || (s_spp.rtos.evt_queue == NULL)) {
        return false;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        /* 先交付生命周期。空哨兵可能排在 CLOSE 前，须跳过而非提前报告超时。 */
        if (xQueueReceive(s_spp.rtos.evt_queue, evt, 0) != pdTRUE) {
            if (spp_server_poll_rx_level(evt) || spp_server_poll_tx_space(evt)) {
                return true;
            }
            const TickType_t elapsed = xTaskGetTickCount() - start;
            if ((elapsed >= budget) ||
                (xQueueReceive(s_spp.rtos.evt_queue, evt, budget - elapsed) != pdTRUE)) {
                return spp_server_poll_rx_level(evt) || spp_server_poll_tx_space(evt);
            }
        }
        if (evt->type == SPP_SERVER_EVT_TX_SPACE) {
            taskENTER_CRITICAL(&s_spp_mux);
            s_spp.tx_wake_pending = false;
            taskEXIT_CRITICAL(&s_spp_mux);
            continue;
        }
        taskENTER_CRITICAL(&s_spp_mux);
        for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
            if (s_spp.sessions[idx].lifecycle_pending &&
                (s_spp.sessions[idx].handle == evt->handle)) {
                if (evt->type == SPP_SERVER_EVT_CLOSE) {
                    s_spp.sessions[idx].lifecycle_pending = false;
                } else if (evt->type == SPP_SERVER_EVT_DATA) {
                    s_spp.sessions[idx].data_pending = false;
                    s_spp.sessions[idx].rx_data_missed = false;
                } else if (evt->type == SPP_SERVER_EVT_RX_DROP) {
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
}

bool spp_server_rx_ready(uint32_t handle)
{
    spp_server_session_t *session = NULL;
    size_t avail = 0U;
    bool missed = false;

    if (!s_spp.initialized) {
        return false;
    }
    session = spp_server_session_acquire(handle);
    if (session == NULL) {
        return false;
    }
    avail = xStreamBufferBytesAvailable(session->rx);
    taskENTER_CRITICAL(&s_spp_mux);
    missed = session->rx_data_missed || (session->rx_dropped > 0U);
    taskEXIT_CRITICAL(&s_spp_mux);
    spp_server_session_release(session);
    return (avail > 0U) || missed;
}

bool spp_server_any_rx_ready(void)
{
    if (!s_spp.initialized) {
        return false;
    }
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; ++idx) {
        spp_server_session_t *session = &s_spp.sessions[idx];
        StreamBufferHandle_t rx = NULL;
        bool ready = false;

        taskENTER_CRITICAL(&s_spp_mux);
        if (session->in_use) {
            session->users++;
            rx = session->rx;
        }
        ready = session->rx_data_missed || (session->rx_dropped > 0U);
        taskEXIT_CRITICAL(&s_spp_mux);

        if (rx != NULL) {
            ready = ready || (xStreamBufferBytesAvailable(rx) > 0U);
            spp_server_session_release(session);
        }
        if (ready) {
            return true;
        }
    }
    return false;
}

void spp_server_rx_stats(spp_server_rx_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));

    taskENTER_CRITICAL(&s_spp_mux);
    stats->evt_dropped = s_spp.evt_dropped;
    stats->wake_calls = s_spp.rx_wake_calls;
    taskEXIT_CRITICAL(&s_spp_mux);

    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; ++idx) {
        spp_server_session_t *session = &s_spp.sessions[idx];
        StreamBufferHandle_t rx = NULL;

        taskENTER_CRITICAL(&s_spp_mux);
        if (session->in_use) {
            session->users++;
            rx = session->rx;
        }
        stats->sessions[idx].high_water = session->rx_high_water;
        stats->sessions[idx].dropped_bytes = session->rx_dropped_total;
        stats->sessions[idx].rx_events = session->rx_events;
        stats->sessions[idx].drop_events = session->drop_events;
        taskEXIT_CRITICAL(&s_spp_mux);

        if (rx != NULL) {
            stats->sessions[idx].used = (uint32_t)xStreamBufferBytesAvailable(rx);
            spp_server_session_release(session);
        }
    }
}

size_t spp_server_read(uint32_t handle, uint8_t *buf, size_t size)
{
    spp_server_session_t *session = NULL;
    size_t got = 0U;

    if ((buf == NULL) || (size == 0U)) {
        return 0U;
    }

    /* 登记期间槽不会被复用，也不会被 CLOSE 后的新连接 reset（QA-E-REVIEW 第 1 项）。 */
    session = spp_server_session_acquire(handle);
    if (session == NULL) {
        return 0U;
    }

    got = xStreamBufferReceive(session->rx, buf, size, 0);
    spp_server_session_release(session);
    return got;
}

uint32_t spp_server_dropped_events(void)
{
    uint32_t dropped = 0U;

    taskENTER_CRITICAL(&s_spp_mux);
    dropped = s_spp.evt_dropped;
    taskEXIT_CRITICAL(&s_spp_mux);
    return dropped;
}
