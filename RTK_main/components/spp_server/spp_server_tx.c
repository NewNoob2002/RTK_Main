/* 发送路径：发送任务（唯一调用 esp_spp_write/esp_spp_disconnect 的地方）、按会话分块提交、
 * WRITE_EVT 结算，以及 spp_server_write / spp_server_disconnect 公共接口。
 * spp_server_worker/worker_submit/disconnect_process 只在发送任务里运行；spp_server_handle_write
 * 在 BTC 回调线程运行（临界区内只改会话状态，临界区外非阻塞唤醒）；
 * spp_server_write/disconnect 只允许链路任务调用，等待范围由调用方的 timeout_ms 收敛。 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_spp_api.h"

#include "spp_server_priv.h"

/* 整次 write 的剩余预算；预算耗尽返回 false。认领、复制、排队与等待都复用它。 */
static bool spp_server_budget_left(TickType_t start, uint32_t timeout_ms)
{
    return (xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms);
}

/* 等本会话状态变化；返回 false 表示本次等待后预算仍不够（由调用方判定超时）。 */
static bool spp_server_wait_change(uint8_t idx, TickType_t start, uint32_t timeout_ms)
{
    TickType_t elapsed = 0U;
    TickType_t budget = 0U;

    if ((timeout_ms == 0U) || (s_spp.rtos.evt_group == NULL)) {
        return false;
    }

    budget = pdMS_TO_TICKS(timeout_ms);
    elapsed = xTaskGetTickCount() - start;
    if (elapsed >= budget) {
        return false;
    }
    (void)xEventGroupWaitBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(idx), pdFALSE, pdFALSE, budget - elapsed);
    return true;
}

/* 发送任务：取一个已排队的请求提交给 IDF。这是全工程唯一调用 esp_spp_write 的位置。 */
static void spp_server_worker_submit(spp_server_session_t *session)
{
    uint32_t seq = 0U;
    uint16_t len = 0U;
    uint32_t handle = 0U;
    uint8_t idx = 0U;
    esp_err_t err = ESP_OK;
    bool wake_worker = false;
    /* 提交时刻在临界区外读取：临界区内不调用任何 FreeRTOS API。 */
    const TickType_t submit_tick = xTaskGetTickCount();

    /* 取任务：在临界区内取走长度与序号，避免与写者复制 tx_buf 交错。
     * 全驱动只允许一个未完成提交：owner 已占用时本任务保持排队，等完成回调通知后再来。 */
    taskENTER_CRITICAL(&s_spp_mux);
    if (session->in_use && (session->tx_state == SPP_SERVER_TX_QUEUED) && (s_spp_tx_inflight < 0)) {
        len = session->tx_len;
        seq = session->tx_seq;
        handle = session->handle;
        idx = session->idx;
        session->tx_state = SPP_SERVER_TX_SUBMITTED;
        session->tx_submitting = true;
        /* 记录提交时刻：写完成事件用它与当前 tick 的差值报告耗时（ms）。 */
        session->tx_submit_tick = submit_tick;
        /* idx < SPP_SERVER_MAX_SESSIONS(=2)，int 窄化安全 */
        s_spp_tx_inflight = (int)idx;
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (len == 0U) {
        return;
    }

    /* esp_spp_write 先深拷贝参数，再经 btc_transfer_context 投递（可能阻塞在 BTC 队列上）。
     * 那次阻塞发生在本任务里，调用方只按自己的剩余预算等待完成事件，因此不会无限阻塞在
     * 调用方的上下文。len ≤ SPP_SERVER_TX_CHUNK，扩宽到 int 无损。 */
    err = esp_spp_write(handle, (int)len, session->tx_buf);

    taskENTER_CRITICAL(&s_spp_mux);
    /* tx_submitting 一定要清（否则槽永远无法复用）；结果只写回仍是本次请求的会话，
     * 避免一次被阻塞很久的提交在会话关闭/复用后污染新会话。 */
    session->tx_submitting = false;
    if (err != ESP_OK) {
        /* 提交失败不会有 WRITE_EVT：释放 owner 并结案，让等待方立刻返回该错误。 */
        if (s_spp_tx_inflight == (int)idx) {
            s_spp_tx_inflight = -1;
            s_spp.tx_inflight_cancelled = false;
            wake_worker = true;
        }
        if (session->in_use && (session->tx_state == SPP_SERVER_TX_SUBMITTED) && (session->tx_seq == seq)) {
            session->tx_result = err;
            session->tx_done_len = -1;
            session->tx_done_seq = seq;
            session->tx_state = SPP_SERVER_TX_IDLE;
        }
    } else if ((s_spp_tx_inflight == (int)idx) && s_spp.tx_inflight_cancelled) {
        /* CLOSE 在本调用期间取消了该提交。API 已返回（深拷贝/投递完成），静态 tx_buf 可再用，
         * 现在释放 owner；旧消息之后若入队失败只会报匿名 NO_CONNECTION，按取消事件忽略。 */
        s_spp_tx_inflight = -1;
        s_spp.tx_inflight_cancelled = false;
        wake_worker = true;
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (err != ESP_OK) {
        /* esp_err_t → uint32_t 只是位模式搬移，追踪只做展示用。 */
        spp_server_trace(SPP_SERVER_TRACE_WRITE_ERR, handle, (uint32_t)err, (uint32_t)len);
    }
    if (s_spp.rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(idx));
    }
    /* 重新扫描一次，保证低于当前下标的排队请求也能被提交。 */
    if (wake_worker && (s_spp.rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_spp.rtos.worker);
    }
}

/* 普通断开有独立的每槽状态。失败每 100 ms 重试，成功只等真实 CLOSE；不伪造关闭。
 * 提交期间禁止槽复用，即使 CLOSE 与 SDK 阻塞调用交错也不会把返回结果写入新会话。 */
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
            /* 断开标志保留，下一轮重试。 */
            spp_server_trace(SPP_SERVER_TRACE_CTRL_ERR, handle, (uint32_t)SPP_SERVER_TRACE_CTRL_DISCONNECT, (uint32_t)err);
        }
    }
}

