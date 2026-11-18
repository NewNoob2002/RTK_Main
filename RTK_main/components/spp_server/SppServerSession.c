/* 会话槽与控制表的簿记：find/acquire/release/alloc/free、拥塞标志，以及控制表的登记与排空。
 * 线程：find/alloc/free/set_congested/ctrl_add/ctrl_drop_reject 在 BTC 回调线程，acquire/release 在链路/发送任务，ctrl_process 只在发送任务。
 * 堆与阻塞：无堆分配；会话表与控制表共用 s_spp_mux，临界区内不调用 FreeRTOS/SDK，唤醒都在锁外。 */

#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

#include "SppServerPriv.h"

#define SPP_SERVER_PIN_LEN (4U)

/* 全部跨文件共享状态的唯一定义点；payload 留在 .bss。 */
spp_server_state_t s_spp;
portMUX_TYPE s_spp_mux = portMUX_INITIALIZER_UNLOCKED;

spp_server_session_t *spp_server_session_find(uint32_t handle)
{
    spp_server_session_t *found = NULL;

    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            found = &s_spp.sessions[idx];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return found;
}

spp_server_session_t *spp_server_session_acquire(uint32_t handle)
{
    spp_server_session_t *found = NULL;

    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            s_spp.sessions[idx].users++;
            found = &s_spp.sessions[idx];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
    return found;
}

void spp_server_session_release(spp_server_session_t *session)
{
    if (session == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_spp_mux);
    if (session->users > 0U) {
        session->users--;
    }
    spp_server_tx_reclaim_locked(session);
    taskEXIT_CRITICAL(&s_spp_mux);
}

/**
 * @brief  句柄是否正等待投递 REJECT
 * @param  handle: 会话句柄
 * @retval true 是；false 否
 * @note   需持 s_spp_mux。
 */
