// ESP-IDF Bluedroid 经典蓝牙 SPP 多会话服务端，C 实现（仅 SOC_BT_CLASSIC_SUPPORTED 目标）。
// 线程模型：
// - Bluedroid GAP/SPP 回调（BTC 任务）中只做：把数据拷进该会话的静态 StreamBuffer、更新拥塞标志、
//   向静态事件队列非阻塞投递。不解析协议、不阻塞、不打印数据内容。
// - poll_event/read/write/disconnect 只允许同一个链路任务调用（单一所有者）。
// IDF v6.1 已核实（reference/c-v1/docs/reports/T04.md）：esp_spp_write 深拷贝；SRV_OPEN 后服务端继续监听，
// 最多 ESP_SPP_MAX_SESSION(=2) 个会话；对端地址在 srv_open.rem_bda。
// 冻结接口（ADR-0003）。实现方不得修改。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SPP_SERVER_MAX_SESSIONS (2U)
/* 每会话接收 StreamBuffer（ADR-0006）：手机经 NTRIP 转发 RTCM 时一次突发可达数 KB，由 Kconfig 配置；
 * 非 IDF 构建（host 检查）没有 sdkconfig，回退到原值 2048。 */
#if defined(CONFIG_RTK_BT_SPP_RX_BUF_SIZE)
#define SPP_SERVER_RX_BUF_SIZE ((size_t)CONFIG_RTK_BT_SPP_RX_BUF_SIZE)
#else
#define SPP_SERVER_RX_BUF_SIZE (2048U)
#endif
#define SPP_SERVER_EVENT_QUEUE_LEN (16U)
#define SPP_SERVER_NAME_MAX        (31U) /* 设备名最大字节数（不含结尾 0） */
#define SPP_SERVER_ADDR_LEN        (6U)
#define SPP_SERVER_LEGACY_PIN      "1234" /* 旧设备 PIN_REQ 应答，与 5.4 注释一致 */

typedef enum {
    SPP_SERVER_EVT_OPEN = 0, /* 新会话建立，addr 有效 */
    SPP_SERVER_EVT_CLOSE,    /* 会话关闭 */
    SPP_SERVER_EVT_DATA,     /* 该会话 StreamBuffer 有新数据 */
    SPP_SERVER_EVT_RX_DROP,  /* 回调中缓冲满，丢弃 dropped 字节 */
    SPP_SERVER_EVT_TX_SPACE, /* TX level 有变化；调用者必须重新查询容量，通知可以合并 */
} spp_server_evt_type_t;

typedef struct {
    spp_server_evt_type_t type;
    uint32_t handle;
    uint8_t addr[SPP_SERVER_ADDR_LEN];
    uint32_t dropped;
} spp_server_evt_t;

/**
 * 初始化控制器（BR/EDR only，释放 BLE 内存）、Bluedroid、GAP（SSP，IO 能力 NONE，CFM_REQ 自动确认，
 * PIN_REQ 回 SPP_SERVER_LEGACY_PIN）、SPP（ESP_SPP_MODE_CB）；在 SPP INIT 事件中启动服务端
 * （ESP_SPP_SEC_NONE，与 5.4 一致）、设置设备名、可连接+可发现。
 * name 为 NULL/空/超过 SPP_SERVER_NAME_MAX/含 '\n' 或 '\r' → ESP_ERR_INVALID_ARG；重复调用 → ESP_ERR_INVALID_STATE；
 * 其他步骤失败：按逆序释放已初始化部分，返回该步骤的错误码。
 */
esp_err_t spp_server_init(const char *name);

/** 等待事件，timeout_ms=0 不等待。有事件返回 true；未初始化或 evt 为 NULL 返回 false。 */
bool spp_server_poll_event(spp_server_evt_t *evt, uint32_t timeout_ms);

/** 从会话 StreamBuffer 非阻塞读取，返回字节数；未知句柄或参数非法返回 0。 */
size_t spp_server_read(uint32_t handle, uint8_t *buf, size_t size);

/** 同步兼容发送：等真实 WRITE_EVT；全次共享 timeout_ms，超时不撤回已提交字节。
 * 只允许链路任务、非 ISR。未知句柄返回 NOT_FOUND；0 预算不提交，返回 TIMEOUT。
 */
esp_err_t spp_server_write(uint32_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms);

