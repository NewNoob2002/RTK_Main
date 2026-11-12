// Bluedroid 经典蓝牙 SPP 多会话服务端驱动（ADR-0003：C 驱动 + C++ 适配器）。
// 由 reference/c-v1/components/bt_spp/bt_spp.c 移植：符号改为 rtk_bt_spp_*、布局改为
// ESP-IDF Style、静态变量 s_ 前缀；PIN_REQ 应答改为 RTK_BT_SPP_LEGACY_PIN（"1234"）；
// 未知句柄的 write/disconnect 返回 ESP_ERR_NOT_FOUND；新增 rtk_bt_spp_dropped_events()。
//
// 线程模型（对应 docs/reports/QA-E-REVIEW.md 第 1–4 项）：
// - BTC 回调（rtk_bt_spp_cb / rtk_bt_spp_gap_cb）：只拷贝数据、记录完成状态、置事件位、非阻塞投递；
//   不调用任何可能阻塞投递的 API。esp_spp_write / esp_spp_disconnect / esp_spp_start_srv 都经
//   btc_transfer_context → btc_task_post(OSI_THREAD_MAX_TIMEOUT)（IDF btc_task.c:366），
//   后者阻塞在 xQueueSend(portMAX_DELAY)，在回调里调用会把 BTC 任务自己锁死。
// - 发送任务 rtk_bt_spp_worker（静态创建、绑核 0）：进程内唯一调用 esp_spp_write 的位置，
//   每次只提交一个分块并等待该分块的真实 WRITE_EVT，因此相邻两次 rtk_bt_spp_write 不会把
//   请求堆进 IDF 每会话深度 10 的 TX 队列；提交动作在任务里做，调用方只会阻塞在有界等待上。
//   全驱动同一时刻至多一个未完成的 IDF 提交（s_tx_inflight），因此 handle=0/len=-1 的匿名
//   提交错误可以唯一归属；完成事件会非阻塞唤醒该任务继续提交其他会话的排队请求。
//   该任务还处理静态控制表：GAP 的 PIN/CFM 应答与“无安全空槽”的断开请求都从任务上下文执行
//   （这些 API 经 btc_transfer_context 投递，回调里调用会阻塞 BTC 任务）。
// - 链路任务（poll_event/read/write/disconnect 的调用者）：单一所有者；read/write 期间登记
//   session->users，槽在读者/写者退出前不会被新连接复用，也不会被 reset。
// - SRV_OPEN 只在 BTC 回调里分配一次槽：找不到 users==0 且无在途发送的安全空槽时，登记拒绝
//   命令并由发送任务断开该连接；不做延后接纳（避免 CLOSE 后重现幽灵会话）。
// 会话表、发送状态、控制表由 portMUX 临界区保护；临界区内不调用 FreeRTOS API、不阻塞。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "sdkconfig.h"

#if !CONFIG_SOC_BT_CLASSIC_SUPPORTED
#error "rtk_bt_spp.c 只支持 SOC_BT_CLASSIC_SUPPORTED 目标（经典蓝牙 SPP）"
#endif

/* 事件位与无符号 tick 差值比较都要求 32 位 EventBits_t / TickType_t。 */
#if configUSE_16_BIT_TICKS
#error "rtk_bt_spp.c 需要 32 位 tick（configUSE_16_BIT_TICKS 必须为 0）"
#endif

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"

#include "rtk_bt_spp.h"

#define RTK_BT_SPP_TAG            "rtk_bt_spp"
#define RTK_BT_SPP_SERVER_NAME    "SPP_SERVER"
#define RTK_BT_SPP_STREAM_TRIGGER (1U)
#define RTK_BT_SPP_PIN_LEN        (4U)

/* 单次交给 esp_spp_write 的最大字节数。公共接口不限制 len，write() 按此值分块，
 * 因此固定提交缓冲不会无声缩小可接受长度。 */
#define RTK_BT_SPP_TX_CHUNK (512U)

/* 发送任务：静态栈与 TCB。栈单位与 IDF task.h 一致（字节）。 */
#define RTK_BT_SPP_WORKER_STACK    (4096U)
#define RTK_BT_SPP_WORKER_PRIORITY (5U)
/* 与 CONFIG_BT_BLUEDROID_PINNED_TO_CORE 相同（sdkconfig=0）：发送任务调用 esp_spp_write 后会
 * 阻塞在 BTC 队列上，同核可直接让高优先级的 BTC 任务接手，避免跨核唤醒延迟。 */
#define RTK_BT_SPP_WORKER_CORE (0)

/* 控制表：GAP 应答/无空槽拒绝只登记请求，由发送任务在任务上下文执行可能阻塞投递的 API。
 * 深度只需覆盖同时在途的认证请求；满了只能丢弃并记录（回调里不能等待）。 */
#define RTK_BT_SPP_CTRL_MAX      (4U)
#define RTK_BT_SPP_CTRL_RETRY_MS (100U)

/* 每槽至多 OPEN/DATA/RX_DROP/CLOSE 各一个；CLOSE 交付前禁止复用。
 * 上界来自接纳背压，不依赖应用持续 poll 或物理连接数限制重连次数。 */
_Static_assert(RTK_BT_SPP_EVENT_QUEUE_LEN >= (4U * RTK_BT_SPP_MAX_SESSIONS), "事件队列不足");

/* SPP 初始化/反初始化是异步的（回调只记录状态），这两个预算只用于等确认。 */
#define RTK_BT_SPP_INIT_TIMEOUT_MS   (5000U)
#define RTK_BT_SPP_UNINIT_TIMEOUT_MS (2000U)

/* 事件位：低 2 位为每会话唤醒位，其余为初始化阶段完成位。 */
#define RTK_BT_SPP_BIT_WAKE(idx)   ((EventBits_t)(1UL << (idx)))
#define RTK_BT_SPP_BIT_INIT_DONE   ((EventBits_t)(1UL << 8))
#define RTK_BT_SPP_BIT_START_DONE  ((EventBits_t)(1UL << 9))
#define RTK_BT_SPP_BIT_UNINIT_DONE ((EventBits_t)(1UL << 10))

_Static_assert(sizeof(TickType_t) == 4U, "rtk_bt_spp.c 假定 32 位 TickType_t");
_Static_assert(RTK_BT_SPP_TX_CHUNK <= UINT16_MAX, "tx_len 以 uint16_t 保存分块长度");
_Static_assert(RTK_BT_SPP_MAX_SESSIONS <= 8U, "唤醒位只有低 8 位可用");

typedef enum {
    RTK_BT_SPP_TX_IDLE = 0,  /* 无在途请求，可接受下一次提交 */
    RTK_BT_SPP_TX_RESERVED,  /* 写者已认领 tx_buf，正在复制数据 */
    RTK_BT_SPP_TX_QUEUED,    /* 写者已填好 tx_buf，等发送任务提交 */
    RTK_BT_SPP_TX_SUBMITTED, /* 发送任务已调用 esp_spp_write，等 WRITE_EVT */
} rtk_bt_spp_tx_state_t;

typedef struct {
    bool in_use;
    bool lifecycle_pending; /* OPEN 已发布，CLOSE 未交付：禁止复用 */
    bool data_pending;
    bool drop_pending;
    uint32_t rx_dropped;
    bool disconnect_pending;
    bool disconnect_submitted;
    bool disconnect_submitting;
    TickType_t disconnect_attempt;
    bool disconnect_attempted;
    bool reserving; /* 正在分配（已占位，字段尚未写完） */
    uint32_t handle;
    uint8_t addr[RTK_BT_SPP_ADDR_LEN];
    bool congested;
    uint8_t idx;
    /* 已登记到该槽的读者/写者数量；>0 时禁止复用槽，也禁止 xStreamBufferReset。 */
    uint32_t users;
    bool tx_submitting; /* 发送任务正处于 esp_spp_write 调用中 */
    rtk_bt_spp_tx_state_t tx_state;
    uint32_t tx_seq;      /* 每提交一次 +1，用于匹配完成事件 */
    uint32_t tx_done_seq; /* 最近一次完成的序号 */
    esp_err_t tx_result;  /* 最近一次完成的错误码 */
    uint16_t tx_len;      /* 本次提交长度（≤ RTK_BT_SPP_TX_CHUNK） */
    int tx_done_len;      /* WRITE_EVT 回报长度（诊断用） */
    uint8_t tx_buf[RTK_BT_SPP_TX_CHUNK];
    StreamBufferHandle_t rx;
    StaticStreamBuffer_t rx_struct;
    uint8_t rx_storage[RTK_BT_SPP_RX_BUF_SIZE];
} rtk_bt_spp_session_t;

