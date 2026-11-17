/* spp_server 驱动的内部共享声明：私有类型、跨文件共享状态 s_spp、跨文件内部函数原型。
 * 本头只给 spp_server_*.c 用，不属于公共接口，组件外不得包含。
 *
 * 线程模型（对应 docs/reports/QA-E-REVIEW.md 第 1–4 项）：
 * - BTC 回调（spp_server_cb / spp_server_gap_cb）：只拷贝数据、记录完成状态、置事件位、非阻塞投递；
 *   不调用任何可能阻塞投递的 API。esp_spp_write / esp_spp_disconnect / esp_spp_start_srv 都经
 *   btc_transfer_context → btc_task_post(OSI_THREAD_MAX_TIMEOUT)（IDF btc_task.c:366），
 *   后者阻塞在 xQueueSend(portMAX_DELAY)，在回调里调用会把 BTC 任务自己锁死。
 * - 发送任务是唯一 SDK write/disconnect 调用者；最多4个在途块，跨会话轮转。
 *   API 前绑定描述符，API 返回前由 users 固定源区。匿名失败只请求候选会话断开。
 *   该任务还处理静态控制表：GAP 的 PIN/CFM 应答与“无安全空槽”的断开请求都从任务上下文执行
 *   （这些 API 经 btc_transfer_context 投递，回调里调用会阻塞 BTC 任务）。
 * - 链路任务（poll_event/read/write/disconnect 的调用者）：单一所有者；read/write 期间登记
 *   session->users，槽在读者/写者退出前不会被新连接复用，也不会被 reset。
 * - SRV_OPEN 只在 BTC 回调里分配一次槽：找不到 users==0 且无在途发送的安全空槽时，登记拒绝
 *   命令并由发送任务断开该连接；不做延后接纳（避免 CLOSE 后重现幽灵会话）。
 * 会话表、发送状态、控制表由 s_spp_mux 临界区保护；临界区内不调用 FreeRTOS API、不阻塞。
 */
#pragma once

#include "sdkconfig.h"

#if !CONFIG_SOC_BT_CLASSIC_SUPPORTED
#error "spp_server 只支持 SOC_BT_CLASSIC_SUPPORTED 目标（经典蓝牙 SPP）"
#endif

/* 事件位与无符号 tick 差值比较都要求 32 位 EventBits_t / TickType_t。 */
#if configUSE_16_BIT_TICKS
#error "spp_server 需要 32 位 tick（configUSE_16_BIT_TICKS 必须为 0）"
#endif

/* C11 的 static_assert 由此头定义；clangd 把私有 .h 视为 C++ 时也须保留。 */
#include <assert.h> // IWYU pragma: keep
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

#include "SppServer.h"
#include "SppServerStorage.h"

#define SPP_SERVER_TAG "spp_server"

/* 同步兼容 write 的分块预算；worker 的聚合块另为1024B。 */
#define SPP_SERVER_TX_CHUNK (512U)
/* IDF v6.1 components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c:37，升级需复核。 */
#define SPP_SERVER_SDK_QUEUE_SIZE    (10U)
#define SPP_SERVER_TX_PAYLOAD_BUDGET (9216U)
static_assert(SPP_SERVER_INFLIGHT_MAX <= 4U, "全局/每会话至多4个在途");
static_assert(SPP_SERVER_INFLIGHT_MAX < SPP_SERVER_SDK_QUEUE_SIZE, "不得填满 SDK slot 队列");
static_assert(SPP_SERVER_REPLY_CAPACITY * SPP_SERVER_MAX_SESSIONS + SPP_SERVER_STREAM_CAPACITY +
                      SPP_SERVER_MERGE_CAPACITY ==
                  SPP_SERVER_TX_PAYLOAD_BUDGET,
              "TX payload 预算");

/* 发送任务：静态栈字节数（与 SppServerStorage.h 一致）；栈存储由组合根经 bind_storage 注入。 */
#define SPP_SERVER_WORKER_STACK SPP_SERVER_WORKER_STACK_BYTES

