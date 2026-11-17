/* 发送工作者任务、真实 WRITE_EVT 结算与普通断开重试；唯一调用 SDK write/disconnect 的地方。
 * 任务上下文轮询 + 回调上下文只改小状态；无分配，回调内不阻塞、不解析、不打印数据内容。 */
#include <stdint.h>
#include <string.h>

#include "SppServerPriv.h"

spp_server_inflight_t *spp_server_oldest_locked(uint8_t slot, bool pending_only)
{
    spp_server_inflight_t *oldest = NULL;
    for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
        spp_server_inflight_t *item = &s_spp.inflight[idx];
        if (item->active && (item->slot == slot) && (!pending_only || !item->done) &&
            ((oldest == NULL) || ((int32_t)(item->order - oldest->order) < 0))) {
            oldest = item;
        }
    }
    return oldest;
}

void spp_server_retire_locked(spp_server_session_t *session)
{
    for (uint32_t count = 0U; count < SPP_SERVER_INFLIGHT_MAX; ++count) {
        spp_server_inflight_t *item = spp_server_oldest_locked(session->idx, false);
        if ((item == NULL) || !item->done || item->submitting) {
            break;
        }
        if (session->in_use && (session->generation == item->generation)) {
            spp_server_ring_t *ring = NULL;
            size_t capacity = SPP_SERVER_REPLY_CAPACITY;
            if (item->channel == SPP_SERVER_CHANNEL_REPLY) {
                ring = &session->reply;
                session->tx_done += item->len;
            } else if (item->channel == SPP_SERVER_CHANNEL_STREAM) {
                ring = &s_spp.stream;
                capacity = SPP_SERVER_STREAM_CAPACITY;
            }
            if (ring != NULL) {
                ring->head =
                    (uint16_t)(((size_t)ring->head + item->len) % capacity); /* 容量<=4096 */
                ring->used -= item->len;
                ring->issued -= item->len;
            }
            if (item->result == ESP_OK) {
                s_spp.tx_stats.completed += item->len;
            } else {
                s_spp.tx_stats.errors++;
                session->tx_errors++;
                session->tx_result = item->result;
            }
            session->tx_space = !session->disconnect_pending;
        }
        item->active = false;
        s_spp.tx_stats.inflight--;
    }
    spp_server_tx_reclaim_locked(session);
}

/**
 * @brief  处理待断开会话：每轮至多一次 esp_spp_disconnect，失败按重试间隔再试
 * @note   只在 worker 任务内调用；锁内只改投递标志，锁外调 SDK。
 */
static void spp_server_disconnect_process(void)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        uint32_t handle = 0U;
        bool submit = false;
        const TickType_t now = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_spp_mux);
        spp_server_session_t *session = &s_spp.sessions[idx];
        submit = session->in_use && session->disconnect_pending && !session->disconnect_submitted &&
                 (!session->disconnect_attempted ||
                  ((now - session->disconnect_attempt) >= pdMS_TO_TICKS(SPP_SERVER_CTRL_RETRY_MS)));
        if (submit) {
            handle = session->handle;
            session->disconnect_submitting = true;
            session->disconnect_attempted = true;
            session->disconnect_attempt = now;
        }
        taskEXIT_CRITICAL(&s_spp_mux);
        if (!submit) {
            continue;
        }
        const esp_err_t err = esp_spp_disconnect(handle);
        taskENTER_CRITICAL(&s_spp_mux);
        session->disconnect_submitting = false;
        session->disconnect_submitted = (err == ESP_OK);
        taskEXIT_CRITICAL(&s_spp_mux);
        if (err != ESP_OK) {
            spp_server_trace(SPP_SERVER_TRACE_CTRL_ERR, handle,
                             (uint32_t)SPP_SERVER_TRACE_CTRL_DISCONNECT, (uint32_t)err);
        }
    }
}

/**
 * @brief  发送任务主循环：提交发送、处理控制请求、重试断开
 * @param  arg: 未使用
 * @note   每轮至多提交 SPP_SERVER_INFLIGHT_MAX 次，随后按 SPP_SERVER_CTRL_RETRY_MS 等待通知。
 */
