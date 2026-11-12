/* spp_server 驱动的内部共享声明：私有类型、跨文件共享状态 s_spp、跨文件内部函数原型。
 * 本头只给 spp_server_*.c 用，不属于公共接口，组件外不得包含。
 *
 * 线程模型（对应 docs/reports/QA-E-REVIEW.md 第 1–4 项）：
 * - BTC 回调（spp_server_cb / spp_server_gap_cb）：只拷贝数据、记录完成状态、置事件位、非阻塞投递；
 *   不调用任何可能阻塞投递的 API。esp_spp_write / esp_spp_disconnect / esp_spp_start_srv 都经
 *   btc_transfer_context → btc_task_post(OSI_THREAD_MAX_TIMEOUT)（IDF btc_task.c:366），
 *   后者阻塞在 xQueueSend(portMAX_DELAY)，在回调里调用会把 BTC 任务自己锁死。
 * - 发送任务 spp_server_worker（静态创建、绑核 0）：进程内唯一调用 esp_spp_write 的位置，
 *   每次只提交一个分块并等待该分块的真实 WRITE_EVT，因此相邻两次 spp_server_write 不会把
 *   请求堆进 IDF 每会话深度 10 的 TX 队列；提交动作在任务里做，调用方只会阻塞在有界等待上。
 *   全驱动同一时刻至多一个未完成的 IDF 提交（s_spp_tx_inflight），因此 handle=0/len=-1 的匿名
 *   提交错误可以唯一归属；完成事件会非阻塞唤醒该任务继续提交其他会话的排队请求。
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

#include "spp_server.h"

#define SPP_SERVER_TAG "spp_server"

/* 单次交给 esp_spp_write 的最大字节数。公共接口不限制 len，write() 按此值分块，
 * 因此固定提交缓冲不会无声缩小可接受长度。 */
#define SPP_SERVER_TX_CHUNK (512U)

/* 发送任务：静态栈与 TCB。栈单位与 IDF task.h 一致（字节）。 */
#define SPP_SERVER_WORKER_STACK (4096U)

/* 控制表：GAP 应答/无空槽拒绝只登记请求，由发送任务在任务上下文执行可能阻塞投递的 API。
 * 深度只需覆盖同时在途的认证请求；满了只能丢弃并记录（回调里不能等待）。 */
#define SPP_SERVER_CTRL_MAX      (4U)
#define SPP_SERVER_CTRL_RETRY_MS (100U)

/* 每槽至多 OPEN/DATA/RX_DROP/CLOSE 各一个；CLOSE 交付前禁止复用。
 * 上界来自接纳背压，不依赖应用持续 poll 或物理连接数限制重连次数。 */
_Static_assert(SPP_SERVER_EVENT_QUEUE_LEN >= (4U * SPP_SERVER_MAX_SESSIONS), "事件队列不足");

/* 事件位：低 2 位为每会话唤醒位，其余为初始化阶段完成位。 */
#define SPP_SERVER_BIT_WAKE(idx)   ((EventBits_t)(1UL << (idx)))
#define SPP_SERVER_BIT_INIT_DONE   ((EventBits_t)(1UL << 8))
#define SPP_SERVER_BIT_START_DONE  ((EventBits_t)(1UL << 9))
#define SPP_SERVER_BIT_UNINIT_DONE ((EventBits_t)(1UL << 10))

_Static_assert(sizeof(TickType_t) == 4U, "spp_server 假定 32 位 TickType_t");
_Static_assert(SPP_SERVER_TX_CHUNK <= UINT16_MAX, "tx_len 以 uint16_t 保存分块长度");
_Static_assert(SPP_SERVER_MAX_SESSIONS <= 8U, "唤醒位只有低 8 位可用");