/* 控制表：GAP 应答/无空槽拒绝只登记请求，由发送任务在任务上下文执行可能阻塞投递的 API。
 * 深度只需覆盖同时在途的认证请求；满了只能丢弃并记录（回调里不能等待）。 */
#define SPP_SERVER_CTRL_MAX      (4U)
#define SPP_SERVER_CTRL_RETRY_MS (100U)

/* OPEN/CLOSE/DATA/RX_DROP/TX_SPACE 五类通知；队列满时 TX level 仍由 poll_event 重查。 */
#define SPP_SERVER_NOTICES_PER_SESSION (5U)
static_assert(SPP_SERVER_EVENT_QUEUE_LEN >=
                  (SPP_SERVER_NOTICES_PER_SESSION * SPP_SERVER_MAX_SESSIONS),
              "事件队列不足");

/* 事件位：低 2 位为每会话唤醒位，其余为初始化阶段完成位。 */
#define SPP_SERVER_BIT_WAKE(idx)   ((EventBits_t)(1UL << (idx)))
#define SPP_SERVER_BIT_INIT_DONE   ((EventBits_t)(1UL << 8))
#define SPP_SERVER_BIT_START_DONE  ((EventBits_t)(1UL << 9))
#define SPP_SERVER_BIT_UNINIT_DONE ((EventBits_t)(1UL << 10))

static_assert(sizeof(TickType_t) == 4U, "spp_server 假定 32 位 TickType_t");
static_assert(SPP_SERVER_TX_CHUNK <= UINT16_MAX, "tx_len 以 uint16_t 保存分块长度");
static_assert(SPP_SERVER_MAX_SESSIONS <= 8U, "唤醒位只有低 8 位可用");

typedef struct {
    uint16_t head;
    uint16_t used;
    uint16_t issued;
    uint16_t reserved;
    uint32_t serial;
    bool copying;
} spp_server_ring_t;

typedef struct {
    bool active;
    bool submitting;
    bool done;
    uint8_t slot;
    uint8_t channel; /* 0 reply、1 stream、2 push */
    uint16_t len;
    uint32_t generation;
    uint32_t handle;
    uint32_t order;
    esp_err_t result;
    TickType_t tick;
} spp_server_inflight_t;

typedef struct {
    bool in_use;
    bool lifecycle_pending; /* OPEN 已发布，CLOSE 未交付：禁止复用 */
    bool data_pending;
    bool drop_pending;
    bool rx_data_missed;
    uint32_t rx_dropped;
    uint32_t rx_dropped_total;
    uint32_t rx_high_water;
    uint32_t rx_events;
    uint32_t drop_events;
    bool disconnect_pending;
    bool disconnect_submitted;
    bool disconnect_submitting;
    TickType_t disconnect_attempt;
    bool disconnect_attempted;
    bool reserving; /* 正在分配（已占位，字段尚未写完） */
    uint32_t handle;
    uint8_t addr[SPP_SERVER_ADDR_LEN];
    bool congested;
    uint8_t idx;
    /* 已登记到该槽的读者/写者数量；>0 时禁止复用槽，也禁止 xStreamBufferReset。 */
    uint32_t users;
    uint32_t generation;
    spp_server_ring_t reply;
    uint8_t push[SPP_SERVER_PUSH_CAPACITY];
    uint16_t push_len;
    bool tx_space; /* level 改变通知，poll_event 原子取走 */
    uint32_t tx_total;
    uint32_t tx_done;
    uint32_t tx_errors;
    esp_err_t tx_result;
    StreamBufferHandle_t rx;
    StaticStreamBuffer_t rx_struct;
} spp_server_session_t;