typedef enum {
    RTK_BT_SPP_CTRL_PIN_REQ = 0, /* GAP PIN_REQ：回固定 PIN */
    RTK_BT_SPP_CTRL_CFM_REQ,     /* GAP CFM_REQ：自动确认 */
    RTK_BT_SPP_CTRL_REJECT,      /* SRV_OPEN 无安全空槽：断开该句柄 */
} rtk_bt_spp_ctrl_type_t;

typedef struct {
    bool in_use;
    bool submitting;
    bool closed;
    bool rearmed; /* 旧 CLOSE 后同句柄的新拒绝，不能由旧 SDK 返回值结算 */
    bool submitted;
    bool attempted;
    TickType_t attempt;
    rtk_bt_spp_ctrl_type_t type;
    uint32_t handle; /* 仅 REJECT 使用 */
    uint8_t bda[RTK_BT_SPP_ADDR_LEN];
} rtk_bt_spp_ctrl_t;

typedef enum {
    RTK_BT_SPP_STAGE_INIT = 0,
    RTK_BT_SPP_STAGE_START,
    RTK_BT_SPP_STAGE_UNINIT,
} rtk_bt_spp_stage_t;

typedef struct {
    QueueHandle_t evt_queue;
    EventGroupHandle_t evt_group; /* 拥塞/完成唤醒位 + 初始化阶段完成位 */
    TaskHandle_t worker;
} rtk_bt_spp_rtos_t;

/* 全部 RTOS 对象使用文件作用域静态存储，初始化后不分配堆。 */
static rtk_bt_spp_session_t s_sessions[RTK_BT_SPP_MAX_SESSIONS];
static portMUX_TYPE s_session_mux = portMUX_INITIALIZER_UNLOCKED;

static StaticQueue_t s_evt_queue_struct;
static uint8_t s_evt_queue_storage[RTK_BT_SPP_EVENT_QUEUE_LEN * sizeof(rtk_bt_spp_evt_t)];

static StaticEventGroup_t s_evt_group_struct;
static rtk_bt_spp_rtos_t s_rtos;

static rtk_bt_spp_ctrl_t s_ctrl[RTK_BT_SPP_CTRL_MAX];

static StackType_t s_worker_stack[RTK_BT_SPP_WORKER_STACK / sizeof(StackType_t)];
static StaticTask_t s_worker_tcb;

static char s_device_name[RTK_BT_SPP_NAME_MAX + 1U];
static uint32_t s_evt_dropped;
/* 回调写入、init 任务在等到对应事件位后读取。 */
static esp_spp_status_t s_init_status;
static esp_spp_status_t s_start_status;
static bool s_initialized;
/* 全驱动至多一个未完成的 IDF 提交：值为会话下标，-1 表示空闲。
 * 这样 handle=0/len=-1 的匿名提交错误只有唯一归属。
 * cancelled=true 表示该提交期间会话被 CLOSE 取消；要等 worker 从 esp_spp_write 返回
 * （静态 tx_buf 不再被 API 使用）后才释放 owner，避免取消后仍复用缓冲/误归属。 */
static int s_tx_inflight = -1;
static bool s_tx_inflight_cancelled;

/* 整次 write 的剩余预算；预算耗尽返回 false。认领、复制、排队与等待都复用它。 */
static bool rtk_bt_spp_budget_left(TickType_t start, uint32_t timeout_ms)
{
    return (xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms);
}

/* 等本会话状态变化；返回 false 表示本次等待后预算仍不够（由调用方判定超时）。 */
static bool rtk_bt_spp_wait_change(uint8_t idx, TickType_t start, uint32_t timeout_ms)
{
    TickType_t elapsed = 0U;
    TickType_t budget = 0U;

    if ((timeout_ms == 0U) || (s_rtos.evt_group == NULL)) {
        return false;
    }

    budget = pdMS_TO_TICKS(timeout_ms);
    elapsed = xTaskGetTickCount() - start;
    if (elapsed >= budget) {
        return false;
    }
    (void)xEventGroupWaitBits(s_rtos.evt_group, RTK_BT_SPP_BIT_WAKE(idx), pdFALSE, pdFALSE, budget - elapsed);
    return true;
}

/* 回调上下文使用：只查表，不登记 users（BTC 是 StreamBuffer 的唯一写者）。 */
static rtk_bt_spp_session_t *rtk_bt_spp_session_find(uint32_t handle)
{
    rtk_bt_spp_session_t *found = NULL;

    taskENTER_CRITICAL(&s_session_mux);
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if (s_sessions[idx].in_use && (s_sessions[idx].handle == handle)) {
            found = &s_sessions[idx];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);
    return found;
}

/* 登记一个读者/写者；返回 NULL 表示句柄已失效。登记后该槽不会被复用或 reset。 */
static rtk_bt_spp_session_t *rtk_bt_spp_session_acquire(uint32_t handle)
{
    rtk_bt_spp_session_t *found = NULL;

    taskENTER_CRITICAL(&s_session_mux);
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if (s_sessions[idx].in_use && (s_sessions[idx].handle == handle)) {
            s_sessions[idx].users++;
            found = &s_sessions[idx];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);
    return found;
}

/* 注销读者/写者。槽在 users 归零前不会被 SRV_OPEN 复用。 */
static void rtk_bt_spp_session_release(rtk_bt_spp_session_t *session)
{
    if (session == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_session_mux);
    if (session->users > 0U) {
        session->users--;
    }
    taskEXIT_CRITICAL(&s_session_mux);
}

/* 分配空闲槽：要求无读者、无在途发送（否则 reset 会与读/写并发或复用未退出的会话）。
 * BTC 回调（快路径）与发送任务（延后重试）都会调用，用 reserving 串行化。 */
