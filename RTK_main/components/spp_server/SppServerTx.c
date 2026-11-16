/* 非阻塞 TX 字节环、四在途 worker 与真实 WRITE_EVT 结算。
 * 链路任务 reserve/commit/cancel；BTC 回调只改小状态并唤醒；worker 唯一调用 SDK write/disconnect。
 * 所有大复制在 mux 外，users 固定源区；无分配，非 ISR。 */
#include <stdint.h>
#include <string.h>

#include "SppServerPriv.h"

#define SPP_SERVER_CHANNEL_REPLY  (0U)
#define SPP_SERVER_CHANNEL_STREAM (1U)
#define SPP_SERVER_CHANNEL_PUSH   (2U)

static spp_server_session_t *spp_server_find_locked(uint32_t handle)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; ++idx) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            return &s_spp.sessions[idx];
        }
    }
    return NULL;
}

/* level 与通知分离：通知可合并，poll_event 在等待前重查每槽 level。 */
void spp_server_tx_wake(void)
{
    bool post = false;
    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; ++idx) {
        if (s_spp.sessions[idx].tx_space && !s_spp.tx_wake_pending) {
            s_spp.tx_wake_pending = true;
            post = true;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    if (post && (s_spp.rtos.evt_queue != NULL)) {
        const spp_server_evt_t evt = {.type = SPP_SERVER_EVT_TX_SPACE};
        if (xQueueSend(s_spp.rtos.evt_queue, &evt, 0) != pdPASS) {
            taskENTER_CRITICAL(&s_spp_mux);
            s_spp.tx_wake_pending = false;
            taskEXIT_CRITICAL(&s_spp_mux);
        }
    }
    if (s_spp.rtos.worker != NULL) {
        (void)xTaskNotifyGive(s_spp.rtos.worker);
    }
    if (s_spp.rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(0) | SPP_SERVER_BIT_WAKE(1));
    }
    if (post) {
        spp_server_notify_rx_wake();
    }
}

void spp_server_tx_reclaim_locked(spp_server_session_t *session)
{
    if ((s_spp.stream_owner == (session->idx + 1U)) && s_spp.stream_unbind && (session->users == 0U)) {
        /* 旧流的回调也不能归还给未来 owner；解绑必须等旧流全部结算或 CLOSE。 */
        bool busy = false;
        for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
            busy = busy || (s_spp.inflight[idx].active && (s_spp.inflight[idx].slot == session->idx) &&
                            (s_spp.inflight[idx].channel == SPP_SERVER_CHANNEL_STREAM));
        }
        if (!busy) {
            s_spp.tx_stats.cancelled += s_spp.stream.used;
            memset(&s_spp.stream, 0, sizeof(s_spp.stream));
            s_spp.stream_owner = 0U;
            s_spp.stream_unbind = false;
            s_spp.stream_active = false;
        }
    }
}

void spp_server_tx_close_locked(spp_server_session_t *session)
{
    s_spp.tx_stats.cancelled += session->reply.used + session->push_len;
    session->reply.used = 0U;
    session->reply.issued = 0U;
    session->reply.reserved = 0U;
    session->push_len = 0U;
    session->tx_result = ESP_ERR_INVALID_STATE;
    session->tx_space = false;
    for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
        spp_server_inflight_t *item = &s_spp.inflight[idx];
        if (item->active && (item->slot == session->idx)) {
            if (item->submitting) {
                item->done = true; /* API 返回前仍固定描述符和 merge/users */
            } else {
                item->active = false;
                s_spp.tx_stats.inflight--;
            }
        }
    }
    if (s_spp.stream_owner == (session->idx + 1U)) {
        s_spp.stream_unbind = true;
        s_spp.stream.reserved = 0U;
    }
    spp_server_tx_reclaim_locked(session);
}