typedef enum {
    SPP_SERVER_CTRL_PIN_REQ = 0, /* GAP PIN_REQ：回固定 PIN */
    SPP_SERVER_CTRL_CFM_REQ,     /* GAP CFM_REQ：自动确认 */
    SPP_SERVER_CTRL_REJECT,      /* SRV_OPEN 无安全空槽：断开该句柄 */
} spp_server_ctrl_type_t;

/* CTRL_ERR 追踪直接把 ctrl_type 作为 spp_server_trace_ctrl_t 上报，两者取值必须一致。 */
_Static_assert((int)SPP_SERVER_CTRL_PIN_REQ == (int)SPP_SERVER_TRACE_CTRL_PIN,
               "ctrl/trace mapping");
_Static_assert((int)SPP_SERVER_CTRL_CFM_REQ == (int)SPP_SERVER_TRACE_CTRL_CONFIRM,
               "ctrl/trace mapping");
_Static_assert((int)SPP_SERVER_CTRL_REJECT == (int)SPP_SERVER_TRACE_CTRL_REJECT,
               "ctrl/trace mapping");

typedef struct {
    bool in_use;
    bool submitting;
    bool closed;
    bool rearmed; /* 旧 CLOSE 后同句柄的新拒绝，不能由旧 SDK 返回值结算 */
    bool submitted;
    bool attempted;
    TickType_t attempt;
    spp_server_ctrl_type_t type;
    uint32_t handle; /* 仅 REJECT 使用 */
    uint8_t bda[SPP_SERVER_ADDR_LEN];
} spp_server_ctrl_t;

typedef enum {
    SPP_SERVER_STAGE_INIT = 0,
    SPP_SERVER_STAGE_START,
    SPP_SERVER_STAGE_UNINIT,
} spp_server_stage_t;

typedef struct {
    QueueHandle_t evt_queue;
    EventGroupHandle_t evt_group; /* 拥塞/完成唤醒位 + 初始化阶段完成位 */
    TaskHandle_t worker;
} spp_server_rtos_t;

/* 全部跨文件共享的可变状态（ADR-0008 R5）：唯一实例 s_spp，唯一定义点在 SppServerSession.c。
 * 7 块大负载缓冲由组合根通过 spp_server_bind_storage() 外供注入，控制块留在 .bss。 */
typedef struct {
    spp_server_session_t sessions[SPP_SERVER_MAX_SESSIONS];
    StaticQueue_t evt_queue_struct;
    uint8_t evt_queue_storage[SPP_SERVER_EVENT_QUEUE_LEN * sizeof(spp_server_evt_t)];
    StaticEventGroup_t evt_group_struct;
    spp_server_rtos_t rtos;
    spp_server_ctrl_t ctrl[SPP_SERVER_CTRL_MAX];
    StaticTask_t worker_tcb;
    spp_server_storage_t storage;
    bool storage_bound;
    bool boot_stop_requested;
    bool boot_worker_stopped;
    char device_name[SPP_SERVER_NAME_MAX + 1U];
    uint32_t evt_dropped;
    /* 回调写入、init 任务在等到对应事件位后读取。 */
    esp_spp_status_t init_status;
    esp_spp_status_t start_status;
    bool initialized;
    spp_server_inflight_t inflight[SPP_SERVER_INFLIGHT_MAX];
    spp_server_ring_t stream;
    uint8_t stream_ends[SPP_SERVER_STREAM_CAPACITY / 8U]; /* 每字节一个句末位，无记录数隐藏上限 */
    uint8_t stream_owner; /* slot+1；0 为无 owner，避免非零初值搬到 .data */
    bool stream_unbind;
    bool stream_active;
    uint8_t tx_cursor;
    uint32_t tx_order;
    spp_server_tx_stats_t tx_stats;
    /* 回调无阻塞唤醒 poll_event，level 另由每槽 tx_space 保存。 */
    bool tx_wake_pending;
    uint32_t rx_wake_calls;
} spp_server_state_t;

extern spp_server_state_t s_spp;

