/* 非阻塞 TX 字节环、四在途 worker 与真实 WRITE_EVT 结算。
 * 链路任务 reserve/commit/cancel；BTC 回调只改小状态并唤醒；worker 唯一调用 SDK write/disconnect。
 * 所有大复制在 mux 外，users 固定源区；无分配，非 ISR。 */
#include <stdint.h>
#include <string.h>

#include "SppServerPriv.h"

spp_server_session_t *spp_server_find_locked(uint32_t handle)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; ++idx) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            return &s_spp.sessions[idx];
        }
    }
    return NULL;
}

/**
 * @brief  从流环中取出一段可发送前缀，遇到句末位即截断并清掉该位
 * @param  offset: 流环内起始下标
 * @param  available: 当前可读字节数
 * @retval 本段长度（1..SPP_SERVER_MERGE_CAPACITY）；句末位出现在首字节时也返回 1
 * @note  需持 s_spp_mux；跨句时置 stream_active=false，让调度优先回到 reply 通道
 */
static size_t spp_server_stream_chunk_locked(size_t offset, size_t available)
{
    size_t len = (available < SPP_SERVER_MERGE_CAPACITY) ? available : SPP_SERVER_MERGE_CAPACITY;
    s_spp.stream_active = true;
    for (size_t byte = 0U; byte < len; ++byte) {
        const size_t pos = (offset + byte) % SPP_SERVER_STREAM_CAPACITY;
        const uint8_t mask = (uint8_t)(1U << (pos % 8U)); /* 位数<8 */
        if ((s_spp.stream_ends[pos / 8U] & mask) != 0U) {
            s_spp.stream_ends[pos / 8U] &= (uint8_t)~mask;
            s_spp.stream_active = false;
            len = byte + 1U;
            break;
        }
    }
    return len;
}

/**
 * @brief  取第一个空闲在途槽
 * @retval 空闲的在途槽指针；四个槽都在用时 NULL
 * @note  需持 s_spp_mux；只查不占，调用方负责填入 active 等字段
 */
static spp_server_inflight_t *spp_server_free_item_locked(void)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
        if (!s_spp.inflight[idx].active) {
            return &s_spp.inflight[idx];
        }
    }
    return NULL;
}

/* worker 串行调用：本轮要写的一段；锁内填充，锁外只读。 */
typedef struct {
    spp_server_inflight_t *item;     /* 已占用的在途槽 */
    spp_server_session_t *session;   /* 该槽所属会话（提交后必须 release） */
    const uint8_t *storage;          /* 整段源区；PUSH 时已并入 merge_buf，为 NULL */
    size_t capacity;                 /* 源环容量，用于跨尾回绕 */
    size_t offset;                   /* 源区内起点 */
    spp_server_inflight_t submitted; /* 提交前快照 */
} spp_server_submit_t;

/**
 * @brief  判断本会话本轮是否改发流通道（而不是 REPLY/PUSH）
 * @param  idx: 会话槽位下标
 * @param  candidate: 候选会话（只读）
 * @retval true 可以；false 仍在应答或未绑定新流
 * @note  需持 s_spp_mux；条件是 owner 为本会话、未在解绑、有未发出的流数据；流未开始时要等
 *        REPLY 写完且无 PUSH 待发，保证应答与流不交错。
 */
static bool spp_server_stream_ready_locked(uint8_t idx, const spp_server_session_t *candidate)
{
    return (s_spp.stream_owner == (idx + 1U)) && !s_spp.stream_unbind &&
           (s_spp.stream.used > s_spp.stream.issued) &&
           (s_spp.stream_active ||
            ((candidate->reply.used == candidate->reply.issued) && (candidate->push_len == 0U)));
}

/**
 * @brief  选出发送任务本轮要提交的一块数据并占住在途槽
 * @param[out] out: 选中结果（会话、数据、容量、偏移与在途登记）
 * @param  now: 当前时钟节拍（调用方在锁外取）
 * @retval true 已选中；false 本轮无数据可发（无空闲在途槽或所有会话都被跳过）
 * @note  需持 s_spp_mux；按 tx_cursor 轮转挑会话/通道；PUSH 通道在此处已拷入 merge_buf，
 *        环形通道由调用方锁外合并；临界区内不调用 FreeRTOS API。
 */
