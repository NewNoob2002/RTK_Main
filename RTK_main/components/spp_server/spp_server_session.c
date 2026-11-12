/* 会话槽与控制表的簿记：find/acquire/release/alloc/free、拥塞标志，以及控制表的登记与排空。
 * 控制表与会话表共用 s_spp_mux，放在同一文件便于审查锁范围（alloc 也要查控制表）。
 * find/alloc/free/set_congested/ctrl_add/ctrl_drop_reject 允许 BTC 回调线程调用：只改静态表，
 * 临界区外只做非阻塞唤醒；ctrl_process 只在发送任务上下文执行可能阻塞投递的 GAP 应答与断开。 */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"

#include "spp_server_priv.h"

#define SPP_SERVER_PIN_LEN (4U)

/* 全部跨文件共享状态的唯一定义点。s_spp 零初始化（.bss）；初值非零的两项单独定义。 */
spp_server_state_t s_spp;
portMUX_TYPE s_spp_mux = portMUX_INITIALIZER_UNLOCKED;
int s_spp_tx_inflight = -1;

/* 回调上下文使用：只查表，不登记 users（BTC 是 StreamBuffer 的唯一写者）。 */
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

/* 登记一个读者/写者；返回 NULL 表示句柄已失效。登记后该槽不会被复用或 reset。 */
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

/* 注销读者/写者。槽在 users 归零前不会被 SRV_OPEN 复用。 */
void spp_server_session_release(spp_server_session_t *session)
{
    if (session == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_spp_mux);
    if (session->users > 0U) {
        session->users--;
    }
    taskEXIT_CRITICAL(&s_spp_mux);
}

/* 分配空闲槽：要求无读者、无在途发送（否则 reset 会与读/写并发或复用未退出的会话）。
 * BTC 回调（快路径）与发送任务（延后重试）都会调用，用 reserving 串行化。 */
spp_server_session_t *spp_server_session_alloc(uint32_t handle, const uint8_t *addr)
{
    spp_server_session_t *slot = NULL;
    uint32_t idx = 0U;

    taskENTER_CRITICAL(&s_spp_mux);
    /* 拒绝请求已被 worker 取走时仍占表，防止 SDK 返回前接纳相同数字句柄。 */
    for (uint32_t ctrl = 0U; ctrl < SPP_SERVER_CTRL_MAX; ctrl++) {
        if (s_spp.ctrl[ctrl].in_use && (s_spp.ctrl[ctrl].type == SPP_SERVER_CTRL_REJECT) &&
            (s_spp.ctrl[ctrl].handle == handle)) {
            taskEXIT_CRITICAL(&s_spp_mux);
            return NULL;
        }
    }
    for (idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        if ((s_spp.sessions[idx].lifecycle_pending || s_spp.sessions[idx].disconnect_submitting) &&
            (s_spp.sessions[idx].handle == handle)) {
            taskEXIT_CRITICAL(&s_spp_mux);
            return NULL;
        }
    }
    for (idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        /* 该槽还有 IDF 在途提交（含 CLOSE 取消、等 worker 从 API 返回）时不可复用，
         * 避免静态 tx_buf/迟到事件污染新会话。 */
        if ((!s_spp.sessions[idx].in_use) && (!s_spp.sessions[idx].reserving) &&
            (!s_spp.sessions[idx].lifecycle_pending) && (!s_spp.sessions[idx].disconnect_submitting) &&
            (s_spp.sessions[idx].users == 0U) && (!s_spp.sessions[idx].tx_submitting) &&
            (s_spp.sessions[idx].tx_state == SPP_SERVER_TX_IDLE) && (s_spp_tx_inflight != (int)idx)) {
            s_spp.sessions[idx].reserving = true;
            slot = &s_spp.sessions[idx];
            break;
        }
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
    slot->tx_submitting = false;
    slot->tx_state = SPP_SERVER_TX_IDLE;
    slot->tx_seq = 0U;
    slot->tx_done_seq = 0U;
    slot->tx_result = ESP_OK;
    slot->tx_len = 0U;
    slot->tx_done_len = 0;
    slot->users = 0U;
    /* idx < SPP_SERVER_MAX_SESSIONS(=2)，uint8_t 窄化安全 */
    slot->idx = (uint8_t)idx;

    taskENTER_CRITICAL(&s_spp_mux);
    slot->reserving = false;
    slot->lifecycle_pending = true;
    slot->data_pending = false;
    slot->drop_pending = false;
    slot->rx_dropped = 0U;
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
    bool release_owner = false;
    uint8_t idx = 0U;

    taskENTER_CRITICAL(&s_spp_mux);
    for (idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        if (s_spp.sessions[idx].in_use && (s_spp.sessions[idx].handle == handle)) {
            s_spp.sessions[idx].in_use = false;
            s_spp.sessions[idx].disconnect_pending = false;
            s_spp.sessions[idx].congested = false;
            /* 让还在等的写者立刻拿到 INVALID_STATE。 */
            s_spp.sessions[idx].tx_state = SPP_SERVER_TX_IDLE;
            s_spp.sessions[idx].tx_result = ESP_ERR_INVALID_STATE;
            s_spp.sessions[idx].tx_done_seq = s_spp.sessions[idx].tx_seq;
            /* CLOSE 就是取消完成：IDF 在 CB 模式 CLOSE 时清空未完成 TX，不逐条合成 WRITE
             * （btc_spp.c:1188-1204 → spp_free_slot → :277-298 → :121-124）。
             * worker 已不在 esp_spp_write 内时立即释放 owner；仍在调用中则只标记取消，
             * 等该调用返回（静态 tx_buf 不再被使用）再由 worker 释放。 */
            if (s_spp_tx_inflight == (int)idx) {
                if (s_spp.sessions[idx].tx_submitting) {
                    s_spp.tx_inflight_cancelled = true;
                } else {
                    s_spp_tx_inflight = -1;
                    s_spp.tx_inflight_cancelled = false;
                    release_owner = true;
                }
            }
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    if (found && (s_spp.rtos.evt_group != NULL)) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(idx));
    }
    /* owner 释放后唤醒 worker，提交其他会话的排队请求（循环只按通知重新扫描）。 */
    if (release_owner && (s_spp.rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_spp.rtos.worker);
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
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spp_mux);

    /* 只记真实变化（a = 1 拥塞 / 0 解除），重复的 CONG_EVT 不刷屏。 */
    if (found && changed) {
        spp_server_trace(SPP_SERVER_TRACE_CONG, handle, congested ? 1U : 0U, 0U);
    }
    if (found && (s_spp.rtos.evt_group != NULL)) {
        (void)xEventGroupSetBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_WAKE(idx));
    }
}