/* 发送任务：串行提交各会话的排队请求，并执行控制表里的 GAP 应答/拒绝断开。
 * 绑核理由见 SPP_SERVER_WORKER_CORE。 */
void spp_server_worker(void *arg)
{
    (void)arg;

    for (;;) {
        for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
            bool queued = false;

            taskENTER_CRITICAL(&s_spp_mux);
            queued = s_spp.sessions[idx].in_use && (s_spp.sessions[idx].tx_state == SPP_SERVER_TX_QUEUED);
            taskEXIT_CRITICAL(&s_spp_mux);

            if (queued) {
                spp_server_worker_submit(&s_spp.sessions[idx]);
            }
        }
        spp_server_ctrl_process();
        spp_server_disconnect_process();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SPP_SERVER_CTRL_RETRY_MS));
    }
}

/* WRITE_EVT 结论：status 非成功、回报长度与提交长度不一致（短写）都按失败返回。
 * 成功路径的 len 是 RFCOMM 实际接受的字节数（bta_jv_act.c:2413-2420 → btc_spp.c:1136），
 * 因此 len != tx_len 不能当成功。只允许在临界区内调用：不访问 RTOS/日志。 */
static esp_err_t spp_server_settle_write(spp_server_session_t *session, esp_spp_status_t status, int done_len)
{
    const bool failed =
        (status != ESP_SPP_SUCCESS) || (done_len < 0) || ((uint32_t)done_len != (uint32_t)session->tx_len);
    const esp_err_t result = failed ? ESP_FAIL : ESP_OK;

    session->tx_done_len = done_len;
    session->tx_result = result;
    session->tx_done_seq = session->tx_seq;
    session->tx_state = SPP_SERVER_TX_IDLE;
    return result;
}

