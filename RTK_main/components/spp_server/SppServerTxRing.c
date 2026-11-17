/* 非阻塞 TX 字节环与令牌提交：reserve 占用窗口、commit 发布、cancel 归还；跨环尾回绕复制在此。
 * 链路任务调用（非 ISR）；所有状态在 s_spp_mux 临界区内推进，大复制在锁外。无分配。 */
#include <stdint.h>
#include <string.h>

#include "SppServerPriv.h"

/* level 与通知分离：通知可合并，poll_event 在等待前重查每槽 level。 */
/**
 * @brief  有会话可写或流已空闲时唤醒发送任务
 * @note   中断安全：只置标志并投递事件/通知，不做写操作。
 */
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
        (void)xEventGroupSetBits(s_spp.rtos.evt_group,
                                 SPP_SERVER_BIT_WAKE(0) | SPP_SERVER_BIT_WAKE(1));
    }
    if (post) {
        spp_server_notify_rx_wake();
    }
}

/**
 * @brief  结算会话在途写后重试解绑旧流
 * @param  session: 会话
 * @note   需持 s_spp_mux；旧流回调不能归还给未来的 owner。
 */
void spp_server_tx_reclaim_locked(spp_server_session_t *session)
{
    if ((s_spp.stream_owner == (session->idx + 1U)) && s_spp.stream_unbind &&
        (session->users == 0U)) {
        /* 旧流的回调也不能归还给未来 owner；解绑必须等旧流全部结算或 CLOSE。 */
        bool busy = false;
        for (uint32_t idx = 0U; idx < SPP_SERVER_INFLIGHT_MAX; ++idx) {
            busy =
                busy || (s_spp.inflight[idx].active && (s_spp.inflight[idx].slot == session->idx) &&
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

/**
 * @brief  关闭会话的发送状态：清预留与 PUSH 邮箱并记取消字节
 * @param  session: 会话
 * @note   需持 s_spp_mux。
 */
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
esp_err_t spp_server_reserve(uint32_t handle, size_t len, uint8_t channel,
                             spp_server_tx_token_t *token)
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
            *token = (spp_server_tx_token_t){session->generation, ring->serial, (uint16_t)len,
                                             session->idx, channel};
            result = ESP_OK;
        } else {
            s_spp.tx_stats.paused++;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return result;
}

/**
 * @brief  为一条应答预留发送空间
 * @param  handle: 会话句柄
 * @param[out] token: 预留凭证
 * @retval ESP_OK 成功；其他为失败原因
 */
esp_err_t spp_server_reply_reserve(uint32_t handle, spp_server_tx_token_t *token)
{
    return spp_server_reserve(handle, SPP_SERVER_REPLY_RESERVE, SPP_SERVER_CHANNEL_REPLY, token);
}

/**
 * @brief  为一块流数据预留发送空间
 * @param  handle: 会话句柄
 * @param  len: 字节数
 * @param[out] token: 预留凭证
 * @retval ESP_OK 成功；其他为失败原因
 */
esp_err_t spp_server_stream_reserve(uint32_t handle, size_t len, spp_server_tx_token_t *token)
{
    return spp_server_reserve(handle, len, SPP_SERVER_CHANNEL_STREAM, token);
}

/**
 * @brief  取 token 对应的环
 * @param  token: 预留凭证
 * @retval 环形缓冲区；token 非法时为 NULL
 * @note   需持 s_spp_mux。
 */
static spp_server_ring_t *spp_server_token_ring_locked(const spp_server_tx_token_t *token)
{
    if ((token == NULL) || (token->slot >= SPP_SERVER_MAX_SESSIONS) ||
        (token->channel > SPP_SERVER_CHANNEL_STREAM)) {
        return NULL;
    }
    spp_server_session_t *session = &s_spp.sessions[token->slot];
    spp_server_ring_t *ring =
        (token->channel == SPP_SERVER_CHANNEL_STREAM) ? &s_spp.stream : &session->reply;
    if (!session->in_use || (session->generation != token->generation) ||
        (ring->serial != token->serial) || (ring->reserved != token->capacity) ||
        (token->capacity == 0U) ||
        ((token->channel == SPP_SERVER_CHANNEL_STREAM) &&
         ((s_spp.stream_owner != (token->slot + 1U)) || s_spp.stream_unbind))) {
        return NULL;
    }
    return ring;
}

/**
 * @brief  撤销未提交的预留并唤醒发送任务
 * @param  token: 预留凭证
 */
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

/**
 * @brief  把 n 字节写入环 pos 处（可跨环尾）
 * @param  storage: 环存储
 * @param  capacity: 环容量
 * @param  pos: 写位置
 * @param  src: 源数据
 * @param  n: 字节数
 * @retval 下一写位置
 */
static size_t spp_server_ring_copy(uint8_t *storage, size_t capacity, size_t pos,
                                   const uint8_t *src, size_t n)
{
    const size_t first = (n <= (capacity - pos)) ? n : (capacity - pos);
    memcpy(storage + pos, src, first);
    memcpy(storage, src + first, n - first);
    return (pos + n) % capacity;
}

/* copy 的区间先固定，锁外 scatter copy，最后一次发布；CLOSE 期间不再接纳新 owner。
 * 两段按序拼成一条记录，句末位只标在第二段末尾，worker 不会在两段之间插入 reply。 */
/**
 * @brief  把两段数据原子提交到 token 预留的空间
 * @param  token: 预留凭证
 * @param  a: 第一段数据
 * @param  alen: 第一段字节数
 * @param  b: 第二段数据（可为 NULL）
 * @param  blen: 第二段字节数
 * @retval ESP_OK 成功；其他为失败原因
 */
static esp_err_t spp_server_tx_commit_spans(const spp_server_tx_token_t *token, const uint8_t *a,
                                            size_t alen, const uint8_t *b, size_t blen)
{
    uint8_t *storage = NULL;
    size_t capacity = 0U;
    size_t tail = 0U;
    const size_t len = alen + blen;
    if ((token == NULL) || (a == NULL) || (alen == 0U) || ((blen != 0U) && (b == NULL)) ||
        (len > token->capacity)) {
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
    capacity = (token->channel == SPP_SERVER_CHANNEL_STREAM) ? SPP_SERVER_STREAM_CAPACITY
                                                             : SPP_SERVER_REPLY_CAPACITY;
    storage = (token->channel == SPP_SERVER_CHANNEL_STREAM) ? s_spp.storage.stream_buf
                                                            : s_spp.storage.reply_buf[token->slot];
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

/**
 * @brief  提交单段数据
 * @param  token: 预留凭证
 * @param  data: 数据
 * @param  len: 字节数
 * @retval ESP_OK 成功；其他为失败原因
 */
esp_err_t spp_server_tx_commit(const spp_server_tx_token_t *token, const uint8_t *data, size_t len)
{
    return spp_server_tx_commit_spans(token, data, len, NULL, 0U);
}

/**
 * @brief  提交两段数据（头部与载荷分处两个缓冲）
 * @param  token: 预留凭证
 * @param  a: 第一段数据
 * @param  alen: 第一段字节数
 * @param  b: 第二段数据
 * @param  blen: 第二段字节数
 * @retval ESP_OK 成功；其他为失败原因
 */
esp_err_t spp_server_tx_commit2(const spp_server_tx_token_t *token, const uint8_t *a, size_t alen,
                                const uint8_t *b, size_t blen)
{
    if ((token == NULL) || (a == NULL) || (alen == 0U) || (b == NULL) || (blen == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return spp_server_tx_commit_spans(token, a, alen, b, blen);
}

/**
 * @brief  绑定流归属并记录对端是否手机
 * @param  handle: 会话句柄
 * @param  phone: 是否手机会话
 * @retval ESP_OK 成功；ESP_ERR_NOT_FOUND 表示会话不存在
 */
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

/**
 * @brief  用最新一帧覆盖会话的 PUSH 邮箱
 * @param  handle: 会话句柄
 * @param  data: 帧数据（<= SPP_SERVER_PUSH_CAPACITY）
 * @param  len: 字节数
 * @retval ESP_OK 成功；其他为失败原因
 */
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

/**
 * @brief  会话当前是否还能接收一条应答
 * @param  handle: 会话句柄
 * @retval true 可以；false 断开中或剩余空间不足
 */
bool spp_server_reply_ready(uint32_t handle)
{
    bool ready = false;
    taskENTER_CRITICAL(&s_spp_mux);
    spp_server_session_t *session = spp_server_find_locked(handle);
    if (session != NULL) {
        ready = !session->disconnect_pending && !session->reply.copying &&
                (session->reply.reserved == 0U) &&
                ((SPP_SERVER_REPLY_CAPACITY - session->reply.used) >= SPP_SERVER_REPLY_RESERVE);
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return ready;
}

/**
 * @brief  读取发送统计快照
 * @param[out] stats: 统计输出（NULL 时忽略）
 */
void spp_server_tx_stats(spp_server_tx_stats_t *stats)
{
    if (stats != NULL) {
        taskENTER_CRITICAL(&s_spp_mux);
        *stats = s_spp.tx_stats;
        taskEXIT_CRITICAL(&s_spp_mux);
    }
}

/* 只在 mux 内运行。SDK 按 handle 顺序完成；同步提交失败也排在旧描述符后退休，避免错误释放环头。 */