static bool spp_server_pick_locked(spp_server_submit_t *out, TickType_t now)
{
    out->item = spp_server_free_item_locked();
    if (out->item == NULL) {
        return false;
    }
    for (uint32_t step = 0U; step < SPP_SERVER_MAX_SESSIONS; ++step) {
        const uint8_t idx = (s_spp.tx_cursor + step) % SPP_SERVER_MAX_SESSIONS; /* 余数<2 */
        spp_server_session_t *candidate = &s_spp.sessions[idx];
        if (!candidate->in_use || candidate->congested || candidate->disconnect_pending) {
            continue;
        }
        spp_server_ring_t *ring = &candidate->reply;
        uint8_t channel = SPP_SERVER_CHANNEL_REPLY;
        out->capacity = SPP_SERVER_REPLY_CAPACITY;
        out->storage = s_spp.storage.reply_buf[idx];
        if (spp_server_stream_ready_locked(idx, candidate)) {
            ring = &s_spp.stream;
            out->capacity = SPP_SERVER_STREAM_CAPACITY;
            out->storage = s_spp.storage.stream_buf;
            channel = SPP_SERVER_CHANNEL_STREAM;
        }
        size_t len = ring->used - ring->issued;
        out->offset = ((size_t)ring->head + ring->issued) % out->capacity;
        if (len == 0U) {
            if (candidate->push_len == 0U) {
                continue;
            }
            len = candidate->push_len;
            channel = SPP_SERVER_CHANNEL_PUSH;
            memcpy(s_spp.storage.merge_buf, candidate->push, len); /* <=40B */
            candidate->push_len = 0U;
            out->storage = NULL;
        } else if (channel == SPP_SERVER_CHANNEL_STREAM) {
            len = spp_server_stream_chunk_locked(out->offset, len);
        } else if (len > SPP_SERVER_MERGE_CAPACITY) {
            len = SPP_SERVER_MERGE_CAPACITY;
        }
        if (channel != SPP_SERVER_CHANNEL_PUSH) {
            ring->issued += (uint16_t)len; /* len<=1024 */
        }
        *out->item = (spp_server_inflight_t){.active = true,
                                             .submitting = true,
                                             .slot = idx,
                                             .channel = channel,
                                             .len = (uint16_t)len,
                                             .generation = candidate->generation,
                                             .handle = candidate->handle,
                                             .order = ++s_spp.tx_order,
                                             .tick = now};
        candidate->users++;
        out->submitted = *out->item;
        s_spp.tx_stats.inflight++;
        s_spp.tx_cursor = (idx + 1U) % SPP_SERVER_MAX_SESSIONS; /* 余数<2 */
        out->session = candidate;
        return true;
    }
    return false;
}

/**
 * @brief  把 SDK write 的同步结果记入在途槽并推进会话状态
 * @param  pick: 选中的提交结果（item 的 submitting 在此清掉）
 * @param  result: esp_spp_write 的返回值
 * @note  需持 s_spp_mux；同步失败可能已部分入队，因此不续发尾部而是置该会话 disconnect_pending
 *        （仅当代数未变、会话仍在册时）；最后调用 retire_locked 结算已完成的块。
 */
static void spp_server_commit_locked(spp_server_submit_t *pick, esp_err_t result)
{
    pick->item->submitting = false;
    if (result != ESP_OK) {
        if (!pick->item->done) {
            pick->item->done = true;
            pick->item->result = result;
        }
        /* 同步提交失败可能已部分入队，不得继续续发尾部，立即关闭受影响会话。 */
        if (pick->session->in_use && (pick->session->generation == pick->item->generation)) {
            pick->session->disconnect_pending = true;
        }
    }
    spp_server_retire_locked(pick->session);
}

bool spp_server_worker_submit(void)
{
    spp_server_submit_t pick = {0};
    const TickType_t now = xTaskGetTickCount();
    taskENTER_CRITICAL(&s_spp_mux);
    const bool found = spp_server_pick_locked(&pick, now);
    taskEXIT_CRITICAL(&s_spp_mux);
    if (!found) {
        return false;
    }
    /* 锁外合并整段（跨环尾回绕）；PUSH 已在 pick 内并入 merge_buf。 */
    if (pick.storage != NULL) {
        const size_t first = (pick.submitted.len <= (pick.capacity - pick.offset))
                                 ? pick.submitted.len
                                 : (pick.capacity - pick.offset);
        memcpy(s_spp.storage.merge_buf, pick.storage + pick.offset, first);
        memcpy(s_spp.storage.merge_buf + first, pick.storage, pick.submitted.len - first);
    }
    /* 唯一 SDK write 调用；len<=1024，转 int 安全。SDK 深拷贝并可能阻塞 BTC 投递。 */
    const esp_err_t result =
        esp_spp_write(pick.submitted.handle, (int)pick.submitted.len, s_spp.storage.merge_buf);
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_commit_locked(&pick, result);
    taskEXIT_CRITICAL(&s_spp_mux);
    spp_server_session_release(pick.session);
    if (result != ESP_OK) {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_ERR, pick.submitted.handle, (uint32_t)result,
                         (uint32_t)pick.submitted.len);
    }
    spp_server_tx_wake();
    return true;
}