/* 写完成：把真实 status/len/cong 写回会话并唤醒等待方（QA-E-REVIEW 第 3 项）。 */
void spp_server_handle_write(esp_spp_cb_param_t *param)
{
    bool matched = false;
    bool wake_worker = false;
    bool ignore_stale = false;
    EventBits_t wake_bits = 0U;
    esp_err_t result = ESP_OK;
    /* 追踪数据先在临界区内取出，调用 trace 钩子时必须在临界区之外。 */
    uint32_t traced_handle = param->write.handle;
    uint32_t traced_bytes = (param->write.len > 0) ? (uint32_t)param->write.len : 0U;
    TickType_t traced_submit = 0;
    bool traced = false;

    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t slot = 0U; slot < SPP_SERVER_MAX_SESSIONS; slot++) {
        if (!s_spp.sessions[slot].in_use || (s_spp.sessions[slot].handle != param->write.handle)) {
            continue;
        }
        if (s_spp.sessions[slot].tx_state != SPP_SERVER_TX_SUBMITTED) {
            continue;
        }
        matched = true;
        traced = true;
        traced_bytes = (uint32_t)s_spp.sessions[slot].tx_len;
        traced_submit = s_spp.sessions[slot].tx_submit_tick;
        result = spp_server_settle_write(&s_spp.sessions[slot], param->write.status, param->write.len);
        s_spp.sessions[slot].congested = param->write.cong;
        wake_bits |= SPP_SERVER_BIT_WAKE(s_spp.sessions[slot].idx);
        if (s_spp_tx_inflight == (int)slot) {
            s_spp_tx_inflight = -1;
            s_spp.tx_inflight_cancelled = false;
            wake_worker = true;
        }
        break;
    }

    if ((!matched) && (param->write.handle == 0U) && (param->write.status != ESP_SPP_SUCCESS)) {
        /* IDF 提交阶段失败时以 handle=0/len=-1 报告（btc_spp.c:925-943）。
         * NO_CONNECTION 表示槽已消失/未连接：那是 CLOSE 取消（或即将到来的 CLOSE）负责的，
         * 迟到旧请求的匿名 NO_CONNECTION 绝不能结算当前 owner。NO_RESOURCE 等是当前
         * 唯一在途提交的真实提交失败，归属 owner。 */
        if (param->write.status == ESP_SPP_NO_CONNECTION) {
            ignore_stale = true;
        } else if (s_spp_tx_inflight >= 0) {
            spp_server_session_t *owner = &s_spp.sessions[s_spp_tx_inflight];

            result = ESP_FAIL;
            matched = true;
            traced = true;
            traced_bytes = (uint32_t)owner->tx_len;
            traced_submit = owner->tx_submit_tick;
            if (owner->in_use && (owner->tx_state == SPP_SERVER_TX_SUBMITTED)) {
                (void)spp_server_settle_write(owner, param->write.status, param->write.len);
                wake_bits |= SPP_SERVER_BIT_WAKE(owner->idx);
            }
            s_spp_tx_inflight = -1;
            s_spp.tx_inflight_cancelled = false;
            wake_worker = true;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (ignore_stale) {
        /* 旧请求在 CLOSE 后才被 BTC 处理：没有新会话的完成可结算，忽略（正常竞态，不记）。 */
        return;
    }
    if (!matched) {
        /* 带有效句柄但不属于当前在途请求的迟到事件（成功或失败）：一律忽略，
         * 绝不能结算到别的会话。 */
        spp_server_trace(SPP_SERVER_TRACE_WRITE_STRAY, param->write.handle,
                         (param->write.len > 0) ? (uint32_t)param->write.len : 0U, (uint32_t)param->write.status);
        return;
    }
    if (traced) {
        if (result == ESP_OK) {
            /* 耗时来自提交时刻与本回调时刻的 tick 差；转换只用于展示。 */
            const uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - traced_submit);
            spp_server_trace(SPP_SERVER_TRACE_WRITE_DONE, traced_handle, traced_bytes, elapsed_ms);
        } else {
            spp_server_trace(SPP_SERVER_TRACE_WRITE_ERR, traced_handle, (uint32_t)ESP_FAIL, traced_bytes);
        }
    }
    if (s_spp.rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, wake_bits);
    }
    /* 完成回调可能来自 BTC：只做非阻塞任务通知，让发送任务提交其他会话的排队请求。 */
    if (wake_worker && (s_spp.rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_spp.rtos.worker);
    }
}

