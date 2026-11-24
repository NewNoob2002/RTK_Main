/* spp_server 驱动的内部共享声明：私有类型、唯一状态实例 s_spp、跨文件内部函数原型；只给本组件 .c 用。
 * 线程：BTC 回调只拷贝/置状态并非阻塞投递（SDK 写/断开经 btc_transfer_context，回调里调用会锁死 BTC 任务）；发送任务是唯一 SDK write/disconnect 调用者；链路任务独占 poll/read/write。
 * 堆与阻塞：状态与负载缓冲全部静态（缓冲由组合根外供）；s_spp_mux 临界区内不调用 FreeRTOS API、不阻塞。 */
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
#include "SppServerPairing.h"

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

/* 发送任务：静态栈字节数（与 SppServerStorage.h 一致）；栈存储由组合根经 bindStorage 注入。 */
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
    SPP_SERVER_CTRL_PIN_REQ = 0, /* GAP PIN_REQ */
    SPP_SERVER_CTRL_CFM_REQ,     /* GAP CFM_REQ */
    SPP_SERVER_CTRL_REJECT,      /* SRV_OPEN 无安全空槽：断开该句柄 */
    SPP_SERVER_CTRL_PAIRED,      /* GAP 认证成功，交给 worker 记录地址 */
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
    uint32_t seq; /* 单调递增到达序号，供 worker 按到达顺序取项 */
    spp_server_ctrl_type_t type;
    uint32_t handle; /* REJECT：会话句柄；CFM_REQ：数字比较码（000000..999999） */
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

/* 全部跨文件共享的可变状态（ADR-0008 §8）：唯一实例 s_spp，唯一定义点在 SppServerSession.c。
 * 7 块大负载缓冲由组合根通过 spp_server_bind_storage() 外供注入，控制块留在 .bss。 */