static rtk_bt_spp_session_t *rtk_bt_spp_session_alloc(uint32_t handle, const uint8_t *addr)
{
    rtk_bt_spp_session_t *slot = NULL;
    uint32_t idx = 0U;

    taskENTER_CRITICAL(&s_session_mux);
    /* 拒绝请求已被 worker 取走时仍占表，防止 SDK 返回前接纳相同数字句柄。 */
    for (uint32_t ctrl = 0U; ctrl < RTK_BT_SPP_CTRL_MAX; ctrl++) {
        if (s_ctrl[ctrl].in_use && (s_ctrl[ctrl].type == RTK_BT_SPP_CTRL_REJECT) && (s_ctrl[ctrl].handle == handle)) {
            taskEXIT_CRITICAL(&s_session_mux);
            return NULL;
        }
    }
    for (idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if ((s_sessions[idx].lifecycle_pending || s_sessions[idx].disconnect_submitting) &&
            (s_sessions[idx].handle == handle)) {
            taskEXIT_CRITICAL(&s_session_mux);
            return NULL;
        }
    }
    for (idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        /* 该槽还有 IDF 在途提交（含 CLOSE 取消、等 worker 从 API 返回）时不可复用，
         * 避免静态 tx_buf/迟到事件污染新会话。 */
        if ((!s_sessions[idx].in_use) && (!s_sessions[idx].reserving) && (!s_sessions[idx].lifecycle_pending) &&
            (!s_sessions[idx].disconnect_submitting) && (s_sessions[idx].users == 0U) &&
            (!s_sessions[idx].tx_submitting) && (s_sessions[idx].tx_state == RTK_BT_SPP_TX_IDLE) &&
            (s_tx_inflight != (int)idx)) {
            s_sessions[idx].reserving = true;
            slot = &s_sessions[idx];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (slot == NULL) {
        return NULL;
    }
    if (slot->rx == NULL) {
        taskENTER_CRITICAL(&s_session_mux);
        slot->reserving = false;
        taskEXIT_CRITICAL(&s_session_mux);
        return NULL;
    }

    /* 此时 users==0 且无在途发送：reset 不会与接收交错，也不会破坏读者已取走的数据。 */
    (void)xStreamBufferReset(slot->rx);

    slot->handle = handle;
    for (uint32_t byte = 0U; byte < RTK_BT_SPP_ADDR_LEN; byte++) {
        slot->addr[byte] = addr[byte];
    }
    slot->congested = false;
    slot->tx_submitting = false;
    slot->tx_state = RTK_BT_SPP_TX_IDLE;
    slot->tx_seq = 0U;
    slot->tx_done_seq = 0U;
    slot->tx_result = ESP_OK;
    slot->tx_len = 0U;
    slot->tx_done_len = 0;
    slot->users = 0U;
    /* idx < RTK_BT_SPP_MAX_SESSIONS(=2)，uint8_t 窄化安全 */
    slot->idx = (uint8_t)idx;

    taskENTER_CRITICAL(&s_session_mux);
    slot->reserving = false;
    slot->lifecycle_pending = true;
    slot->data_pending = false;
    slot->drop_pending = false;
    slot->rx_dropped = 0U;
    slot->disconnect_pending = false;
    slot->disconnect_submitted = false;
    slot->disconnect_attempted = false;
    slot->in_use = true;
    taskEXIT_CRITICAL(&s_session_mux);
    return slot;
}

static bool rtk_bt_spp_session_free(uint32_t handle)
{
    bool found = false;
    bool release_owner = false;
    uint8_t idx = 0U;

    taskENTER_CRITICAL(&s_session_mux);
    for (idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if (s_sessions[idx].in_use && (s_sessions[idx].handle == handle)) {
            s_sessions[idx].in_use = false;
            s_sessions[idx].disconnect_pending = false;
            s_sessions[idx].congested = false;
            /* 让还在等的写者立刻拿到 INVALID_STATE。 */
            s_sessions[idx].tx_state = RTK_BT_SPP_TX_IDLE;
            s_sessions[idx].tx_result = ESP_ERR_INVALID_STATE;
            s_sessions[idx].tx_done_seq = s_sessions[idx].tx_seq;
            /* CLOSE 就是取消完成：IDF 在 CB 模式 CLOSE 时清空未完成 TX，不逐条合成 WRITE
             * （btc_spp.c:1188-1204 → spp_free_slot → :277-298 → :121-124）。
             * worker 已不在 esp_spp_write 内时立即释放 owner；仍在调用中则只标记取消，
             * 等该调用返回（静态 tx_buf 不再被使用）再由 worker 释放。 */
            if (s_tx_inflight == (int)idx) {
                if (s_sessions[idx].tx_submitting) {
                    s_tx_inflight_cancelled = true;
                } else {
                    s_tx_inflight = -1;
                    s_tx_inflight_cancelled = false;
                    release_owner = true;
                }
            }
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (found && (s_rtos.evt_group != NULL)) {
        (void)xEventGroupSetBits(s_rtos.evt_group, RTK_BT_SPP_BIT_WAKE(idx));
    }
    /* owner 释放后唤醒 worker，提交其他会话的排队请求（循环只按通知重新扫描）。 */
    if (release_owner && (s_rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_rtos.worker);
    }
    return found;
}

static void rtk_bt_spp_session_set_congested(uint32_t handle, bool congested)
{
    bool found = false;
    uint8_t idx = 0U;

    taskENTER_CRITICAL(&s_session_mux);
    for (idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if (s_sessions[idx].in_use && (s_sessions[idx].handle == handle)) {
            s_sessions[idx].congested = congested;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (found && (s_rtos.evt_group != NULL)) {
        (void)xEventGroupSetBits(s_rtos.evt_group, RTK_BT_SPP_BIT_WAKE(idx));
    }
}

/* 登记一条控制请求；回调上下文调用，只改静态表并在临界区外非阻塞唤醒发送任务。
 * 表满返回 false（回调不能等待，只能丢弃并记录）。 */
static bool rtk_bt_spp_ctrl_add(rtk_bt_spp_ctrl_type_t type, uint32_t handle, const uint8_t *bda)
{
    bool added = false;
    uint32_t free_slot = RTK_BT_SPP_CTRL_MAX;

    taskENTER_CRITICAL(&s_session_mux);
    for (uint32_t slot = 0U; slot < RTK_BT_SPP_CTRL_MAX; slot++) {
        if ((type == RTK_BT_SPP_CTRL_REJECT) && s_ctrl[slot].in_use && (s_ctrl[slot].type == type) &&
            (s_ctrl[slot].handle == handle)) {
            if (s_ctrl[slot].closed) {
                s_ctrl[slot].closed = false;
                s_ctrl[slot].rearmed = true;
            }
            added = true;
            break;
        }
        if (!s_ctrl[slot].in_use && (free_slot == RTK_BT_SPP_CTRL_MAX)) {
            free_slot = slot;
        }
    }
    if (!added && (free_slot < RTK_BT_SPP_CTRL_MAX)) {
        const uint32_t slot = free_slot;
        memset(&s_ctrl[slot], 0, sizeof(s_ctrl[slot]));
        s_ctrl[slot].type = type;
        s_ctrl[slot].handle = handle;
        for (uint32_t byte = 0U; byte < RTK_BT_SPP_ADDR_LEN; byte++) {
            s_ctrl[slot].bda[byte] = (bda != NULL) ? bda[byte] : 0U;
        }
        s_ctrl[slot].in_use = true;
        added = true;
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (added && (s_rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_rtos.worker);
    }
    return added;
}

/* 会话已关闭：撤销尚未执行的同句柄拒绝命令，避免稍后误断被复用的句柄。
 * 已由发送任务取走的命令无法撤销（窗口很小），见 ctrl_process 里的在册检查。 */
static void rtk_bt_spp_ctrl_drop_reject(uint32_t handle)
{
    taskENTER_CRITICAL(&s_session_mux);
    for (uint32_t slot = 0U; slot < RTK_BT_SPP_CTRL_MAX; slot++) {
        if (s_ctrl[slot].in_use && (s_ctrl[slot].type == RTK_BT_SPP_CTRL_REJECT) && (s_ctrl[slot].handle == handle)) {
            if (s_ctrl[slot].submitting) {
                s_ctrl[slot].closed = true;
            } else {
                s_ctrl[slot].in_use = false;
            }
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);
}

/* 发送任务上下文：执行控制表里的阻塞投递 API。 */
static void rtk_bt_spp_ctrl_process(void)
{
    for (uint32_t slot = 0U; slot < RTK_BT_SPP_CTRL_MAX; slot++) {
        bool has = false;
        rtk_bt_spp_ctrl_type_t type = RTK_BT_SPP_CTRL_PIN_REQ;
        uint32_t handle = 0U;
        uint8_t bda[RTK_BT_SPP_ADDR_LEN] = {0};
        esp_err_t err = ESP_OK;

        const TickType_t now = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_session_mux);
        has = s_ctrl[slot].in_use && !s_ctrl[slot].submitted &&
              (!s_ctrl[slot].attempted || ((now - s_ctrl[slot].attempt) >= pdMS_TO_TICKS(RTK_BT_SPP_CTRL_RETRY_MS)));
        if (has) {
            type = s_ctrl[slot].type;
            handle = s_ctrl[slot].handle;
            for (uint32_t byte = 0U; byte < RTK_BT_SPP_ADDR_LEN; byte++) {
                bda[byte] = s_ctrl[slot].bda[byte];
            }
            if (type == RTK_BT_SPP_CTRL_REJECT) {
                s_ctrl[slot].submitting = true;
                s_ctrl[slot].attempted = true;
                s_ctrl[slot].attempt = now;
            } else {
                s_ctrl[slot].in_use = false;
            }
        }
        taskEXIT_CRITICAL(&s_session_mux);

        if (!has) {
            continue;
        }

        switch (type) {
        case RTK_BT_SPP_CTRL_PIN_REQ: {
            esp_bt_pin_code_t pin_code = {0};

            /* 逐字节复制固定 PIN：字面量长度与 RTK_BT_SPP_PIN_LEN 同源，避免硬编码下标。
             * 字面量只含 ASCII 数字（0x30..0x39），缩窄到 uint8_t 安全。 */
            for (uint32_t pin_idx = 0U; pin_idx < RTK_BT_SPP_PIN_LEN; pin_idx++) {
                pin_code[pin_idx] = (uint8_t)RTK_BT_SPP_LEGACY_PIN[pin_idx];
            }

            err = esp_bt_gap_pin_reply(bda, true, RTK_BT_SPP_PIN_LEN, pin_code);
            break;
        }
        case RTK_BT_SPP_CTRL_CFM_REQ:
            err = esp_bt_gap_ssp_confirm_reply(bda, true);
            break;
        case RTK_BT_SPP_CTRL_REJECT:
            /* 取走后、断开前若同句柄已被新会话接纳（IDF 复用了句柄），放弃这次断开。 */
            if (rtk_bt_spp_session_find(handle) != NULL) {
                ESP_LOGW(RTK_BT_SPP_TAG, "reject handle=%" PRIu32 " skipped: session already active", handle);
                break;
            }
            err = esp_spp_disconnect(handle);
            if (err != ESP_OK) {
                ESP_LOGW(RTK_BT_SPP_TAG, "reject handle=%" PRIu32 ": %s", handle, esp_err_to_name(err));
            } else {
                ESP_LOGW(RTK_BT_SPP_TAG, "reject handle=%" PRIu32 " (no safe session slot)", handle);
            }
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }

        if (type == RTK_BT_SPP_CTRL_REJECT) {
            taskENTER_CRITICAL(&s_session_mux);
            s_ctrl[slot].submitting = false;
            if (s_ctrl[slot].rearmed) {
                s_ctrl[slot].submitted = false;
                s_ctrl[slot].attempted = false;
                s_ctrl[slot].rearmed = false;
            } else {
                s_ctrl[slot].submitted = (err == ESP_OK);
            }
            if (s_ctrl[slot].closed) {
                s_ctrl[slot].in_use = false;
            }
            taskEXIT_CRITICAL(&s_session_mux);
        }
        if (err != ESP_OK) {
            ESP_LOGW(RTK_BT_SPP_TAG, "ctrl type=%d: %s", (int)type, esp_err_to_name(err));
        }
    }
}

/* 回调单生产者：DATA/RX_DROP 合并，生命周期由槽接纳背压保证队列容量。 */
static void rtk_bt_spp_evt_post(rtk_bt_spp_evt_type_t type, uint32_t handle, const uint8_t *addr, uint32_t dropped)
{
    rtk_bt_spp_evt_t evt;

    if (s_rtos.evt_queue == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_session_mux);
    if ((type == RTK_BT_SPP_EVT_RX_DROP) || (type == RTK_BT_SPP_EVT_DATA)) {
        for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
            rtk_bt_spp_session_t *session = &s_sessions[idx];
            if (session->in_use && (session->handle == handle)) {
                bool *pending = (type == RTK_BT_SPP_EVT_RX_DROP) ? &session->drop_pending : &session->data_pending;
                if (type == RTK_BT_SPP_EVT_RX_DROP) {
                    /* 饱和累计，避免长时间不 poll 时回绕成小数；保留每个 handle 的归属。 */
                    session->rx_dropped =
                        (dropped > (UINT32_MAX - session->rx_dropped)) ? UINT32_MAX : (session->rx_dropped + dropped);
                }
                if (*pending) {
                    taskEXIT_CRITICAL(&s_session_mux);
                    return;
                }
                *pending = true;
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);

    evt.type = type;
    evt.handle = handle;
    evt.dropped = dropped;
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_ADDR_LEN; idx++) {
        evt.addr[idx] = (addr != NULL) ? addr[idx] : 0U;
    }

    if (xQueueSend(s_rtos.evt_queue, &evt, 0) != pdPASS) {
        taskENTER_CRITICAL(&s_session_mux);
        s_evt_dropped++;
        taskEXIT_CRITICAL(&s_session_mux);
        ESP_LOGW(RTK_BT_SPP_TAG, "event queue full, total dropped=%" PRIu32, s_evt_dropped);
    }
}

/* 逆序释放：Bluedroid 先于控制器。 */
static void rtk_bt_spp_unwind(bool bluedroid_enabled, bool bluedroid_inited, bool controller_enabled,
                              bool controller_inited)
{
    esp_err_t err = ESP_OK;

    if (bluedroid_enabled) {
        err = esp_bluedroid_disable();
        if (err != ESP_OK) {
            ESP_LOGW(RTK_BT_SPP_TAG, "bluedroid disable: %s", esp_err_to_name(err));
        }
    }
    if (bluedroid_inited) {
        err = esp_bluedroid_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(RTK_BT_SPP_TAG, "bluedroid deinit: %s", esp_err_to_name(err));
        }
    }
    if (controller_enabled) {
        err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGW(RTK_BT_SPP_TAG, "controller disable: %s", esp_err_to_name(err));
        }
    }
    if (controller_inited) {
        err = esp_bt_controller_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(RTK_BT_SPP_TAG, "controller deinit: %s", esp_err_to_name(err));
        }
    }
}

/* 只在 init 失败路径调用：此时还没有会话，且 SPP/Bluedroid 已关闭，不会再进回调。
 * 发送任务不删除——它只等任务通知，留着可被下一次 init 复用（静态对象不回收）。 */
static void rtk_bt_spp_objects_delete(void)
{
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        s_sessions[idx].in_use = false;
        s_sessions[idx].reserving = false;
        s_sessions[idx].users = 0U;
        s_sessions[idx].tx_submitting = false;
        s_sessions[idx].tx_state = RTK_BT_SPP_TX_IDLE;
        s_sessions[idx].handle = 0U;
        if (s_sessions[idx].rx != NULL) {
            vStreamBufferDelete(s_sessions[idx].rx);
            s_sessions[idx].rx = NULL;
        }
    }
    for (uint32_t slot = 0U; slot < RTK_BT_SPP_CTRL_MAX; slot++) {
        s_ctrl[slot].in_use = false;
        s_ctrl[slot].handle = 0U;
    }
    taskENTER_CRITICAL(&s_session_mux);
    s_tx_inflight = -1;
    s_tx_inflight_cancelled = false;
    taskEXIT_CRITICAL(&s_session_mux);
    if (s_rtos.evt_queue != NULL) {
        vQueueDelete(s_rtos.evt_queue);
        s_rtos.evt_queue = NULL;
    }
    if (s_rtos.evt_group != NULL) {
        vEventGroupDelete(s_rtos.evt_group);
        s_rtos.evt_group = NULL;
    }
    s_initialized = false;
}

static void rtk_bt_spp_worker(void *arg);

/* 创建静态 RTOS 对象与发送任务。发送任务跨 init 重试复用。 */
static esp_err_t rtk_bt_spp_objects_create(void)
{
    /* 重试时发送任务仍在运行：会话/控制表的复位放在同一把锁下，避免与它读到中间状态。 */
    taskENTER_CRITICAL(&s_session_mux);
    memset(s_sessions, 0, sizeof(s_sessions));
    memset(s_ctrl, 0, sizeof(s_ctrl));
    s_evt_dropped = 0U;
    s_tx_inflight = -1;
    s_tx_inflight_cancelled = false;
    taskEXIT_CRITICAL(&s_session_mux);

    s_rtos.evt_queue = xQueueCreateStatic(RTK_BT_SPP_EVENT_QUEUE_LEN, sizeof(rtk_bt_spp_evt_t), s_evt_queue_storage,
                                          &s_evt_queue_struct);
    s_rtos.evt_group = xEventGroupCreateStatic(&s_evt_group_struct);
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        /* idx < 2，uint8_t 窄化安全 */
        s_sessions[idx].idx = (uint8_t)idx;
        s_sessions[idx].rx = xStreamBufferCreateStatic(RTK_BT_SPP_RX_BUF_SIZE, RTK_BT_SPP_STREAM_TRIGGER,
                                                       s_sessions[idx].rx_storage, &s_sessions[idx].rx_struct);
    }
    if ((s_rtos.evt_queue == NULL) || (s_rtos.evt_group == NULL) || (s_sessions[0].rx == NULL) ||
        (s_sessions[1].rx == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    /* 只等任务通知/读会话表的常驻任务，栈 4096 字节（调用 esp_spp_write 与日志）。 */
    if (s_rtos.worker == NULL) {
        /* IDF 静态创建接口的栈单位是字节（task.h:519），因此直接给栈字节数。 */
        const uint32_t stack_bytes = (uint32_t)(sizeof(s_worker_stack) / sizeof(StackType_t));

        s_rtos.worker = xTaskCreateStaticPinnedToCore(rtk_bt_spp_worker, "rtk_bt_spp_worker", stack_bytes, NULL,
                                                      RTK_BT_SPP_WORKER_PRIORITY, s_worker_stack, &s_worker_tcb,
                                                      RTK_BT_SPP_WORKER_CORE);
        if (s_rtos.worker == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* 发送任务：取一个已排队的请求提交给 IDF。这是全工程唯一调用 esp_spp_write 的位置。 */
static void rtk_bt_spp_worker_submit(rtk_bt_spp_session_t *session)
{
    uint32_t seq = 0U;
    uint16_t len = 0U;
    uint32_t handle = 0U;
    uint8_t idx = 0U;
    esp_err_t err = ESP_OK;
    bool wake_worker = false;

    /* 取任务：在临界区内取走长度与序号，避免与写者复制 tx_buf 交错。
     * 全驱动只允许一个未完成提交：owner 已占用时本任务保持排队，等完成回调通知后再来。 */
    taskENTER_CRITICAL(&s_session_mux);
    if (session->in_use && (session->tx_state == RTK_BT_SPP_TX_QUEUED) && (s_tx_inflight < 0)) {
        len = session->tx_len;
        seq = session->tx_seq;
        handle = session->handle;
        idx = session->idx;
        session->tx_state = RTK_BT_SPP_TX_SUBMITTED;
        session->tx_submitting = true;
        /* idx < RTK_BT_SPP_MAX_SESSIONS(=2)，int 窄化安全 */
        s_tx_inflight = (int)idx;
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (len == 0U) {
        return;
    }

    /* esp_spp_write 先深拷贝参数，再经 btc_transfer_context 投递（可能阻塞在 BTC 队列上）。
     * 那次阻塞发生在本任务里，调用方只按自己的剩余预算等待完成事件，因此不会无限阻塞在
     * 调用方的上下文。len ≤ RTK_BT_SPP_TX_CHUNK，扩宽到 int 无损。 */
    err = esp_spp_write(handle, (int)len, session->tx_buf);

    taskENTER_CRITICAL(&s_session_mux);
    /* tx_submitting 一定要清（否则槽永远无法复用）；结果只写回仍是本次请求的会话，
     * 避免一次被阻塞很久的提交在会话关闭/复用后污染新会话。 */
    session->tx_submitting = false;
    if (err != ESP_OK) {
        /* 提交失败不会有 WRITE_EVT：释放 owner 并结案，让等待方立刻返回该错误。 */
        if (s_tx_inflight == (int)idx) {
            s_tx_inflight = -1;
            s_tx_inflight_cancelled = false;
            wake_worker = true;
        }
        if (session->in_use && (session->tx_state == RTK_BT_SPP_TX_SUBMITTED) && (session->tx_seq == seq)) {
            session->tx_result = err;
            session->tx_done_len = -1;
            session->tx_done_seq = seq;
            session->tx_state = RTK_BT_SPP_TX_IDLE;
        }
    } else if ((s_tx_inflight == (int)idx) && s_tx_inflight_cancelled) {
        /* CLOSE 在本调用期间取消了该提交。API 已返回（深拷贝/投递完成），静态 tx_buf 可再用，
         * 现在释放 owner；旧消息之后若入队失败只会报匿名 NO_CONNECTION，按取消事件忽略。 */
        s_tx_inflight = -1;
        s_tx_inflight_cancelled = false;
        wake_worker = true;
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "submit handle=%" PRIu32 " len=%u: %s", handle, (unsigned)len, esp_err_to_name(err));
    }
    if (s_rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_rtos.evt_group, RTK_BT_SPP_BIT_WAKE(idx));
    }
    /* 重新扫描一次，保证低于当前下标的排队请求也能被提交。 */
    if (wake_worker && (s_rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_rtos.worker);
    }
}

/* 普通断开有独立的每槽状态。失败每 100 ms 重试，成功只等真实 CLOSE；不伪造关闭。
 * 提交期间禁止槽复用，即使 CLOSE 与 SDK 阻塞调用交错也不会把返回结果写入新会话。 */
static void rtk_bt_spp_disconnect_process(void)
{
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        uint32_t handle = 0U;
        bool submit = false;
        const TickType_t now = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_session_mux);
        rtk_bt_spp_session_t *session = &s_sessions[idx];
        submit = session->in_use && session->disconnect_pending && !session->disconnect_submitted &&
                 (!session->disconnect_attempted ||
                  ((now - session->disconnect_attempt) >= pdMS_TO_TICKS(RTK_BT_SPP_CTRL_RETRY_MS)));
        if (submit) {
            handle = session->handle;
            session->disconnect_submitting = true;
            session->disconnect_attempted = true;
            session->disconnect_attempt = now;
        }
        taskEXIT_CRITICAL(&s_session_mux);
        if (!submit) {
            continue;
        }
        const esp_err_t err = esp_spp_disconnect(handle);
        taskENTER_CRITICAL(&s_session_mux);
        session->disconnect_submitting = false;
        session->disconnect_submitted = (err == ESP_OK);
        taskEXIT_CRITICAL(&s_session_mux);
        if (err != ESP_OK) {
            ESP_LOGW(RTK_BT_SPP_TAG, "disconnect handle=%" PRIu32 ": %s; retry pending", handle, esp_err_to_name(err));
        }
    }
}

/* 发送任务：串行提交各会话的排队请求，并执行控制表里的 GAP 应答/拒绝断开。
 * 绑核理由见 RTK_BT_SPP_WORKER_CORE。 */
static void rtk_bt_spp_worker(void *arg)
{
    (void)arg;

    for (;;) {
        for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
            bool queued = false;

            taskENTER_CRITICAL(&s_session_mux);
            queued = s_sessions[idx].in_use && (s_sessions[idx].tx_state == RTK_BT_SPP_TX_QUEUED);
            taskEXIT_CRITICAL(&s_session_mux);

            if (queued) {
                rtk_bt_spp_worker_submit(&s_sessions[idx]);
            }
        }
        rtk_bt_spp_ctrl_process();
        rtk_bt_spp_disconnect_process();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RTK_BT_SPP_CTRL_RETRY_MS));
    }
}

/* SPP 初始化结果：回调在 portMUX 下写状态、在临界区外置位，启动动作留给 init 任务。
 * 先写状态后置位，等待方拿到事件位后再在同一把锁下读取，因此迟到/早到两种顺序都拿到真实结果。 */
static void rtk_bt_spp_handle_init(esp_spp_cb_param_t *param)
{
    taskENTER_CRITICAL(&s_session_mux);
    s_init_status = param->init.status;
    taskEXIT_CRITICAL(&s_session_mux);

    if (param->init.status != ESP_SPP_SUCCESS) {
        ESP_LOGE(RTK_BT_SPP_TAG, "SPP init failed: %d", (int)param->init.status);
    }
    if (s_rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_rtos.evt_group, RTK_BT_SPP_BIT_INIT_DONE);
    }
}