void spp_server_worker(void *arg)
{
    (void)arg;
    for (;;) {
        bool stopped = false;
        taskENTER_CRITICAL(&s_spp_mux);
        stopped = s_spp.boot_stop_requested;
        if (stopped) {
            s_spp.boot_worker_stopped = true;
        }
        taskEXIT_CRITICAL(&s_spp_mux);
        if (stopped) {
            /* 启动失败后永久停泊；TCB/栈与回调存储保留至重启，不再提交 SDK 调用。 */
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        /* 每轮至多四次，控制请求不能被持续生产的数据饿死。满/CONG 时由通知唤醒，无忙轮询。 */
        for (uint32_t count = 0U; count < SPP_SERVER_INFLIGHT_MAX; ++count) {
            if (!spp_server_worker_submit()) {
                break;
            }
        }
        spp_server_ctrl_process();
        spp_server_disconnect_process();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SPP_SERVER_CTRL_RETRY_MS));
    }
}

/* 需持 s_spp_mux：匿名写失败时把每个在途块的会话（同 generation）请求断开，返回候选槽掩码。 */
/**
 * @brief  匿名写失败：把同 generation 的会话全部置断开请求
 * @retval 已置位会话的候选掩码
 * @note   需持 s_spp_mux。
 */
static uint32_t spp_server_mark_anon_fail_locked(void)
{
    uint32_t candidates = 0U;

    s_spp.tx_stats.anon_write_fail++;
    for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
        const spp_server_inflight_t *item = &s_spp.inflight[idx];
        spp_server_session_t *session = &s_spp.sessions[item->slot];
        if (item->active && session->in_use && (session->generation == item->generation)) {
            session->disconnect_pending = true;
            candidates |= 1U << item->slot;
        }
    }
    return candidates;
}

/* SDK 回调无 request id：仅匹配相同 handle 的最早待完成块；generation 保护本地槽复用，
 * 不能辨识 SDK 假设外的“旧 handle 回调恰在新 handle 同值在途”的重复事件。 */
/**
 * @brief  写完成回调：结算在途块并唤醒发送任务
 * @param  param: spp_server_hw_init() 注册时启用的 IND 回调参数
 * @note   蓝牙回调上下文，不阻塞、不解析、不打印数据内容。
 */
void spp_server_handle_write(esp_spp_cb_param_t *param)
{
    uint32_t candidates = 0U;
    uint32_t bytes = 0U;
    TickType_t tick = 0U;
    bool matched = false;
    bool success = false;
    const bool anonymous = (param->write.handle == 0U) && (param->write.len == -1) &&
                           (param->write.status != ESP_SPP_SUCCESS);
    taskENTER_CRITICAL(&s_spp_mux);
    if (anonymous) {
        candidates = spp_server_mark_anon_fail_locked();
    } else {
        spp_server_session_t *session = spp_server_find_locked(param->write.handle);
        spp_server_inflight_t *item =
            (session != NULL) ? spp_server_oldest_locked(session->idx, true) : NULL;
        if ((item != NULL) && (item->generation == session->generation)) {
            matched = true;
            bytes = item->len;
            tick = item->tick;
            success =
                (param->write.status == ESP_SPP_SUCCESS) && (param->write.len == (int)item->len);
            item->done = true;
            item->result = success ? ESP_OK : ESP_FAIL;
            if (!success) {
                /* 短写或失败状态不得盲目重发尾部，关闭受影响会话并报告。 */
                session->disconnect_pending = true;
            }
            session->congested = param->write.cong;
            spp_server_retire_locked(session);
        } else {
            s_spp.tx_stats.stray_write++;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    if (anonymous) {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_ANON, 0U, (uint32_t)param->write.status,
                         candidates);
    } else if (!matched) {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_STRAY, param->write.handle,
                         (param->write.len > 0) ? (uint32_t)param->write.len : 0U,
                         (uint32_t)param->write.status);
    } else if (success) {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_DONE, param->write.handle, bytes,
                         (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - tick));
    } else {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_ERR, param->write.handle, (uint32_t)ESP_FAIL,
                         bytes);
    }
    spp_server_tx_wake();
}