#define SPP_SERVER_REPLY_CAPACITY  (2048U)
#define SPP_SERVER_REPLY_RESERVE   (280U)
#define SPP_SERVER_STREAM_CAPACITY (4096U)
#define SPP_SERVER_MERGE_CAPACITY  (1024U)
#define SPP_SERVER_INFLIGHT_MAX    (4U)
#define SPP_SERVER_PUSH_CAPACITY   (40U)

/** 不透明预留凭证；同一链路任务持有至 commit/cancel，不得跨会话重用。 */
typedef struct {
    uint32_t generation;
    uint32_t serial;
    uint16_t capacity;
    uint8_t slot;
    uint8_t channel;
} spp_server_tx_token_t;

/** TX 累计字节与状态快照；completed 指 WRITE_EVT 成功，不代表应用 ACK。 */
typedef struct {
    uint32_t accepted;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t errors;
    uint32_t cong;
    uint32_t high_water;
    uint32_t paused;
    uint32_t anon_write_fail;
    uint32_t stray_write;
    uint32_t inflight;
} spp_server_tx_stats_t;

/** 以下接口只允许链路任务、非 ISR；均不等待。reserve 满返回 TIMEOUT，失败零接纳。
 * reply 预留固定 280B，在 Router 之前调用；commit 只使用原预留，未用容量归还。
 * CLOSE 使凭证失效；commit 在锁外复制后原子发布，期间 users 阻止 CLOSE 后复用。
 */
esp_err_t spp_server_reply_reserve(uint32_t handle, spp_server_tx_token_t *token);
esp_err_t spp_server_tx_commit(const spp_server_tx_token_t *token, const uint8_t *data, size_t len);
void spp_server_tx_cancel(const spp_server_tx_token_t *token);
/** PHONE 绑定由角色所有者维护；仅一个句柄可拥有唯一 stream；解绑等待当前 API/copy 退出。
 * reserve 按整条句长接纳，最大4096；不足时零接纳，调用者不能消费源句。
 * 复制可能跨环尾，锁外最多4096B；worker 仅在整句边界插入 reply。
 */
esp_err_t spp_server_stream_bind(uint32_t handle, bool phone);
esp_err_t spp_server_stream_reserve(uint32_t handle, size_t len, spp_server_tx_token_t *token);
/** 最新推送至多40B，覆盖尚未提交的旧推送；不会覆盖 reply reservation。 */
esp_err_t spp_server_push_latest(uint32_t handle, const uint8_t *data, size_t len);
/** reply 可预留的 level；TX_SPACE 只是合并通知，睡眠前必须重查此状态。 */
bool spp_server_reply_ready(uint32_t handle);
/** 任意任务可读、非 ISR；锁内只拷贝小快照。 */
void spp_server_tx_stats(spp_server_tx_stats_t *stats);

/** RX 累计与高水位快照；dropped_bytes 为会话生命期内累计丢字节。 */
typedef struct {
    uint32_t used;
    uint32_t high_water;
    uint32_t dropped_bytes;
    uint32_t rx_events;
    uint32_t drop_events;
} spp_server_rx_session_stats_t;

typedef struct {
    uint32_t evt_dropped;
    uint32_t wake_calls;
    spp_server_rx_session_stats_t sessions[SPP_SERVER_MAX_SESSIONS];
} spp_server_rx_stats_t;

/** RX 唤醒钩子：BTC DATA_IND 在离开 portMUX 后触发，只允许非阻塞唤醒（如任务通知）。 */
typedef void (*spp_server_rx_wake_fn)(void *ctx);

/** 安装/清除（NULL）RX 唤醒钩子；任意时刻可调用。 */
void spp_server_set_rx_wake_hook(spp_server_rx_wake_fn fn, void *ctx);

/** 查询指定或任意会话的 RX 水平状态（有待读数据或未消费的 RX_DROP/DATA 漏投递状态）；阻塞等待前须复查。 */
bool spp_server_rx_ready(uint32_t handle);
bool spp_server_any_rx_ready(void);

/** 任意任务可读、非 ISR；锁内只拷贝小快照。 */
void spp_server_rx_stats(spp_server_rx_stats_t *stats);

/** 请求断开；结果以 SPP_SERVER_EVT_CLOSE 事件返回。未知句柄 → ESP_ERR_NOT_FOUND。 */
esp_err_t spp_server_disconnect(uint32_t handle);