/* 登记一条控制请求；回调上下文调用，只改静态表并在临界区外非阻塞唤醒发送任务。
 * 表满返回 false（回调不能等待，只能丢弃并记录）。 */
bool spp_server_ctrl_add(spp_server_ctrl_type_t type, uint32_t handle, const uint8_t *bda)
{
    bool added = false;
    uint32_t free_slot = SPP_SERVER_CTRL_MAX;

    taskENTER_CRITICAL(&s_spp_mux);
    for (uint32_t slot = 0U; slot < SPP_SERVER_CTRL_MAX; slot++) {
        if ((type == SPP_SERVER_CTRL_REJECT) && s_spp.ctrl[slot].in_use && (s_spp.ctrl[slot].type == type) &&
            (s_spp.ctrl[slot].handle == handle)) {
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

/* 会话已关闭：撤销尚未执行的同句柄拒绝命令，避免稍后误断被复用的句柄。
 * 已由发送任务取走的命令无法撤销（窗口很小），见 ctrl_process 里的在册检查。 */
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
void spp_server_ctrl_process(void)
{
    for (uint32_t slot = 0U; slot < SPP_SERVER_CTRL_MAX; slot++) {
        bool has = false;
        spp_server_ctrl_type_t type = SPP_SERVER_CTRL_PIN_REQ;
        uint32_t handle = 0U;
        uint8_t bda[SPP_SERVER_ADDR_LEN] = {0};
        esp_err_t err = ESP_OK;

        const TickType_t now = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_spp_mux);
        has = s_spp.ctrl[slot].in_use && !s_spp.ctrl[slot].submitted &&
              (!s_spp.ctrl[slot].attempted ||
               ((now - s_spp.ctrl[slot].attempt) >= pdMS_TO_TICKS(SPP_SERVER_CTRL_RETRY_MS)));
        if (has) {
            type = s_spp.ctrl[slot].type;
            handle = s_spp.ctrl[slot].handle;
            for (uint32_t byte = 0U; byte < SPP_SERVER_ADDR_LEN; byte++) {
                bda[byte] = s_spp.ctrl[slot].bda[byte];
            }
            if (type == SPP_SERVER_CTRL_REJECT) {
                s_spp.ctrl[slot].submitting = true;
                s_spp.ctrl[slot].attempted = true;
                s_spp.ctrl[slot].attempt = now;
            } else {
                s_spp.ctrl[slot].in_use = false;
            }
        }
        taskEXIT_CRITICAL(&s_spp_mux);

        if (!has) {
            continue;
        }

        switch (type) {
        case SPP_SERVER_CTRL_PIN_REQ: {
            esp_bt_pin_code_t pin_code = {0};

            /* 逐字节复制固定 PIN：字面量长度与 SPP_SERVER_PIN_LEN 同源，避免硬编码下标。
             * 字面量只含 ASCII 数字（0x30..0x39），缩窄到 uint8_t 安全。 */
            for (uint32_t pin_idx = 0U; pin_idx < SPP_SERVER_PIN_LEN; pin_idx++) {
                pin_code[pin_idx] = (uint8_t)SPP_SERVER_LEGACY_PIN[pin_idx];
            }

            err = esp_bt_gap_pin_reply(bda, true, SPP_SERVER_PIN_LEN, pin_code);
            break;
        }
        case SPP_SERVER_CTRL_CFM_REQ:
            err = esp_bt_gap_ssp_confirm_reply(bda, true);
            break;
        case SPP_SERVER_CTRL_REJECT:
            /* 取走后、断开前若同句柄已被新会话接纳（IDF 复用了句柄），放弃这次断开。 */
            if (spp_server_session_find(handle) != NULL) {
                ESP_LOGW(SPP_SERVER_TAG, "reject handle=%" PRIu32 " skipped: session already active", handle);
                break;
            }
            err = esp_spp_disconnect(handle);
            if (err != ESP_OK) {
                ESP_LOGW(SPP_SERVER_TAG, "reject handle=%" PRIu32 ": %s", handle, esp_err_to_name(err));
            } else {
                ESP_LOGW(SPP_SERVER_TAG, "reject handle=%" PRIu32 " (no safe session slot)", handle);
            }
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }

        if (type == SPP_SERVER_CTRL_REJECT) {
            taskENTER_CRITICAL(&s_spp_mux);
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
            taskEXIT_CRITICAL(&s_spp_mux);
        }
        if (err != ESP_OK) {
            ESP_LOGW(SPP_SERVER_TAG, "ctrl type=%d: %s", (int)type, esp_err_to_name(err));
        }
    }
}