static void rtk_bt_spp_handle_start(esp_spp_cb_param_t *param)
{
    taskENTER_CRITICAL(&s_session_mux);
    s_start_status = param->start.status;
    taskEXIT_CRITICAL(&s_session_mux);

    if (param->start.status == ESP_SPP_SUCCESS) {
        ESP_LOGI(RTK_BT_SPP_TAG, "server started, scn=%u", (unsigned)param->start.scn);
    } else {
        ESP_LOGE(RTK_BT_SPP_TAG, "server start failed: %d", (int)param->start.status);
    }
    if (s_rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_rtos.evt_group, RTK_BT_SPP_BIT_START_DONE);
    }
}

static void rtk_bt_spp_handle_srv_open(esp_spp_cb_param_t *param)
{
    const uint32_t handle = param->srv_open.handle;

    if (param->srv_open.status != ESP_SPP_SUCCESS) {
        ESP_LOGE(RTK_BT_SPP_TAG, "srv open failed: %d", (int)param->srv_open.status);
        return;
    }

    if (rtk_bt_spp_session_alloc(handle, param->srv_open.rem_bda) != NULL) {
        rtk_bt_spp_evt_post(RTK_BT_SPP_EVT_OPEN, handle, param->srv_open.rem_bda, 0U);
        return;
    }

    /* 没有 users==0 且无在途发送的安全空槽（旧读者/写者未退出、上一会话仍占用槽）：
     * 不在回调里调用 esp_spp_disconnect（btc_transfer_context 可能无限阻塞），登记拒绝命令，
     * 由发送任务断开。不做延后接纳，避免 CLOSE 后把已死的连接重新发布为会话。 */
    if (!rtk_bt_spp_ctrl_add(RTK_BT_SPP_CTRL_REJECT, handle, NULL)) {
        ESP_LOGE(RTK_BT_SPP_TAG, "no slot and ctrl table full, handle=%" PRIu32, handle);
    }
}