/** 本机 BT MAC（设备名后缀与日志用）。 */
esp_err_t spp_server_get_local_addr(uint8_t addr[SPP_SERVER_ADDR_LEN]);

/** 事件队列满被丢弃的事件数（诊断）。 */
uint32_t spp_server_dropped_events(void);

/* ---- 调试追踪钩子（ADR-0009 §2.2，类别 spp）---- */

/** 驱动事件码；参数含义写在每个码后面。新增码只能追加在末尾。 */
typedef enum {
    SPP_SERVER_TRACE_SRV_START = 0, /* 服务端启动：a = scn */
    SPP_SERVER_TRACE_CONG,          /* 拥塞状态变化：a = 1 拥塞 / 0 解除 */
    SPP_SERVER_TRACE_WRITE_DONE,    /* 一块写完成：a = 字节数，b = 耗时 ms */
    SPP_SERVER_TRACE_WRITE_ERR,     /* 写失败/超时：a = esp_err_t，b = 字节数 */
    SPP_SERVER_TRACE_RX_DROP,       /* RX 缓冲满丢弃：a = 本次丢弃字节，b = 缓冲已用字节 */
    SPP_SERVER_TRACE_AUTH,          /* GAP 认证完成：a = 状态码 */
    SPP_SERVER_TRACE_EVT_DROP,      /* 事件队列满丢事件：a = 事件类型 */
    SPP_SERVER_TRACE_STAGE,         /* SPP 回调阶段结果：a = spp_server_trace_stage_t，b = esp_spp_status_t（0 成功） */
    SPP_SERVER_TRACE_CTRL_FULL,     /* 控制表满，GAP 应答/拒绝断开命令丢失：a = spp_server_trace_ctrl_t */
    SPP_SERVER_TRACE_CTRL_ERR,      /* 控制命令/断开提交失败：a = spp_server_trace_ctrl_t，b = esp_err_t */
    SPP_SERVER_TRACE_REJECT,        /* 无安全槽拒绝连接：a = 1 已发断开 / 0 跳过（同句柄已被新会话接纳） */
    SPP_SERVER_TRACE_WRITE_STRAY,   /* 不属于在途请求的写完成事件（被忽略）：a = 写长度，b = esp_spp_status_t */
    SPP_SERVER_TRACE_WRITE_ANON,    /* 匿名写失败：a = status，b = 受影响会话位图，不猜 owner */
} spp_server_trace_code_t;

/** SPP_SERVER_TRACE_STAGE 的 a。 */
typedef enum {
    SPP_SERVER_TRACE_STAGE_INIT = 0, /* ESP_SPP_INIT_EVT */
    SPP_SERVER_TRACE_STAGE_START,    /* ESP_SPP_START_EVT */
    SPP_SERVER_TRACE_STAGE_UNINIT,   /* ESP_SPP_UNINIT_EVT */
    SPP_SERVER_TRACE_STAGE_SRV_OPEN, /* ESP_SPP_SRV_OPEN_EVT */
} spp_server_trace_stage_t;

/** SPP_SERVER_TRACE_CTRL_FULL / CTRL_ERR 的 a。 */
typedef enum {
    SPP_SERVER_TRACE_CTRL_PIN = 0,    /* GAP PIN 应答 */
    SPP_SERVER_TRACE_CTRL_CONFIRM,    /* GAP SSP 确认应答 */
    SPP_SERVER_TRACE_CTRL_REJECT,     /* 无安全槽时断开新连接 */
    SPP_SERVER_TRACE_CTRL_DISCONNECT, /* 本地请求的断开 */
} spp_server_trace_ctrl_t;

/**
 * 追踪钩子：在 BT 回调任务、worker 任务或调用 write 的任务中被同步调用，必须非阻塞且不做 I/O。
 * handle 为会话句柄（无关时为 0）。
 */
typedef void (*spp_server_trace_fn)(spp_server_trace_code_t code, uint32_t handle, uint32_t a, uint32_t b);

/** 安装/清除（NULL）钩子；任意时刻可调用（原子指针写）。未安装时驱动不产生任何追踪开销以外的工作。 */
void spp_server_set_trace_hook(spp_server_trace_fn fn);

#ifdef __cplusplus
}
#endif