static bool spp_server_reject_pending_locked(uint32_t handle)
{
    for (uint32_t ctrl = 0U; ctrl < SPP_SERVER_CTRL_MAX; ctrl++) {
        if (s_spp.ctrl[ctrl].in_use && (s_spp.ctrl[ctrl].type == SPP_SERVER_CTRL_REJECT) &&
            (s_spp.ctrl[ctrl].handle == handle)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief  句柄是否已被在途会话占用（防止复用）
 * @param  handle: 会话句柄
 * @retval true 已占用；false 空闲
 * @note   需持 s_spp_mux。
 */
static bool spp_server_live_handle_locked(uint32_t handle)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        const spp_server_session_t *session = &s_spp.sessions[idx];
        if ((session->lifecycle_pending || session->disconnect_submitting ||
             (session->users != 0U)) &&
            (session->handle == handle)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief  找一个空闲槽并置 reserving，跳过正在退出的槽
 * @retval 预留到的槽；NULL 表示没有空闲槽
 * @note   需持 s_spp_mux；调用方负责 reset 与发布。
 */
static spp_server_session_t *spp_server_reserve_locked(void)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        /* CLOSE 取消在途，API/copy 尚未返回时 users 保留源区。 */
        if ((!s_spp.sessions[idx].in_use) && (!s_spp.sessions[idx].reserving) &&
            (!s_spp.sessions[idx].lifecycle_pending) &&
            (!s_spp.sessions[idx].disconnect_submitting) && (s_spp.sessions[idx].users == 0U)) {
            s_spp.sessions[idx].reserving = true;
            return &s_spp.sessions[idx];
        }
    }
    return NULL;
}

spp_server_session_t *spp_server_session_alloc(uint32_t handle, const uint8_t *addr)
{
    spp_server_session_t *slot = NULL;

    taskENTER_CRITICAL(&s_spp_mux);
    /* 拒绝请求已被 worker 取走时仍占表，防止 SDK 返回前接纳相同数字句柄。 */
    const bool rejected = spp_server_reject_pending_locked(handle);
    if (!rejected && !spp_server_live_handle_locked(handle)) {
        slot = spp_server_reserve_locked();
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (slot == NULL) {
        return NULL;
    }
    if (slot->rx == NULL) {
        taskENTER_CRITICAL(&s_spp_mux);
        slot->reserving = false;
        taskEXIT_CRITICAL(&s_spp_mux);
        return NULL;
    }

    /* 此时 users==0 且无在途发送：reset 不会与接收交错，也不会破坏读者已取走的数据。 */
    (void)xStreamBufferReset(slot->rx);

    slot->handle = handle;
    for (uint32_t byte = 0U; byte < SPP_SERVER_ADDR_LEN; byte++) {
        slot->addr[byte] = addr[byte];
    }
    slot->congested = false;
    slot->generation++;
    memset(&slot->reply, 0, sizeof(slot->reply));
    slot->push_len = 0U;
    slot->tx_space = false;
    slot->tx_total = 0U;
    slot->tx_done = 0U;
    slot->tx_errors = 0U;
    slot->tx_result = ESP_OK;
    slot->users = 0U;
    /* slot 落在 sessions[] 内，指针差 < SPP_SERVER_MAX_SESSIONS(=2)，uint8_t 窄化安全 */
    slot->idx = (uint8_t)(slot - s_spp.sessions);

    taskENTER_CRITICAL(&s_spp_mux);
    slot->reserving = false;
    slot->lifecycle_pending = true;
    slot->data_pending = false;
    slot->drop_pending = false;
    slot->rx_data_missed = false;
    slot->rx_dropped = 0U;
    slot->rx_dropped_total = 0U;
    slot->rx_high_water = 0U;
    slot->rx_events = 0U;
    slot->drop_events = 0U;
    slot->disconnect_pending = false;
    slot->disconnect_submitted = false;
    slot->disconnect_attempted = false;
    slot->in_use = true;
    taskEXIT_CRITICAL(&s_spp_mux);
    return slot;
}

bool spp_server_session_free(uint32_t handle)
{
    bool found = false;
    uint8_t idx = 0U;

    taskENTER_CRITICAL(&s_spp_mux);
    for (idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            s_spp.sessions[idx].in_use = false;
            s_spp.sessions[idx].disconnect_pending = false;
            s_spp.sessions[idx].congested = false;
            spp_server_tx_close_locked(&s_spp.sessions[idx]);
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (found && (s_spp.rtos.evt_group != NULL)) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(idx));
    }
    if (found) {
        spp_server_tx_wake();
    }
    return found;
}

void spp_server_session_set_congested(uint32_t handle, bool congested)
{
    bool found = false;
    bool changed = false;
    uint8_t idx = 0U;

    taskENTER_CRITICAL(&s_spp_mux);
    for (idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            changed = (s_spp.sessions[idx].congested != congested);
            s_spp.sessions[idx].congested = congested;
            s_spp.sessions[idx].tx_space = true;
            if (changed && congested) {
                s_spp.tx_stats.cong++;
            }
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    /* 只记真实变化（a = 1 拥塞 / 0 解除），重复的 CONG_EVT 不刷屏。 */
    if (found && changed) {
        spp_server_trace(SPP_SERVER_TRACE_CONG, handle, congested ? 1U : 0U, 0U);
    }
    if (found) {
        spp_server_tx_wake();
    }
}

bool spp_server_ctrl_add(spp_server_ctrl_type_t type, uint32_t handle, const uint8_t *bda)
{
    bool added = false;
    uint32_t free_slot = SPP_SERVER_CTRL_MAX;

    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t slot = 0U; slot < SPP_SERVER_CTRL_MAX; slot++) {
        if ((type == SPP_SERVER_CTRL_REJECT) && s_spp.ctrl[slot].in_use &&
            (s_spp.ctrl[slot].type == type) && (s_spp.ctrl[slot].handle == handle)) {
            if (s_spp.ctrl[slot].closed) {
                s_spp.ctrl[slot].closed = false;
                s_spp.ctrl[slot].rearmed = true;
            }
            added = true;
            break;
        }
        if (!s_spp.ctrl[slot].in_use && (free_slot == SPP_SERVER_CTRL_MAX)) {
            free_slot = slot;
        }
    }
    if (!added && (free_slot < SPP_SERVER_CTRL_MAX)) {
        const uint32_t slot = free_slot;
        memset(&s_spp.ctrl[slot], 0, sizeof(s_spp.ctrl[slot]));
        s_spp.ctrl[slot].type = type;
        s_spp.ctrl[slot].handle = handle;
        for (uint32_t byte = 0U; byte < SPP_SERVER_ADDR_LEN; byte++) {
            s_spp.ctrl[slot].bda[byte] = (bda != NULL) ? bda[byte] : 0U;
        }
        s_spp.ctrl[slot].in_use = true;
        added = true;
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (added && (s_spp.rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_spp.rtos.worker);
    }
    return added;
}

void spp_server_ctrl_drop_reject(uint32_t handle)
{
    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t slot = 0U; slot < SPP_SERVER_CTRL_MAX; slot++) {
        if (s_spp.ctrl[slot].in_use && (s_spp.ctrl[slot].type == SPP_SERVER_CTRL_REJECT) &&
            (s_spp.ctrl[slot].handle == handle)) {
            if (s_spp.ctrl[slot].submitting) {
                s_spp.ctrl[slot].closed = true;
            } else {
                s_spp.ctrl[slot].in_use = false;
            }
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);
}

/* 发送任务上下文：执行控制表里的阻塞投递 API。 */
/* 本轮取走的一条控制请求；锁内填充，锁外只读。 */
typedef struct {
    spp_server_ctrl_type_t type;
    uint32_t handle;
    uint8_t bda[SPP_SERVER_ADDR_LEN];
} spp_server_ctrl_request_t;

/**
 * @brief  从会话取出待投递的控制请求并标记在途
 * @param  slot: 会话槽位下标
 * @param[out] req: 取出的请求
 * @param  now: 当前时钟节拍（用于超时判定）
 * @retval true 取到请求；false 无事可做
 * @note   需持 s_spp_mux。
 */
static bool spp_server_ctrl_claim_locked(uint32_t slot, spp_server_ctrl_request_t *req,
                                         TickType_t now)
{
    const bool has = s_spp.ctrl[slot].in_use && !s_spp.ctrl[slot].submitted &&
                     (!s_spp.ctrl[slot].attempted || ((now - s_spp.ctrl[slot].attempt) >=
                                                      pdMS_TO_TICKS(SPP_SERVER_CTRL_RETRY_MS)));
    if (!has) {
        return false;
    }
    req->type = s_spp.ctrl[slot].type;
    req->handle = s_spp.ctrl[slot].handle;
    for (uint32_t byte = 0U; byte < SPP_SERVER_ADDR_LEN; byte++) {
        req->bda[byte] = s_spp.ctrl[slot].bda[byte];
    }
    if (req->type == SPP_SERVER_CTRL_REJECT) {
        /* REJECT 保留在表中直到确认断开，便于 worker 重试。 */
        s_spp.ctrl[slot].submitting = true;
        s_spp.ctrl[slot].attempted = true;
        s_spp.ctrl[slot].attempt = now;
    } else {
        s_spp.ctrl[slot].in_use = false;
    }
    return true;
}

/**
 * @brief  把控制请求投递给蓝牙栈
 * @param  req: 请求内容（esp_bd_addr_t 需要可写指针）
 * @retval ESP_OK 成功；其他为 IDF 错误码
 * @note   锁外调用，可能阻塞。
 */
static esp_err_t spp_server_ctrl_deliver(spp_server_ctrl_request_t *req)
{
    esp_err_t err = ESP_OK;

    switch (req->type) {
    case SPP_SERVER_CTRL_PIN_REQ: {
        esp_bt_pin_code_t pin_code = {0};

        /* 逐字节复制固定 PIN：字面量长度与 SPP_SERVER_PIN_LEN 同源，避免硬编码下标。
         * 字面量只含 ASCII 数字（0x30..0x39），缩窄到 uint8_t 安全。 */
        for (uint32_t pin_idx = 0U; pin_idx < SPP_SERVER_PIN_LEN; pin_idx++) {
            pin_code[pin_idx] = (uint8_t)SPP_SERVER_LEGACY_PIN[pin_idx];
        }

        err = esp_bt_gap_pin_reply(req->bda, true, SPP_SERVER_PIN_LEN, pin_code);
        break;
    }
    case SPP_SERVER_CTRL_CFM_REQ:
        err = esp_bt_gap_ssp_confirm_reply(req->bda, true);
        break;
    case SPP_SERVER_CTRL_REJECT:
        /* 取走后、断开前若同句柄已被新会话接纳（IDF 复用了句柄），放弃这次断开。 */
        if (spp_server_session_find(req->handle) != NULL) {
            spp_server_trace(SPP_SERVER_TRACE_REJECT, req->handle, 0U, 0U);
            break;
        }
        err = esp_spp_disconnect(req->handle);
        if (err == ESP_OK) {
            spp_server_trace(SPP_SERVER_TRACE_REJECT, req->handle, 1U, 0U);
        }
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }
    return err;
}

/**
 * @brief  按投递结果结算控制请求的在途标志
 * @param  slot: 会话槽位下标
 * @param  err: 投递结果
 * @note   需持 s_spp_mux。
 */
static void spp_server_ctrl_settle_locked(uint32_t slot, esp_err_t err)
{
    s_spp.ctrl[slot].submitting = false;
    if (s_spp.ctrl[slot].rearmed) {
        s_spp.ctrl[slot].submitted = false;
        s_spp.ctrl[slot].attempted = false;
        s_spp.ctrl[slot].rearmed = false;
    } else {
        s_spp.ctrl[slot].submitted = (err == ESP_OK);
    }
    if (s_spp.ctrl[slot].closed) {
        s_spp.ctrl[slot].in_use = false;
    }
}

/**
 * @brief  每轮处理所有会话待投递的控制请求
 * @note   发送任务上下文；锁内取请求，锁外投递与追踪，只有 REJECT 需锁内结算。
 */
void spp_server_ctrl_process(void)
{
    for (uint32_t slot = 0U; slot < SPP_SERVER_CTRL_MAX; slot++) {
        spp_server_ctrl_request_t req = {0};
        const TickType_t now = xTaskGetTickCount();

        taskENTER_CRITICAL(&s_spp_mux);
        const bool has = spp_server_ctrl_claim_locked(slot, &req, now);
        taskEXIT_CRITICAL(&s_spp_mux);
        if (!has) {
            continue;
        }

        const esp_err_t err = spp_server_ctrl_deliver(&req);
        if (req.type == SPP_SERVER_CTRL_REJECT) {
            taskENTER_CRITICAL(&s_spp_mux);
            spp_server_ctrl_settle_locked(slot, err);
            taskEXIT_CRITICAL(&s_spp_mux);
        }
        if (err != ESP_OK) {
            /* 控制类型与追踪枚举一一对应（PIN/CONFIRM/REJECT），见 spp_server_trace_ctrl_t。 */
            spp_server_trace(SPP_SERVER_TRACE_CTRL_ERR, req.handle, (uint32_t)req.type,
                             (uint32_t)err);
        }
    }
}