static void rtk_bt_spp_handle_close(esp_spp_cb_param_t *param)
{
    const uint32_t handle = param->close.handle;

    rtk_bt_spp_ctrl_drop_reject(handle);
    /* 未发布 OPEN 的拒绝连接没有应用生命周期，不能合成幽灵 CLOSE。 */
    if (rtk_bt_spp_session_free(handle)) {
        rtk_bt_spp_evt_post(RTK_BT_SPP_EVT_CLOSE, handle, NULL, 0U);
    }
}

static void rtk_bt_spp_handle_data(esp_spp_cb_param_t *param)
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

/* WRITE_EVT 结论：status 非成功、回报长度与提交长度不一致（短写）都按失败返回。
 * 成功路径的 len 是 RFCOMM 实际接受的字节数（bta_jv_act.c:2413-2420 → btc_spp.c:1136），
 * 因此 len != tx_len 不能当成功。只允许在临界区内调用：不访问 RTOS/日志。 */
static esp_err_t rtk_bt_spp_settle_write(rtk_bt_spp_session_t *session, esp_spp_status_t status, int done_len)
{
    const bool failed =
        (status != ESP_SPP_SUCCESS) || (done_len < 0) || ((uint32_t)done_len != (uint32_t)session->tx_len);
    const esp_err_t result = failed ? ESP_FAIL : ESP_OK;

    session->tx_done_len = done_len;
    session->tx_result = result;
    session->tx_done_seq = session->tx_seq;
    session->tx_state = RTK_BT_SPP_TX_IDLE;
    return result;
}