/**
 * @brief  在剩余预算内等待该会话的发送唤醒位
 * @param  idx: 会话槽位下标
 * @param  start: 调用方记录的起始节拍
 * @param  timeout_ms: 总预算（ms）
 * @retval false 预算已用完，没有等待
 * @retval true 已等待（等待本身可能超时返回，调用方需要再判一次预算）
 * @note  链路任务/写调用者上下文；xEventGroupWaitBits 会阻塞到唤醒或预算耗尽，非 ISR
 */
static bool spp_server_wait_change(uint8_t idx, TickType_t start, uint32_t timeout_ms)
{
    const TickType_t elapsed = xTaskGetTickCount() - start;
    const TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    if (elapsed >= budget) {
        return false;
    }
    (void)xEventGroupWaitBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(idx), pdFALSE, pdFALSE,
                              budget - elapsed);
    return true;
}

/**
 * @brief  同步写一块：等到环内有空间并预留，提交后等到这块真正写完
 * @param  session: 已登记的会话（调用方持有 users）
 * @param  data: 待写数据
 * @param  len: 本块长度，≤SPP_SERVER_TX_CHUNK
 * @param  start: 整个 write 调用的起始节拍（预算跨块共享）
 * @param  timeout_ms: 整个 write 调用的总预算（ms）
 * @retval ESP_OK 本块已被 SDK 报告写完
 * @retval ESP_ERR_TIMEOUT 预算用尽（预留在环内等待超时，或已提交但未等到完成）
 * @retval ESP_ERR_INVALID_STATE 等待期间会话被断开或释放
 * @retval 其他 提交失败的错误码，或等待期间 WRITE_EVT 上报的错误码
 * @note  链路任务/写调用者上下文；阻塞等待真实完成，超时不撤回已接纳/已提交的字节
 */
static esp_err_t spp_server_write_chunk(spp_server_session_t *session, const uint8_t *data,
                                        size_t len, TickType_t start, uint32_t timeout_ms)
{
    spp_server_tx_token_t token = {0};
    uint32_t target = 0U;
    uint32_t errors = 0U;
    for (;;) {
        (void)xEventGroupClearBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(session->idx));
        bool alive = false;
        bool ready = false;
        taskENTER_CRITICAL(&s_spp_mux);
        alive = session->in_use && !session->disconnect_pending;
        ready = alive && !session->congested;
        errors = session->tx_errors;
        target = session->tx_total + (uint32_t)len; /* len<=512 */
        taskEXIT_CRITICAL(&s_spp_mux);
        if (!alive) {
            return ESP_ERR_INVALID_STATE;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        if (ready && (spp_server_reserve(session->handle, len, SPP_SERVER_CHANNEL_REPLY, &token) ==
                      ESP_OK)) {
            break;
        }
        if (!spp_server_wait_change(session->idx, start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
    }
    const esp_err_t result = spp_server_tx_commit(&token, data, len);
    if (result != ESP_OK) {
        return result;
    }
    for (;;) {
        (void)xEventGroupClearBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(session->idx));
        taskENTER_CRITICAL(&s_spp_mux);
        const bool alive = session->in_use;
        const bool done = ((int32_t)(session->tx_done - target) >= 0);
        const esp_err_t status = (session->tx_errors != errors) ? session->tx_result : ESP_OK;
        taskEXIT_CRITICAL(&s_spp_mux);
        if (!alive) {
            return ESP_ERR_INVALID_STATE;
        }
        if (done) {
            return status;
        }
        if (!spp_server_wait_change(session->idx, start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t spp_server_write(uint32_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if ((data == NULL) || (len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_spp.initialized || (s_spp.rtos.worker == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    spp_server_session_t *session = spp_server_session_acquire(handle);
    if (session == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const TickType_t start = xTaskGetTickCount();
    esp_err_t result = ESP_OK;
    for (size_t offset = 0U; offset < len;) {
        const size_t chunk =
            ((len - offset) < SPP_SERVER_TX_CHUNK) ? (len - offset) : SPP_SERVER_TX_CHUNK;
        result = spp_server_write_chunk(session, data + offset, chunk, start, timeout_ms);
        if (result != ESP_OK) {
            break;
        }
        offset += chunk;
    }
    spp_server_session_release(session);
    return result;
}

esp_err_t spp_server_disconnect(uint32_t handle)
{
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_session_t *session = spp_server_find_locked(handle);
    if (session != NULL) {
        session->disconnect_pending = true;
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    if (session == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    spp_server_tx_wake();
    return ESP_OK;
}