/* 保护所有驱动状态；临界区内不调用 RTOS/SDK。 */
extern portMUX_TYPE s_spp_mux;

/* ---- 追踪钩子（ADR-0009；SppServer.h 冻结接口 spp_server_set_trace_hook 的存储） ---- */

/* 钩子函数指针：写侧在任务上下文安装，读侧在 BTC 回调/发送任务/链路任务里读取。
 * 用 s_spp_mux 临界区保护而不用 _Atomic：临界区只做一次指针读取（不阻塞、不调 FreeRTOS API），
 * 开销与原子读相当，而 C11 _Atomic 在本工程的两套主机前端（clang C11 / clangd 的 C++ 回退）上
 * 诊断不一致。所有调用点都必须在临界区之外（本文件的调用点已满足），否则 portENTER_CRITICAL
 * 在同一核上不可重入。 */
extern spp_server_trace_fn s_spp_trace;

/* 非空才调用：先在本临界区取出函数指针，出临界区后再调用，避免钩子实现里再进临界区。 */
static inline void spp_server_trace(spp_server_trace_code_t code, uint32_t handle, uint32_t a,
                                    uint32_t b)
{
#if defined(CONFIG_RTK_TRACE_ENABLE)
    spp_server_trace_fn fn = NULL;

    taskENTER_CRITICAL(&s_spp_mux);
    fn = s_spp_trace;
    taskEXIT_CRITICAL(&s_spp_mux);

    if (fn != NULL) {
        fn(code, handle, a, b);
    }
#else
    /* CONFIG_RTK_TRACE_ENABLE=n（ADR-0009 §2.6）：不进临界区、不调用钩子，调用点被优化掉。 */
    (void)code;
    (void)handle;
    (void)a;
    (void)b;
#endif
}

/* ---- 跨文件内部函数（每个注明了允许的调用上下文） ---- */

/* SppServerSession.c：会话槽与控制表。
 * find/alloc/free/set_congested/ctrl_add/ctrl_drop_reject 允许 BTC 回调线程调用（只改静态表、非阻塞）；
 * acquire/release 由链路任务在读/写期间调用；ctrl_process 只在发送任务上下文调用。 */
spp_server_session_t *spp_server_session_find(uint32_t handle);
spp_server_session_t *spp_server_session_acquire(uint32_t handle);
void spp_server_session_release(spp_server_session_t *session);
spp_server_session_t *spp_server_session_alloc(uint32_t handle, const uint8_t *addr);
bool spp_server_session_free(uint32_t handle);
void spp_server_session_set_congested(uint32_t handle, bool congested);
bool spp_server_ctrl_add(spp_server_ctrl_type_t type, uint32_t handle, const uint8_t *bda);
void spp_server_ctrl_drop_reject(uint32_t handle);
void spp_server_ctrl_process(void);

/* SppServerRx.c：BTC 回调线程的接收与事件投递。 */
void spp_server_evt_post(spp_server_evt_type_t type, uint32_t handle, const uint8_t *addr,
                         uint32_t dropped);
void spp_server_handle_data(esp_spp_cb_param_t *param);
void spp_server_notify_rx_wake(void);

/* SppServerTx.c：发送任务的入口与 WRITE_EVT 结算（后者在 BTC 回调线程）。 */
void spp_server_worker(void *arg);
void spp_server_handle_write(esp_spp_cb_param_t *param);
/* 以下 locked 仅在 mux 内调用；reclaim 在 users 归零后解绑唯一 stream。 */
void spp_server_tx_close_locked(spp_server_session_t *session);
void spp_server_tx_reclaim_locked(spp_server_session_t *session);
/* 任意任务/BTC、非 ISR；锁外合并投递 TX_SPACE 并唤醒 worker。 */
void spp_server_tx_wake(void);

/* SppServerGap.c：两个 Bluedroid 回调的注册入口（BTC 回调线程调用）。 */
void spp_server_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
void spp_server_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