/* 写完成：把真实 status/len/cong 写回会话并唤醒等待方（QA-E-REVIEW 第 3 项）。 */
static void rtk_bt_spp_handle_write(esp_spp_cb_param_t *param)
{
    bool matched = false;
    bool wake_worker = false;
    bool ignore_stale = false;
    EventBits_t wake_bits = 0U;
    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_session_mux);
    for (uint32_t slot = 0U; slot < RTK_BT_SPP_MAX_SESSIONS; slot++) {
        if (!s_sessions[slot].in_use || (s_sessions[slot].handle != param->write.handle)) {
            continue;
        }
        if (s_sessions[slot].tx_state != RTK_BT_SPP_TX_SUBMITTED) {
            continue;
        }
        matched = true;
        result = rtk_bt_spp_settle_write(&s_sessions[slot], param->write.status, param->write.len);
        s_sessions[slot].congested = param->write.cong;
        wake_bits |= RTK_BT_SPP_BIT_WAKE(s_sessions[slot].idx);
        if (s_tx_inflight == (int)slot) {
            s_tx_inflight = -1;
            s_tx_inflight_cancelled = false;
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
        } else if (s_tx_inflight >= 0) {
            rtk_bt_spp_session_t *owner = &s_sessions[s_tx_inflight];

            result = ESP_FAIL;
            matched = true;
            if (owner->in_use && (owner->tx_state == RTK_BT_SPP_TX_SUBMITTED)) {
                (void)rtk_bt_spp_settle_write(owner, param->write.status, param->write.len);
                wake_bits |= RTK_BT_SPP_BIT_WAKE(owner->idx);
            }
            s_tx_inflight = -1;
            s_tx_inflight_cancelled = false;
            wake_worker = true;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (ignore_stale) {
        /* 旧请求在 CLOSE 后才被 BTC 处理：没有新会话的完成可结算，忽略。 */
        ESP_LOGD(RTK_BT_SPP_TAG, "stale anonymous write evt ignored: len=%d status=%d", param->write.len,
                 (int)param->write.status);
        return;
    }
    if (!matched) {
        /* 带有效句柄但不属于当前在途请求的迟到事件（成功或失败）：一律忽略，
         * 绝不能结算到别的会话。 */
        ESP_LOGW(RTK_BT_SPP_TAG, "write evt unmatched: handle=%" PRIu32 " len=%d status=%d", param->write.handle,
                 param->write.len, (int)param->write.status);
        return;
    }
    if (result != ESP_OK) {
        ESP_LOGW(RTK_BT_SPP_TAG, "write failed: handle=%" PRIu32 " len=%d status=%d", param->write.handle,
                 param->write.len, (int)param->write.status);
    }
    if (s_rtos.evt_group != NULL) {
        (void)xEventGroupSetBits(s_rtos.evt_group, wake_bits);
    }
    /* 完成回调可能来自 BTC：只做非阻塞任务通知，让发送任务提交其他会话的排队请求。 */
    if (wake_worker && (s_rtos.worker != NULL)) {
        (void)xTaskNotifyGive(s_rtos.worker);
    }
}

static void rtk_bt_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    switch (event) {
    case ESP_SPP_INIT_EVT:
        rtk_bt_spp_handle_init(param);
        break;

    case ESP_SPP_START_EVT:
        rtk_bt_spp_handle_start(param);
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        rtk_bt_spp_handle_srv_open(param);
        break;

    case ESP_SPP_CLOSE_EVT:
        rtk_bt_spp_handle_close(param);
        break;

    case ESP_SPP_DATA_IND_EVT:
        rtk_bt_spp_handle_data(param);
        break;

    case ESP_SPP_CONG_EVT:
        rtk_bt_spp_session_set_congested(param->cong.handle, param->cong.cong);
        break;

    case ESP_SPP_WRITE_EVT:
        rtk_bt_spp_handle_write(param);
        break;

    case ESP_SPP_UNINIT_EVT:
        ESP_LOGI(RTK_BT_SPP_TAG, "SPP uninitialized");
        if (s_rtos.evt_group != NULL) {
            (void)xEventGroupSetBits(s_rtos.evt_group, RTK_BT_SPP_BIT_UNINIT_DONE);
        }
        break;

    default:
        break;
    }
}

/* GAP 回调：只记录身份/认证结果；PIN/CFM 应答经 btc_transfer_context 投递（可能在 BTC 队列上
 * 无限等待），因此复制地址到静态控制表、非阻塞唤醒发送任务，在任务上下文里应答。 */
static void rtk_bt_spp_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(RTK_BT_SPP_TAG, "auth success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(RTK_BT_SPP_TAG, "auth failed: %d", (int)param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT:
        if (!rtk_bt_spp_ctrl_add(RTK_BT_SPP_CTRL_PIN_REQ, 0U, param->pin_req.bda)) {
            ESP_LOGE(RTK_BT_SPP_TAG, "pin req dropped: ctrl table full");
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        if (!rtk_bt_spp_ctrl_add(RTK_BT_SPP_CTRL_CFM_REQ, 0U, param->cfm_req.bda)) {
            ESP_LOGE(RTK_BT_SPP_TAG, "ssp confirm dropped: ctrl table full");
        }
        break;

    default:
        /* 其余事件（含 ESP_BT_GAP_MODE_CHG_EVT）无需处理。 */
        break;
    }
}

typedef struct {
    bool controller_inited;
    bool controller_enabled;
    bool bluedroid_inited;
    bool bluedroid_enabled;
} rtk_bt_spp_hw_state_t;