typedef struct {
    spp_server_session_t sessions[SPP_SERVER_MAX_SESSIONS];
    StaticQueue_t evt_queue_struct;
    uint8_t evt_queue_storage[SPP_SERVER_EVENT_QUEUE_LEN * sizeof(spp_server_evt_t)];
    StaticEventGroup_t evt_group_struct;
    spp_server_rtos_t rtos;
    spp_server_ctrl_t ctrl[SPP_SERVER_CTRL_MAX];
    uint32_t ctrl_seq; /* 控制表单调递增到达序号计数（允许无符号回绕） */
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

/* 钩子函数指针：写侧在任务上下文安装，读侧在 BTC 回调/发送任务/链路任务里读取。用 s_spp_mux 临界区
 * 保护而不用 _Atomic：临界区只做一次指针读取（不阻塞、不调 FreeRTOS API），开销与原子读相当，而 C11
 * _Atomic 在本工程两套主机前端（clang C11 / clangd 的 C++ 回退）上诊断不一致。调用点都必须在临界区外。 */
extern spp_server_trace_fn s_spp_trace;

/**
 * @brief  追踪上报：取出钩子后在本文件之外调用，钩子为空时直接返回
 * @param  code: 追踪类别（SPP_SERVER_TRACE_*）
 * @param  handle: 相关蓝牙句柄或会话下标，无则 0
 * @param  a: 第一个载荷，含义由 code 决定
 * @param  b: 第二个载荷，含义由 code 决定
 * @note  任意任务/BTC 回调、非 ISR；锁内只拷贝函数指针，调用在锁外（钩子实现里可以再进临界区）；
 *        CONFIG_RTK_TRACE_ENABLE=n 时整个函数被编译成空操作
 */
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

/* SppServerSession.c：会话槽与静态控制表。
 * 线程：find/alloc/free/set_congested/ctrl_add/ctrl_drop_reject 由 BTC 回调线程调用；acquire/release 由链路任务
 *       与发送任务调用；ctrl_process 只在发送任务上下文执行。
 * 锁：以上函数只在 s_spp_mux 临界区内改静态表，临界区内不调用 FreeRTOS/SDK。 */

/**
 * @brief  按句柄查会话槽（不登记使用者）
 * @param  handle: 蓝牙 SPP 句柄
 * @retval 命中的会话槽指针；没有则 NULL
 * @note  BTC 回调线程（BTC 是接收流的唯一写者）；返回的槽随时可能被释放，需要稳定引用请改用 acquire
 */
spp_server_session_t *spp_server_session_find(uint32_t handle);

/**
 * @brief  取会话并登记一个使用者，保证返回期间该槽不被复用
 * @param  handle: 蓝牙 SPP 句柄
 * @retval 已登记的会话槽；句柄未知或正被分配时 NULL
 * @note  链路任务/发送任务；锁内登记，登记后该槽不会被复用或 reset，users 归零前 SRV_OPEN 也复用不了；
 *        每次成功都要配对调用 release
 */
spp_server_session_t *spp_server_session_acquire(uint32_t handle);

/**
 * @brief  注销一个使用者，users 归零时回收该会话的发送状态
 * @param  session: 已登记使用者持有的会话；NULL 直接返回
 * @note  任意任务上下文；锁外唤醒发送任务，不阻塞
 */
void spp_server_session_release(spp_server_session_t *session);

/**
 * @brief  为新连接分配会话槽并记录对端地址
 * @param  handle: 蓝牙 SPP 句柄
 * @param  addr: 对端地址（6 字节）；NULL 表示槽内地址保持全零
 * @retval 新槽；该句柄已有在途会话或待投递 REJECT、无空闲槽时 NULL
 * @note  BTC 回调线程（唯一调用者是 SRV_OPEN 处理）；要求该槽无读者、无在途发送，用 reserving
 *        串行化；两段临界区之间复位该槽的接收流缓冲，期间其它函数看到 reserving
 */
spp_server_session_t *spp_server_session_alloc(uint32_t handle, const uint8_t *addr);

/**
 * @brief  按句柄释放会话槽
 * @param  handle: 蓝牙 SPP 句柄
 * @retval true 找到并释放；false 该句柄当前没有会话
 * @note  BTC 回调线程；锁外唤醒发送任务，不阻塞
 */
bool spp_server_session_free(uint32_t handle);

/**
 * @brief  记录 SDK 上报的链路拥塞状态并唤醒发送任务
 * @param  handle: 蓝牙 SPP 句柄
 * @param  congested: true 进入拥塞，false 解除
 * @note  BTC 回调线程；状态没有真实变化时只唤醒不计入统计，不阻塞
 */
void spp_server_session_set_congested(uint32_t handle, bool congested);

/**
 * @brief  把一条控制命令放入静态控制表并唤醒发送任务
 * @param  type: 命令类型（REJECT/PIN_REQ/CFM_REQ）
 * @param  handle: REJECT 为目标句柄；CFM_REQ 为数字比较码；其余忽略
 * @param  bda: 对端地址（6 字节），REJECT 可传 NULL
 * @retval true 已入队或同句柄 REJECT 已在表中；false 表已满（回调上下文不能等待，调用方只能丢弃并记统计）
 * @note  BTC/GAP 回调线程；只改静态表，锁外非阻塞唤醒发送任务；同句柄 REJECT 幂等，
 *        已被取走并关闭的 REJECT 会重挂（rearmed）
 */
bool spp_server_ctrl_add(spp_server_ctrl_type_t type, uint32_t handle, const uint8_t *bda);

/**
 * @brief  撤销某句柄待发送的 REJECT 命令
 * @param  handle: 蓝牙 SPP 句柄
 * @note  BTC 回调线程（CLOSE 事件）；发送任务已取走并正在投递的命令置 closed，不再下发；撤销窗口
 *        很小，ctrl_process 里还按会话在册状态复查
 */
void spp_server_ctrl_drop_reject(uint32_t handle);

/**
 * @brief  发送任务侧的控制命令处理：按到达顺序取出、按类型下发并结算
 * @note  只在发送任务上下文调用，每轮至多处理 SPP_SERVER_CTRL_MAX 条；PIN/CFM 应答与 REJECT 断开都在这里执行
 */
void spp_server_ctrl_process(void);

// ---- SppServerRx.c：接收流与事件投递，全部在 BTC 回调线程执行。

/**
 * @brief  投递一条接收事件（打开/关闭/数据/丢弃）
 * @param  type: 事件类型
 * @param  handle: 蓝牙 SPP 句柄
 * @param  addr: 对端地址（6 字节），仅 OPEN 事件使用
 * @param  dropped: 随事件交给读者的丢弃字节数（DATA 丢失时为丢掉的字节）
 * @note  单生产者（BTC 回调），队列操作无需加锁；不阻塞：队列满时本次事件被丢弃并回滚 pending，
 *        DATA/RX_DROP 的缺失由 poll_event 的 level 状态兜底补发；OPEN/CLOSE 之后唤醒读侧
 */
void spp_server_evt_post(spp_server_evt_type_t type, uint32_t handle, const uint8_t *addr,
                         uint32_t dropped);

/**
 * @brief  DATA_IND 处理：把负载拷进接收流并投递事件
 * @param  param: SDK 回调参数，用 data_ind.handle/data/len
 * @note  BTC 回调线程；只复制不解析；流缓冲剩余空间不足时整包丢弃并累计 dropped
 */
void spp_server_handle_data(esp_spp_cb_param_t *param);

/**
 * @brief  唤醒读侧（已注册的 rx 唤醒钩子 + poll_event 使用者）
 * @note  BTC 回调线程/任意任务、非 ISR；锁内只取标志与钩子，调用在锁外
 */
void spp_server_notify_rx_wake(void);

// ---- SppServerTx.c：发送任务主体与 WRITE_EVT 结算（后者在 BTC 回调线程）。

/**
 * @brief  发送任务主体：处理控制命令、选块、合并、唯一调用 SDK 写出、结算在途块
 * @param  arg: 未使用
 * @note  常驻任务（优先级 5、绑核 0、栈由组合根外供）；任务内不持锁调用 esp_spp_write/disconnect，
 *        退出条件由 stop_boot 置位后唤醒
 */
void spp_server_worker(void *arg);

/**
 * @brief  WRITE_EVT 处理：结算 SDK 写出的块并推进队列
 * @param  param: SDK 回调参数，用 write.handle/write.status
 * @note  BTC 回调线程；只改静态状态，不阻塞，不调用 SDK 写接口、不解析/打印数据内容。
 *        SDK 回调不带请求号，只匹配同 handle 最早待完成块，generation 防止本地槽复用串味；
 *        handle==0 且 len==-1 的失败回调按匿名失败处理（会话整代置断开）
 */
void spp_server_handle_write(esp_spp_cb_param_t *param);

/**
 * @brief  会话关闭时的发送状态清理（需持 s_spp_mux）
 * @param  session: 会话
 * @note  临界区内调用；把在途块标记取消并归还环形空间，不调用 FreeRTOS/SDK
 */
void spp_server_tx_close_locked(spp_server_session_t *session);

/**
 * @brief  users 归零后回收会话的发送状态与 stream 占位（需持 s_spp_mux）
 * @param  session: 会话
 * @note  临界区内调用；解绑该会话占用的唯一 stream 环，不调用 FreeRTOS/SDK
 */
void spp_server_tx_reclaim_locked(spp_server_session_t *session);

/**
 * @brief  合并投递 TX_SPACE 位并唤醒发送任务
 * @note  任意任务/BTC 回调、非 ISR；锁内只置位去重，唤醒在锁外，不阻塞
 */
void spp_server_tx_wake(void);

// ---- SppServerTxRing.c：环形空间、预留令牌与提交。reserve 走同步写路径，*_locked 只在 mux 内调用。

/**
 * @brief  在指定通道上预留一段发送窗口（同步写路径直接调用；异步路径用 reply/stream_reserve）
 * @param  handle: 蓝牙 SPP 句柄
 * @param  len: 要预留的字节数，1..SPP_SERVER_STREAM_CAPACITY
 * @param  channel: SPP_SERVER_CHANNEL_* 通道编号
 * @param[out] token: 成功时写入句柄/通道/代数/序号/容量；失败时不被修改
 * @retval ESP_OK 预留成功，环形空间已记账，必须 commit 或 cancel
 * @retval ESP_ERR_INVALID_ARG token 为 NULL，或 len 为 0/超过 4096
 * @retval ESP_ERR_INVALID_STATE 驱动未初始化；或 stream 通道归其它会话/已被解绑
 * @retval ESP_ERR_NOT_FOUND 该句柄当前没有会话
 * @retval ESP_ERR_TIMEOUT 会话断开中、正在提交、已有预留或环内空间不足（零接纳，计入 paused）
 * @note  任意任务、非 ISR；只在锁内记账，不复制负载
 */
esp_err_t spp_server_reserve(uint32_t handle, size_t len, uint8_t channel,
                             spp_server_tx_token_t *token);

/**
 * @brief  按句柄查会话槽（需持 s_spp_mux）
 * @param  handle: 蓝牙 SPP 句柄
 * @retval 命中的会话槽；没有则 NULL
 */
spp_server_session_t *spp_server_find_locked(uint32_t handle);

/**
 * @brief  取会话最老的在途块（需持 s_spp_mux）
 * @param  slot: 会话槽位下标
 * @param  pending_only: 只统计尚未完成的块
 * @retval 最老的块；没有则 NULL
 */
spp_server_inflight_t *spp_server_oldest_locked(uint8_t slot, bool pending_only);

/**
 * @brief  发送任务一轮：取控制命令以外的待发数据，合并后在锁外写出
 * @retval true 本轮提交了数据；false 没有可发数据
 * @note  只在发送任务上下文调用；锁内选块，esp_spp_write 在锁外；merge_buf 不会被第二个 API 复用，
 *        回调早于返回也保留 submitting/users，未归零前槽不会被复用
 */
bool spp_server_worker_submit(void);

/**
 * @brief  结算会话已完成的在途块（需持 s_spp_mux）
 * @param  session: 会话
 */
void spp_server_retire_locked(spp_server_session_t *session);

/* 在途发送的通道编号（spp_server_inflight_t.channel）：应答/流/推送的择一顺序即这里的值。 */
#define SPP_SERVER_CHANNEL_REPLY  (0U)
#define SPP_SERVER_CHANNEL_STREAM (1U)
#define SPP_SERVER_CHANNEL_PUSH   (2U)

typedef struct {
    bool controller_inited;
    bool controller_enabled;
    bool bluedroid_inited;
    bool bluedroid_enabled;
} spp_server_hw_state_t;

/**
 * @brief  逆序释放仍处于已使能/已初始化状态的蓝牙栈阶段，Bluedroid 先于控制器
 * @param  bluedroid_enabled: Bluedroid 已使能时先 esp_bluedroid_disable
 * @param  bluedroid_inited: Bluedroid 已初始化时再 esp_bluedroid_deinit
 * @param  controller_enabled: 控制器已使能时 esp_bt_controller_disable
 * @param  controller_inited: 控制器已初始化时再 esp_bt_controller_deinit
 * @note  启动/停止路径（调用方任务）调用；某一步失败只记日志并继续下一步，不阻塞等待
 */
void spp_server_unwind(bool bluedroid_enabled, bool bluedroid_inited, bool controller_enabled,
                       bool controller_inited);

/**
 * @brief  把蓝牙栈各阶段完成标志全部清零
 * @param[out] state: 要清零的状态；所有字段写成 false
 * @note  只在启动/停止路径读写该结构，无并发访问
 */
void spp_server_hw_state_clear(spp_server_hw_state_t *state);

/**
 * @brief  初始化并使能控制器与 Bluedroid
 * @param[out] state: 阶段完成标志；每一步成功后置位，失败时内部逆序释放并清零
 * @retval ESP_OK 控制器与 Bluedroid 均已初始化并使能
 * @retval 其他 IDF 错误码 该步失败（此前成功的阶段已释放，state 已清零）
 * @note  只能在 spp_server_init() 的启动路径调用；会经 btc_transfer_context 阻塞等待，耗时可到秒级
 */
esp_err_t spp_server_hw_init(spp_server_hw_state_t *state);

/**
 * @brief  注册 GAP/SPP 回调、设置 SSP 参数并发起 SPP 初始化
 * @retval ESP_OK 回调已注册且 SPP 初始化请求已提交
 * @retval 其他 IDF 错误码 注册/配置失败，由调用方统一 unwind
 * @note  只能在 spp_server_init() 的启动路径调用；初始化完成由 INIT_EVT 回调置位，不在此等待
 */
esp_err_t spp_server_profile_init(void);

/**
 * @brief  SPP 事件入口（注册给 esp_spp_register_callback）
 * @param  event: 事件类型，未处理的事件直接忽略
 * @param  param: 事件参数；NULL 时直接返回
 * @note  BTC 回调线程；只拷贝数据、改状态、置事件位、非阻塞投递，绝不调用 SDK 写/断开接口
 */
void spp_server_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

/**
 * @brief  GAP 事件入口（注册给 esp_bt_gap_register_callback）
 * @param  event: 事件类型，只处理 AUTH_CMPL/PIN_REQ/CFM_REQ
 * @param  param: 事件参数；NULL 时直接返回
 * @note  BTC 回调线程；PIN/CFM 应答复制地址后交给发送任务执行，认证结果只记追踪
 */
void spp_server_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