/* 提交一个分块并等它的真实完成。start/timeout_ms 由调用方在整次 write 内共享。 */
static esp_err_t spp_server_write_chunk(spp_server_session_t *session, const uint8_t *data, size_t len,
                                        TickType_t start, uint32_t timeout_ms)
{
    uint32_t seq = 0U;

    if ((s_spp.rtos.worker == NULL) || (s_spp.rtos.evt_group == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 0) 认领/复制/排队与等待共用整次 write 的预算：预算已在块间耗尽时不得再提交下一块。 */
    if (!spp_server_budget_left(start, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    /* 1) 原子地认领发送槽：同一会话一次只有一个在途请求，连续 write 不会堆进 IDF 的 10 深 TX
     *    队列；拥塞时按剩余预算等 ESP_SPP_CONG_EVT 解除。认领成功后本调用独占 tx_buf，
     *    并发写者只能等它交还给发送任务。 */
    for (;;) {
        bool alive = false;
        bool claimed = false;

        (void)xEventGroupClearBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(session->idx));
        taskENTER_CRITICAL(&s_spp_mux);
        alive = session->in_use;
        if (alive && (session->tx_state == SPP_SERVER_TX_IDLE) && (!session->tx_submitting) && (!session->congested)) {
            session->tx_state = SPP_SERVER_TX_RESERVED;
            claimed = true;
        }
        taskEXIT_CRITICAL(&s_spp_mux);

        if (claimed) {
            break;
        }
        if (!alive) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!spp_server_wait_change(session->idx, start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
    }

    /* 复制前再核对预算；已过预算则把认领交还 IDLE，不能留下永久 RESERVED 的槽。 */
    if (!spp_server_budget_left(start, timeout_ms)) {
        taskENTER_CRITICAL(&s_spp_mux);
        if (session->in_use && (session->tx_state == SPP_SERVER_TX_RESERVED)) {
            session->tx_state = SPP_SERVER_TX_IDLE;
        }
        taskEXIT_CRITICAL(&s_spp_mux);
        return ESP_ERR_TIMEOUT;
    }

    /* 2) 复制到静态提交缓冲：已认领 RESERVED，其它写者进不来，发送任务也不会取走它。 */
    for (size_t idx = 0U; idx < len; idx++) {
        session->tx_buf[idx] = data[idx];
    }

    taskENTER_CRITICAL(&s_spp_mux);
    /* 复制期间会话可能被关闭：此时槽已交还给 IDLE，不得再排入请求。 */
    if (session->in_use) {
        /* len ≤ SPP_SERVER_TX_CHUNK(512)，uint16_t 窄化安全 */
        session->tx_len = (uint16_t)len;
        session->tx_seq++;
        seq = session->tx_seq;
        session->tx_state = SPP_SERVER_TX_QUEUED;
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (seq == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xTaskNotifyGive(s_spp.rtos.worker);

    /* 3) 等真实 WRITE_EVT：status/len/cong 由回调写回，超时按剩余预算收敛。 */
    for (;;) {
        bool alive = false;
        bool done = false;
        esp_err_t result = ESP_OK;

        (void)xEventGroupClearBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(session->idx));
        taskENTER_CRITICAL(&s_spp_mux);
        alive = session->in_use;
        done = (session->tx_done_seq == seq);
        result = session->tx_result;
        taskEXIT_CRITICAL(&s_spp_mux);

        if (done) {
            return result;
        }
        if (!alive) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!spp_server_wait_change(session->idx, start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t spp_server_write(uint32_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    spp_server_session_t *session = NULL;
    TickType_t start = 0U;
    size_t offset = 0U;
    esp_err_t err = ESP_OK;

    if ((data == NULL) || (len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_spp.rtos.worker == NULL) || (!s_spp.initialized)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 合法参数之后先确认会话：未知句柄必须返回 NOT_FOUND，不能被 0 预算的 TIMEOUT 抢先。
     * 发送是异步的，0 预算无法确认任何完成，对已知会话按超时返回且不提交。 */
    start = xTaskGetTickCount();
    session = spp_server_session_acquire(handle);
    if (session == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (timeout_ms == 0U) {
        spp_server_session_release(session);
        return ESP_ERR_TIMEOUT;
    }

    /* 分块：每块 ≤ SPP_SERVER_TX_CHUNK，整次调用共享同一份 timeout 预算。 */
    while (offset < len) {
        size_t chunk = len - offset;

        if (!spp_server_budget_left(start, timeout_ms)) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (chunk > (size_t)SPP_SERVER_TX_CHUNK) {
            chunk = (size_t)SPP_SERVER_TX_CHUNK;
        }
        err = spp_server_write_chunk(session, data + offset, chunk, start, timeout_ms);
        if (err != ESP_OK) {
            break;
        }
        offset += chunk;
    }

    spp_server_session_release(session);
    return err;
}

esp_err_t spp_server_disconnect(uint32_t handle)
{
    bool found = false;

    /* 只登记；独立于 GAP/拒绝控制表，重复请求幂等且不会挤掉拒绝命令。 */
    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            s_spp.sessions[idx].disconnect_pending = true;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)xTaskNotifyGive(s_spp.rtos.worker);
    return ESP_OK;
}