static void rtk_bt_spp_hw_state_clear(rtk_bt_spp_hw_state_t *state)
{
    state->controller_inited = false;
    state->controller_enabled = false;
    state->bluedroid_inited = false;
    state->bluedroid_enabled = false;
}

/* 控制器 + Bluedroid。失败时在内部完成逆序释放。 */
static esp_err_t rtk_bt_spp_hw_init(rtk_bt_spp_hw_state_t *state)
{
    esp_err_t err = ESP_OK;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();

    err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "mem release: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "controller init: %s", esp_err_to_name(err));
        return err;
    }
    state->controller_inited = true;

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "controller enable: %s", esp_err_to_name(err));
        rtk_bt_spp_unwind(state->bluedroid_enabled, state->bluedroid_inited, state->controller_enabled,
                          state->controller_inited);
        rtk_bt_spp_hw_state_clear(state);
        return err;
    }
    state->controller_enabled = true;

    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "bluedroid init: %s", esp_err_to_name(err));
        rtk_bt_spp_unwind(state->bluedroid_enabled, state->bluedroid_inited, state->controller_enabled,
                          state->controller_inited);
        rtk_bt_spp_hw_state_clear(state);
        return err;
    }
    state->bluedroid_inited = true;

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "bluedroid enable: %s", esp_err_to_name(err));
        rtk_bt_spp_unwind(state->bluedroid_enabled, state->bluedroid_inited, state->controller_enabled,
                          state->controller_inited);
        rtk_bt_spp_hw_state_clear(state);
        return err;
    }
    state->bluedroid_enabled = true;

    return ESP_OK;
}