typedef enum {
    SPP_SERVER_TX_IDLE = 0,  /* 无在途请求，可接受下一次提交 */
    SPP_SERVER_TX_RESERVED,  /* 写者已认领 tx_buf，正在复制数据 */
    SPP_SERVER_TX_QUEUED,    /* 写者已填好 tx_buf，等发送任务提交 */
    SPP_SERVER_TX_SUBMITTED, /* 发送任务已调用 esp_spp_write，等 WRITE_EVT */
} spp_server_tx_state_t;

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
    uint8_t addr[SPP_SERVER_ADDR_LEN];
    bool congested;
    uint8_t idx;
    /* 已登记到该槽的读者/写者数量；>0 时禁止复用槽，也禁止 xStreamBufferReset。 */
    uint32_t users;
    bool tx_submitting; /* 发送任务正处于 esp_spp_write 调用中 */
    spp_server_tx_state_t tx_state;
    uint32_t tx_seq;      /* 每提交一次 +1，用于匹配完成事件 */
    uint32_t tx_done_seq; /* 最近一次完成的序号 */
    esp_err_t tx_result;  /* 最近一次完成的错误码 */
    uint16_t tx_len;      /* 本次提交长度（≤ SPP_SERVER_TX_CHUNK） */
    int tx_done_len;      /* WRITE_EVT 回报长度（诊断用） */
    uint8_t tx_buf[SPP_SERVER_TX_CHUNK];
    StreamBufferHandle_t rx;
    StaticStreamBuffer_t rx_struct;
    uint8_t rx_storage[SPP_SERVER_RX_BUF_SIZE];
} spp_server_session_t;

typedef enum {
    SPP_SERVER_CTRL_PIN_REQ = 0, /* GAP PIN_REQ：回固定 PIN */
    SPP_SERVER_CTRL_CFM_REQ,     /* GAP CFM_REQ：自动确认 */
    SPP_SERVER_CTRL_REJECT,      /* SRV_OPEN 无安全空槽：断开该句柄 */
} spp_server_ctrl_type_t;

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

/* 全部跨文件共享的可变状态（ADR-0008 R5）：唯一实例 s_spp，唯一定义点在 spp_server_session.c。
 * 所有 RTOS 对象都指向本结构里的静态存储，初始化后不分配堆。 */
typedef struct {
    spp_server_session_t sessions[SPP_SERVER_MAX_SESSIONS];
    StaticQueue_t evt_queue_struct;
    uint8_t evt_queue_storage[SPP_SERVER_EVENT_QUEUE_LEN * sizeof(spp_server_evt_t)];
    StaticEventGroup_t evt_group_struct;
    spp_server_rtos_t rtos;
    spp_server_ctrl_t ctrl[SPP_SERVER_CTRL_MAX];
    StackType_t worker_stack[SPP_SERVER_WORKER_STACK / sizeof(StackType_t)];
    StaticTask_t worker_tcb;
    char device_name[SPP_SERVER_NAME_MAX + 1U];
    uint32_t evt_dropped;
    /* 回调写入、init 任务在等到对应事件位后读取。 */
    esp_spp_status_t init_status;
    esp_spp_status_t start_status;
    bool initialized;
    bool tx_inflight_cancelled; /* 当前在途提交期间会话被 CLOSE 取消，含义见 s_spp_tx_inflight */
} spp_server_state_t;

extern spp_server_state_t s_spp;

/* 以下两项初值非零，单独定义：放进 s_spp 会让整个 14 KB 结构落入 .data，镜像多一份初值拷贝。 */
/* 保护 s_spp 的可变字段与 s_spp_tx_inflight；不可重入，临界区内不调用 FreeRTOS API。
 * IDF 的“未加锁”值不是全零（spinlock.h SPINLOCK_FREE），必须用初始化宏。 */
extern portMUX_TYPE s_spp_mux;
/* 全驱动至多一个未完成的 IDF 提交：值为会话下标，-1 表示空闲。
 * 这样 handle=0/len=-1 的匿名提交错误只有唯一归属。
 * s_spp.tx_inflight_cancelled=true 表示该提交期间会话被 CLOSE 取消；要等 worker 从 esp_spp_write 返回
 * （静态 tx_buf 不再被 API 使用）后才释放 owner，避免取消后仍复用缓冲/误归属。 */
extern int s_spp_tx_inflight;

/* ---- 跨文件内部函数（每个注明了允许的调用上下文） ---- */

/* spp_server_session.c：会话槽与控制表。
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

/* spp_server_rx.c：BTC 回调线程的接收与事件投递。 */
void spp_server_evt_post(spp_server_evt_type_t type, uint32_t handle, const uint8_t *addr, uint32_t dropped);
void spp_server_handle_data(esp_spp_cb_param_t *param);

/* spp_server_tx.c：发送任务的入口与 WRITE_EVT 结算（后者在 BTC 回调线程）。 */
void spp_server_worker(void *arg);
void spp_server_handle_write(esp_spp_cb_param_t *param);

/* spp_server_gap.c：两个 Bluedroid 回调的注册入口（BTC 回调线程调用）。 */
void spp_server_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
void spp_server_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