/* 预留只改元数据；每通道一个未发布区间，不持有跨等待租约。 */
static esp_err_t spp_server_reserve(uint32_t handle, size_t len, uint8_t channel, spp_server_tx_token_t *token)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if ((token == NULL) || (len == 0U) || (len > SPP_SERVER_STREAM_CAPACITY)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_spp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_session_t *session = spp_server_find_locked(handle);
    if (session != NULL) {
        spp_server_ring_t *ring = &session->reply;
        size_t capacity = SPP_SERVER_REPLY_CAPACITY;
        result = ESP_ERR_TIMEOUT;
        if (channel == SPP_SERVER_CHANNEL_STREAM) {
            if ((s_spp.stream_owner != (session->idx + 1U)) || s_spp.stream_unbind) {
                taskEXIT_CRITICAL(&s_spp_mux);
                return ESP_ERR_INVALID_STATE;
            }
            ring = &s_spp.stream;
            capacity = SPP_SERVER_STREAM_CAPACITY;
        }
        if (!session->disconnect_pending && !ring->copying && (ring->reserved == 0U) &&
            (len <= (capacity - ring->used))) {
            /* len <=4096，uint16_t 足够；slot<2、channel<=1。 */
            ring->reserved = (uint16_t)len;
            ++ring->serial;
            *token = (spp_server_tx_token_t){session->generation, ring->serial, (uint16_t)len, session->idx, channel};
            result = ESP_OK;
        } else {
            s_spp.tx_stats.paused++;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return result;
}

esp_err_t spp_server_reply_reserve(uint32_t handle, spp_server_tx_token_t *token)
{
    return spp_server_reserve(handle, SPP_SERVER_REPLY_RESERVE, SPP_SERVER_CHANNEL_REPLY, token);
}

esp_err_t spp_server_stream_reserve(uint32_t handle, size_t len, spp_server_tx_token_t *token)
{
    return spp_server_reserve(handle, len, SPP_SERVER_CHANNEL_STREAM, token);
}

static spp_server_ring_t *spp_server_token_ring_locked(const spp_server_tx_token_t *token)
{
    if ((token == NULL) || (token->slot >= SPP_SERVER_MAX_SESSIONS) || (token->channel > SPP_SERVER_CHANNEL_STREAM)) {
        return NULL;
    }
    spp_server_session_t *session = &s_spp.sessions[token->slot];
    spp_server_ring_t *ring = (token->channel == SPP_SERVER_CHANNEL_STREAM) ? &s_spp.stream : &session->reply;
    if (!session->in_use || (session->generation != token->generation) || (ring->serial != token->serial) ||
        (ring->reserved != token->capacity) || (token->capacity == 0U) ||
        ((token->channel == SPP_SERVER_CHANNEL_STREAM) &&
         ((s_spp.stream_owner != (token->slot + 1U)) || s_spp.stream_unbind))) {
        return NULL;
    }
    return ring;
}

void spp_server_tx_cancel(const spp_server_tx_token_t *token)
{
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_ring_t *ring = spp_server_token_ring_locked(token);
    if ((ring != NULL) && !ring->copying) {
        ring->reserved = 0U;
        s_spp.sessions[token->slot].tx_space = true;
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    spp_server_tx_wake();
}

/* 把 n 字节复制到环 pos 处（可跨环尾），返回下一写位置。 */
static size_t spp_server_ring_copy(uint8_t *storage, size_t capacity, size_t pos, const uint8_t *src, size_t n)
{
    const size_t first = (n <= (capacity - pos)) ? n : (capacity - pos);
    memcpy(storage + pos, src, first);
    memcpy(storage, src + first, n - first);
    return (pos + n) % capacity;
}

/* copy 的区间先固定，锁外 scatter copy，最后一次发布；CLOSE 期间不再接纳新 owner。
 * 两段按序拼成一条记录，句末位只标在第二段末尾，worker 不会在两段之间插入 reply。 */
static esp_err_t spp_server_tx_commit_spans(const spp_server_tx_token_t *token, const uint8_t *a, size_t alen,
                                            const uint8_t *b, size_t blen)
{
    uint8_t *storage = NULL;
    size_t capacity = 0U;
    size_t tail = 0U;
    const size_t len = alen + blen;
    if ((token == NULL) || (a == NULL) || (alen == 0U) || ((blen != 0U) && (b == NULL)) || (len > token->capacity)) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_ring_t *ring = spp_server_token_ring_locked(token);
    if ((ring == NULL) || ring->copying) {
        taskEXIT_CRITICAL(&s_spp_mux);
        return ESP_ERR_INVALID_STATE;
    }
    spp_server_session_t *session = &s_spp.sessions[token->slot];
    ring->copying = true;
    session->users++;
    capacity = (token->channel == SPP_SERVER_CHANNEL_STREAM) ? SPP_SERVER_STREAM_CAPACITY : SPP_SERVER_REPLY_CAPACITY;
    storage =
        (token->channel == SPP_SERVER_CHANNEL_STREAM) ? s_spp.storage.stream_buf : s_spp.storage.reply_buf[token->slot];
    tail = ((size_t)ring->head + ring->used) % capacity;
    taskEXIT_CRITICAL(&s_spp_mux);

    const size_t mid = spp_server_ring_copy(storage, capacity, tail, a, alen);
    if (blen != 0U) {
        (void)spp_server_ring_copy(storage, capacity, mid, b, blen);
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_spp_mux);
    ring = spp_server_token_ring_locked(token);
    if (ring != NULL) {
        /* used+len<=容量<=4096；句末位在发布 used 前更新，worker 不会看到半句。 */
        ring->used += (uint16_t)len;
        ring->reserved = 0U;
        if (token->channel == SPP_SERVER_CHANNEL_STREAM) {
            const size_t end = (tail + len - 1U) % capacity;
            s_spp.stream_ends[end / 8U] |= (uint8_t)(1U << (end % 8U));
        } else {
            session->tx_total += (uint32_t)len;
        }
        s_spp.tx_stats.accepted += (uint32_t)len;
        if (ring->used > s_spp.tx_stats.high_water) {
            s_spp.tx_stats.high_water = ring->used;
        }
        session->tx_space = true;
        result = ESP_OK;
    }
    /* CLOSE 可使 token 失效；复制标志仍须清理，否则槽永远不能再用。 */
    if (token->channel == SPP_SERVER_CHANNEL_STREAM) {
        s_spp.stream.copying = false;
    } else {
        session->reply.copying = false;
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    spp_server_session_release(session);
    spp_server_tx_wake();
    return result;
}

esp_err_t spp_server_tx_commit(const spp_server_tx_token_t *token, const uint8_t *data, size_t len)
{
    return spp_server_tx_commit_spans(token, data, len, NULL, 0U);
}

esp_err_t spp_server_tx_commit2(const spp_server_tx_token_t *token, const uint8_t *a, size_t alen, const uint8_t *b,
                                size_t blen)
{
    if ((token == NULL) || (a == NULL) || (alen == 0U) || (b == NULL) || (blen == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return spp_server_tx_commit_spans(token, a, alen, b, blen);
}

esp_err_t spp_server_stream_bind(uint32_t handle, bool phone)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_session_t *session = spp_server_find_locked(handle);
    if (session != NULL) {
        result = ESP_OK;
        if (phone && (s_spp.stream_owner == 0U)) {
            s_spp.stream_owner = session->idx + 1U;
            memset(s_spp.stream_ends, 0, sizeof(s_spp.stream_ends));
        } else if (phone && ((s_spp.stream_owner != (session->idx + 1U)) || s_spp.stream_unbind)) {
            result = ESP_ERR_TIMEOUT;
        } else if (!phone && (s_spp.stream_owner == (session->idx + 1U))) {
            s_spp.stream_unbind = true;
            s_spp.stream.reserved = 0U;
            spp_server_tx_reclaim_locked(session);
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return result;
}

esp_err_t spp_server_push_latest(uint32_t handle, const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U) || (len > SPP_SERVER_PUSH_CAPACITY)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_session_t *session = spp_server_find_locked(handle);
    if ((session != NULL) && !session->disconnect_pending) {
        s_spp.tx_stats.cancelled += session->push_len;
        memcpy(session->push, data, len);  /* <=40B，独立小邮箱，不占用 reply 预留 */
        session->push_len = (uint16_t)len; /* len<=40 */
        s_spp.tx_stats.accepted += (uint32_t)len;
        result = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    spp_server_tx_wake();
    return result;
}

bool spp_server_reply_ready(uint32_t handle)
{
    bool ready = false;
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_session_t *session = spp_server_find_locked(handle);
    if (session != NULL) {
        ready = !session->disconnect_pending && !session->reply.copying && (session->reply.reserved == 0U) &&
                ((SPP_SERVER_REPLY_CAPACITY - session->reply.used) >= SPP_SERVER_REPLY_RESERVE);
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return ready;
}

void spp_server_tx_stats(spp_server_tx_stats_t *stats)
{
    if (stats != NULL) {
        taskENTER_CRITICAL(&s_spp_mux);
        *stats = s_spp.tx_stats;
        taskEXIT_CRITICAL(&s_spp_mux);
    }
}

/* 只在 mux 内运行。SDK 按 handle 顺序完成；同步提交失败也排在旧描述符后退休，避免错误释放环头。 */
static spp_server_inflight_t *spp_server_oldest_locked(uint8_t slot, bool pending_only)
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

static void spp_server_retire_locked(spp_server_session_t *session)
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
                ring->head = (uint16_t)(((size_t)ring->head + item->len) % capacity); /* 容量<=4096 */
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

/* 只取完整已发布句的前缀，清除已取句末位；跨句前优先回到 reply 调度。 */
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

/* 需持 s_spp_mux：返回第一个空闲在途槽，无空槽返回 NULL。 */
static spp_server_inflight_t *spp_server_free_item_locked(void)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
        if (!s_spp.inflight[idx].active) {
            return &s_spp.inflight[idx];
        }
    }
    return NULL;
}

/* worker 串行调用：merge 不被第二个 API 复用；回调早于返回也保留 submitting/users。 */
static bool spp_server_worker_submit(void)
{
    spp_server_inflight_t *item = NULL;
    spp_server_session_t *session = NULL;
    const uint8_t *storage = NULL;
    size_t capacity = 0U;
    size_t offset = 0U;
    /* 提交前快照：锁外 memcpy/API/trace 不再访问可能已被 retire 掉的在途描述符。 */
    spp_server_inflight_t submitted = {0};
    const TickType_t now = xTaskGetTickCount();
    taskENTER_CRITICAL(&s_spp_mux);
    item = spp_server_free_item_locked();
    if (item != NULL) {
        for (uint32_t step = 0U; step < SPP_SERVER_MAX_SESSIONS; ++step) {
            const uint8_t idx = (s_spp.tx_cursor + step) % SPP_SERVER_MAX_SESSIONS; /* 余数<2 */
            spp_server_session_t *candidate = &s_spp.sessions[idx];
            if (!candidate->in_use || candidate->congested || candidate->disconnect_pending) {
                continue;
            }
            spp_server_ring_t *ring = &candidate->reply;
            uint8_t channel = SPP_SERVER_CHANNEL_REPLY;
            capacity = SPP_SERVER_REPLY_CAPACITY;
            storage = s_spp.storage.reply_buf[idx];
            const bool stream =
                (s_spp.stream_owner == (idx + 1U)) && !s_spp.stream_unbind && (s_spp.stream.used > s_spp.stream.issued);
            if (stream && (s_spp.stream_active || ((ring->used == ring->issued) && (candidate->push_len == 0U)))) {
                ring = &s_spp.stream;
                capacity = SPP_SERVER_STREAM_CAPACITY;
                storage = s_spp.storage.stream_buf;
                channel = SPP_SERVER_CHANNEL_STREAM;
            }
            size_t len = ring->used - ring->issued;
            offset = ((size_t)ring->head + ring->issued) % capacity;
            if (len == 0U) {
                if (candidate->push_len == 0U) {
                    continue;
                }
                len = candidate->push_len;
                channel = SPP_SERVER_CHANNEL_PUSH;
                memcpy(s_spp.storage.merge_buf, candidate->push, len); /* <=40B */
                candidate->push_len = 0U;
                storage = NULL;
            } else if (channel == SPP_SERVER_CHANNEL_STREAM) {
                len = spp_server_stream_chunk_locked(offset, len);
            } else if (len > SPP_SERVER_MERGE_CAPACITY) {
                len = SPP_SERVER_MERGE_CAPACITY;
            }
            if (channel != SPP_SERVER_CHANNEL_PUSH) {
                ring->issued += (uint16_t)len; /* len<=1024 */
            }
            *item = (spp_server_inflight_t){.active = true,
                                            .submitting = true,
                                            .slot = idx,
                                            .channel = channel,
                                            .len = (uint16_t)len,
                                            .generation = candidate->generation,
                                            .handle = candidate->handle,
                                            .order = ++s_spp.tx_order,
                                            .tick = now};
            candidate->users++;
            submitted = *item;
            s_spp.tx_stats.inflight++;
            s_spp.tx_cursor = (idx + 1U) % SPP_SERVER_MAX_SESSIONS; /* 余数<2 */
            session = candidate;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    if (session == NULL) {
        return false;
    }
    if (storage != NULL) {
        const size_t first = (submitted.len <= (capacity - offset)) ? submitted.len : (capacity - offset);
        memcpy(s_spp.storage.merge_buf, storage + offset, first);
        memcpy(s_spp.storage.merge_buf + first, storage, submitted.len - first);
    }
    /* 唯一 SDK write 调用；len<=1024，转 int 安全。SDK 深拷贝并可能阻塞 BTC 投递。 */
    const esp_err_t result = esp_spp_write(submitted.handle, (int)submitted.len, s_spp.storage.merge_buf);
    taskENTER_CRITICAL(&s_spp_mux);
    item->submitting = false;
    if (result != ESP_OK) {
        if (!item->done) {
            item->done = true;
            item->result = result;
        }
        /* 同步提交失败可能已部分入队，不得继续续发尾部，立即关闭受影响会话。 */
        if (session->in_use && (session->generation == item->generation)) {
            session->disconnect_pending = true;
        }
    }
    spp_server_retire_locked(session);
    taskEXIT_CRITICAL(&s_spp_mux);
    spp_server_session_release(session);
    if (result != ESP_OK) {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_ERR, submitted.handle, (uint32_t)result, (uint32_t)submitted.len);
    }
    spp_server_tx_wake();
    return true;
}

/* 普通断开保留独立重试状态；只在 worker 内调用 SDK。 */
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
            spp_server_trace(SPP_SERVER_TRACE_CTRL_ERR, handle, (uint32_t)SPP_SERVER_TRACE_CTRL_DISCONNECT,
                             (uint32_t)err);
        }
    }
}

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

/* SDK 回调无 request id：仅匹配相同 handle 的最早待完成块；generation 保护本地槽复用，
 * 不能辨识 SDK 假设外的“旧 handle 回调恰在新 handle 同值在途”的重复事件。 */
void spp_server_handle_write(esp_spp_cb_param_t *param)
{
    uint32_t candidates = 0U;
    uint32_t bytes = 0U;
    TickType_t tick = 0U;
    bool matched = false;
    bool success = false;
    const bool anonymous =
        (param->write.handle == 0U) && (param->write.len == -1) && (param->write.status != ESP_SPP_SUCCESS);
    taskENTER_CRITICAL(&s_spp_mux);
    if (anonymous) {
        s_spp.tx_stats.anon_write_fail++;
        for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
            const spp_server_inflight_t *item = &s_spp.inflight[idx];
            if (item->active && s_spp.sessions[item->slot].in_use &&
                (s_spp.sessions[item->slot].generation == item->generation)) {
                s_spp.sessions[item->slot].disconnect_pending = true;
                candidates |= 1U << item->slot;
            }
        }
    } else {
        spp_server_session_t *session = spp_server_find_locked(param->write.handle);
        spp_server_inflight_t *item = (session != NULL) ? spp_server_oldest_locked(session->idx, true) : NULL;
        if ((item != NULL) && (item->generation == session->generation)) {
            matched = true;
            bytes = item->len;
            tick = item->tick;
            success = (param->write.status == ESP_SPP_SUCCESS) && (param->write.len == (int)item->len);
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
        spp_server_trace(SPP_SERVER_TRACE_WRITE_ANON, 0U, (uint32_t)param->write.status, candidates);
    } else if (!matched) {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_STRAY, param->write.handle,
                         (param->write.len > 0) ? (uint32_t)param->write.len : 0U, (uint32_t)param->write.status);
    } else if (success) {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_DONE, param->write.handle, bytes,
                         (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - tick));
    } else {
        spp_server_trace(SPP_SERVER_TRACE_WRITE_ERR, param->write.handle, (uint32_t)ESP_FAIL, bytes);
    }
    spp_server_tx_wake();
}

static bool spp_server_wait_change(uint8_t idx, TickType_t start, uint32_t timeout_ms)
{
    const TickType_t elapsed = xTaskGetTickCount() - start;
    const TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    if (elapsed >= budget) {
        return false;
    }
    (void)xEventGroupWaitBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(idx), pdFALSE, pdFALSE, budget - elapsed);
    return true;
}

/* 同步兼容路径：每块等待真实完成；timeout 不撤回已接纳/已提交字节。 */
static esp_err_t spp_server_write_chunk(spp_server_session_t *session, const uint8_t *data, size_t len,
                                        TickType_t start, uint32_t timeout_ms)
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
        if (ready && (spp_server_reserve(session->handle, len, SPP_SERVER_CHANNEL_REPLY, &token) == ESP_OK)) {
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
        const size_t chunk = ((len - offset) < SPP_SERVER_TX_CHUNK) ? (len - offset) : SPP_SERVER_TX_CHUNK;
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