/* GAP/SPP 回调、SSP 参数与 SPP 初始化。失败时由调用方统一 unwind。 */
static esp_err_t rtk_bt_spp_profile_init(void)
{
    esp_err_t err = ESP_OK;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_pin_code_t pin_code = {0};
    esp_spp_cfg_t spp_cfg = {0};

    err = esp_bt_gap_register_callback(rtk_bt_spp_gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "gap callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_spp_register_callback(rtk_bt_spp_cb);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "spp callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "set iocap: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "set pin: %s", esp_err_to_name(err));
        return err;
    }

    spp_cfg.mode = ESP_SPP_MODE_CB;
    spp_cfg.enable_l2cap_ertm = true;
    spp_cfg.tx_buffer_size = 0U;
    err = esp_spp_enhanced_init(&spp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "spp init: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static EventBits_t rtk_bt_spp_stage_bit(rtk_bt_spp_stage_t stage)
{
    switch (stage) {
    case RTK_BT_SPP_STAGE_INIT:
        return RTK_BT_SPP_BIT_INIT_DONE;
    case RTK_BT_SPP_STAGE_START:
        return RTK_BT_SPP_BIT_START_DONE;
    case RTK_BT_SPP_STAGE_UNINIT:
    default:
        return RTK_BT_SPP_BIT_UNINIT_DONE;
    }
}

/* 等一个异步阶段完成：完成位未到 → ESP_ERR_TIMEOUT；否则在同一把锁下读取该阶段的实际状态
 * （回调先写状态、再置位），因此事件早于/晚于等待返回都能得到真实结果，不能用调用前的快照。 */
static esp_err_t rtk_bt_spp_wait_stage(rtk_bt_spp_stage_t stage, uint32_t timeout_ms)
{
    const EventBits_t bit = rtk_bt_spp_stage_bit(stage);
    EventBits_t bits = 0U;
    esp_spp_status_t status = ESP_SPP_FAILURE;

    if (s_rtos.evt_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bits = xEventGroupWaitBits(s_rtos.evt_group, bit, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & bit) == 0U) {
        ESP_LOGE(RTK_BT_SPP_TAG, "stage 0x%" PRIx32 " timeout", (uint32_t)bit);
        return ESP_ERR_TIMEOUT;
    }
    if (stage == RTK_BT_SPP_STAGE_UNINIT) {
        return ESP_OK; /* UNINIT 回调没有业务状态 */
    }

    taskENTER_CRITICAL(&s_session_mux);
    status = (stage == RTK_BT_SPP_STAGE_INIT) ? s_init_status : s_start_status;
    taskEXIT_CRITICAL(&s_session_mux);
    return (status == ESP_SPP_SUCCESS) ? ESP_OK : ESP_FAIL;
}

/* 任务上下文完成启动阶段：等 INIT、启动服务端、等 START、设置设备名与可发现模式。
 * 这些 API 都可能阻塞在 btc_transfer_context 上，因此绝不能放到回调里（QA-E-REVIEW 第 2 项）。 */
static esp_err_t rtk_bt_spp_bringup(void)
{
    esp_err_t err = ESP_OK;

    err = rtk_bt_spp_wait_stage(RTK_BT_SPP_STAGE_INIT, RTK_BT_SPP_INIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* START 状态必须在发起前复位并清掉上次尝试的完成位：回调可能在 esp_spp_start_srv
     * 返回前（同步/极快）到达，不能在发起后再清。 */
    taskENTER_CRITICAL(&s_session_mux);
    s_start_status = ESP_SPP_FAILURE;
    taskEXIT_CRITICAL(&s_session_mux);
    (void)xEventGroupClearBits(s_rtos.evt_group, RTK_BT_SPP_BIT_START_DONE);

    err = esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, RTK_BT_SPP_SERVER_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "start srv: %s", esp_err_to_name(err));
        return err;
    }

    err = rtk_bt_spp_wait_stage(RTK_BT_SPP_STAGE_START, RTK_BT_SPP_INIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bt_gap_set_device_name(s_device_name);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "set name: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "set scan mode: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* SPP 反初始化同样异步：发起后等 UNINIT_EVT，超时只告警（没有回滚手段）。 */
static void rtk_bt_spp_spp_deinit(void)
{
    esp_err_t err = ESP_OK;

    /* 清掉上一次尝试可能残留的完成位，避免误以为本次已确认。 */
    if (s_rtos.evt_group != NULL) {
        (void)xEventGroupClearBits(s_rtos.evt_group, RTK_BT_SPP_BIT_UNINIT_DONE);
    }
    err = esp_spp_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(RTK_BT_SPP_TAG, "spp deinit: %s", esp_err_to_name(err));
        return;
    }
    if (rtk_bt_spp_wait_stage(RTK_BT_SPP_STAGE_UNINIT, RTK_BT_SPP_UNINIT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(RTK_BT_SPP_TAG, "spp deinit ack missing");
    }
}

esp_err_t rtk_bt_spp_init(const char *name)
{
    esp_err_t err = ESP_OK;
    rtk_bt_spp_hw_state_t hw_state = {false, false, false, false};
    size_t name_len = 0U;

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    name_len = strlen(name);
    if ((name_len == 0U) || (name_len > RTK_BT_SPP_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t idx = 0U; idx < name_len; idx++) {
        if ((name[idx] == '\n') || (name[idx] == '\r')) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    err = rtk_bt_spp_objects_create();
    if (err != ESP_OK) {
        ESP_LOGE(RTK_BT_SPP_TAG, "objects create: %s", esp_err_to_name(err));
        rtk_bt_spp_objects_delete();
        return err;
    }

    for (size_t idx = 0U; idx < name_len; idx++) {
        s_device_name[idx] = name[idx];
    }
    s_device_name[name_len] = '\0';
    /* 阶段结果初值：回调未到按失败处理；同时清掉上次尝试可能残留的完成位，
     * 避免 INIT 等待被旧事件立即满足。清位在 profile_init 之前，同步回调也不会丢。 */
    taskENTER_CRITICAL(&s_session_mux);
    s_init_status = ESP_SPP_FAILURE;
    s_start_status = ESP_SPP_FAILURE;
    taskEXIT_CRITICAL(&s_session_mux);
    if (s_rtos.evt_group != NULL) {
        (void)xEventGroupClearBits(s_rtos.evt_group, RTK_BT_SPP_BIT_INIT_DONE | RTK_BT_SPP_BIT_START_DONE);
    }

    err = rtk_bt_spp_hw_init(&hw_state);
    if (err != ESP_OK) {
        rtk_bt_spp_objects_delete();
        return err;
    }

    err = rtk_bt_spp_profile_init();
    if (err != ESP_OK) {
        rtk_bt_spp_unwind(hw_state.bluedroid_enabled, hw_state.bluedroid_inited, hw_state.controller_enabled,
                          hw_state.controller_inited);
        rtk_bt_spp_objects_delete();
        return err;
    }

    err = rtk_bt_spp_bringup();
    if (err != ESP_OK) {
        /* 逆序清理：SPP → Bluedroid → 控制器 → RTOS 对象。 */
        rtk_bt_spp_spp_deinit();
        rtk_bt_spp_unwind(hw_state.bluedroid_enabled, hw_state.bluedroid_inited, hw_state.controller_enabled,
                          hw_state.controller_inited);
        rtk_bt_spp_objects_delete();
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

bool rtk_bt_spp_poll_event(rtk_bt_spp_evt_t *evt, uint32_t timeout_ms)
{
    if ((evt == NULL) || (!s_initialized) || (s_rtos.evt_queue == NULL)) {
        return false;
    }

    if (xQueueReceive(s_rtos.evt_queue, evt, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    taskENTER_CRITICAL(&s_session_mux);
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if (s_sessions[idx].lifecycle_pending && (s_sessions[idx].handle == evt->handle)) {
            if (evt->type == RTK_BT_SPP_EVT_CLOSE) {
                s_sessions[idx].lifecycle_pending = false;
            } else if (evt->type == RTK_BT_SPP_EVT_DATA) {
                s_sessions[idx].data_pending = false;
            } else if (evt->type == RTK_BT_SPP_EVT_RX_DROP) {
                evt->dropped = s_sessions[idx].rx_dropped;
                s_sessions[idx].rx_dropped = 0U;
                s_sessions[idx].drop_pending = false;
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);
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

/* 提交一个分块并等它的真实完成。start/timeout_ms 由调用方在整次 write 内共享。 */
static esp_err_t rtk_bt_spp_write_chunk(rtk_bt_spp_session_t *session, const uint8_t *data, size_t len,
                                        TickType_t start, uint32_t timeout_ms)
{
    uint32_t seq = 0U;

    if ((s_rtos.worker == NULL) || (s_rtos.evt_group == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 0) 认领/复制/排队与等待共用整次 write 的预算：预算已在块间耗尽时不得再提交下一块。 */
    if (!rtk_bt_spp_budget_left(start, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    /* 1) 原子地认领发送槽：同一会话一次只有一个在途请求，连续 write 不会堆进 IDF 的 10 深 TX
     *    队列；拥塞时按剩余预算等 ESP_SPP_CONG_EVT 解除。认领成功后本调用独占 tx_buf，
     *    并发写者只能等它交还给发送任务。 */
    for (;;) {
        bool alive = false;
        bool claimed = false;

        (void)xEventGroupClearBits(s_rtos.evt_group, RTK_BT_SPP_BIT_WAKE(session->idx));
        taskENTER_CRITICAL(&s_session_mux);
        alive = session->in_use;
        if (alive && (session->tx_state == RTK_BT_SPP_TX_IDLE) && (!session->tx_submitting) && (!session->congested)) {
            session->tx_state = RTK_BT_SPP_TX_RESERVED;
            claimed = true;
        }
        taskEXIT_CRITICAL(&s_session_mux);

        if (claimed) {
            break;
        }
        if (!alive) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!rtk_bt_spp_wait_change(session->idx, start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
    }

    /* 复制前再核对预算；已过预算则把认领交还 IDLE，不能留下永久 RESERVED 的槽。 */
    if (!rtk_bt_spp_budget_left(start, timeout_ms)) {
        taskENTER_CRITICAL(&s_session_mux);
        if (session->in_use && (session->tx_state == RTK_BT_SPP_TX_RESERVED)) {
            session->tx_state = RTK_BT_SPP_TX_IDLE;
        }
        taskEXIT_CRITICAL(&s_session_mux);
        return ESP_ERR_TIMEOUT;
    }

    /* 2) 复制到静态提交缓冲：已认领 RESERVED，其它写者进不来，发送任务也不会取走它。 */
    for (size_t idx = 0U; idx < len; idx++) {
        session->tx_buf[idx] = data[idx];
    }

    taskENTER_CRITICAL(&s_session_mux);
    /* 复制期间会话可能被关闭：此时槽已交还给 IDLE，不得再排入请求。 */
    if (session->in_use) {
        /* len ≤ RTK_BT_SPP_TX_CHUNK(512)，uint16_t 窄化安全 */
        session->tx_len = (uint16_t)len;
        session->tx_seq++;
        seq = session->tx_seq;
        session->tx_state = RTK_BT_SPP_TX_QUEUED;
    }
    taskEXIT_CRITICAL(&s_session_mux);

    if (seq == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xTaskNotifyGive(s_rtos.worker);

    /* 3) 等真实 WRITE_EVT：status/len/cong 由回调写回，超时按剩余预算收敛。 */
    for (;;) {
        bool alive = false;
        bool done = false;
        esp_err_t result = ESP_OK;

        (void)xEventGroupClearBits(s_rtos.evt_group, RTK_BT_SPP_BIT_WAKE(session->idx));
        taskENTER_CRITICAL(&s_session_mux);
        alive = session->in_use;
        done = (session->tx_done_seq == seq);
        result = session->tx_result;
        taskEXIT_CRITICAL(&s_session_mux);

        if (done) {
            return result;
        }
        if (!alive) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!rtk_bt_spp_wait_change(session->idx, start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t rtk_bt_spp_write(uint32_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    rtk_bt_spp_session_t *session = NULL;
    TickType_t start = 0U;
    size_t offset = 0U;
    esp_err_t err = ESP_OK;

    if ((data == NULL) || (len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_rtos.worker == NULL) || (!s_initialized)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 合法参数之后先确认会话：未知句柄必须返回 NOT_FOUND，不能被 0 预算的 TIMEOUT 抢先。
     * 发送是异步的，0 预算无法确认任何完成，对已知会话按超时返回且不提交。 */
    start = xTaskGetTickCount();
    session = rtk_bt_spp_session_acquire(handle);
    if (session == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (timeout_ms == 0U) {
        rtk_bt_spp_session_release(session);
        return ESP_ERR_TIMEOUT;
    }

    /* 分块：每块 ≤ RTK_BT_SPP_TX_CHUNK，整次调用共享同一份 timeout 预算。 */
    while (offset < len) {
        size_t chunk = len - offset;

        if (!rtk_bt_spp_budget_left(start, timeout_ms)) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (chunk > (size_t)RTK_BT_SPP_TX_CHUNK) {
            chunk = (size_t)RTK_BT_SPP_TX_CHUNK;
        }
        err = rtk_bt_spp_write_chunk(session, data + offset, chunk, start, timeout_ms);
        if (err != ESP_OK) {
            break;
        }
        offset += chunk;
    }

    rtk_bt_spp_session_release(session);
    return err;
}

esp_err_t rtk_bt_spp_disconnect(uint32_t handle)
{
    bool found = false;

    /* 只登记；独立于 GAP/拒绝控制表，重复请求幂等且不会挤掉拒绝命令。 */
    taskENTER_CRITICAL(&s_session_mux);
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_MAX_SESSIONS; idx++) {
        if (s_sessions[idx].in_use && (s_sessions[idx].handle == handle)) {
            s_sessions[idx].disconnect_pending = true;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_session_mux);
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)xTaskNotifyGive(s_rtos.worker);
    return ESP_OK;
}

esp_err_t rtk_bt_spp_get_local_addr(uint8_t addr[RTK_BT_SPP_ADDR_LEN])
{
    const uint8_t *local = NULL;

    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    local = esp_bt_dev_get_address();
    if (local == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t idx = 0U; idx < RTK_BT_SPP_ADDR_LEN; idx++) {
        addr[idx] = local[idx];
    }
    return ESP_OK;
}

uint32_t rtk_bt_spp_dropped_events(void)
{
    uint32_t dropped = 0U;

    taskENTER_CRITICAL(&s_session_mux);
    dropped = s_evt_dropped;
    taskEXIT_CRITICAL(&s_session_mux);
    return dropped;
}
