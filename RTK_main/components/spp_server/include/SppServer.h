// ESP-IDF Bluedroid 经典蓝牙 SPP 多会话服务端，C 实现（仅 SOC_BT_CLASSIC_SUPPORTED 目标），最多 2 个会话。
// 线程：GAP/SPP 回调（BTC 任务）只拷贝数据、改状态、非阻塞投递；poll_event/read/write 只允许同一链路任务。
// 堆与阻塞：无堆分配；回调内不阻塞，init/stop_boot 与 write 会阻塞等待 BTC 队列或事件。冻结接口（ADR-0003）。
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
#define SPP_SERVER_LEGACY_PIN      "1234" /* GAP PIN_REQ 的应答 PIN */

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
 * @brief  初始化控制器（BR/EDR only，释放 BLE 内存）、Bluedroid、GAP(SSP) 与 SPP，启动服务端并可发现
 * @param  name: 蓝牙设备名，1..SPP_SERVER_NAME_MAX 字节，不得含 '\n' 或 '\r'
 * @retval ESP_OK 启动完成，服务端已可接受连接
 * @retval ESP_ERR_INVALID_ARG name 为 NULL、空串、超长或含换行
 * @retval ESP_ERR_INVALID_STATE 已初始化、未绑定外供存储，或已请求停止
 * @retval 其他 失败步骤的 IDF 错误码；返回前已按逆序释放本次已初始化部分
 * @note   只在启动任务调用；会阻塞等待 SPP INIT/START 事件（各 5 s）；非 ISR；不分配堆
 * @note   GAP 用 IO 能力 NONE、CFM_REQ 自动确认、PIN_REQ 回 SPP_SERVER_LEGACY_PIN；SPP 用 ESP_SPP_MODE_CB
 */
esp_err_t spp_server_init(const char *name);

/**
 * @brief  等待并取出一条会话事件（生命周期事件优先）
 * @param[out] evt: 事件输出；返回 false 时内容未定义
 * @param  timeout_ms: 最长等待毫秒数，0 表示不等待
 * @retval true 取到事件
 * @retval false 超时、未初始化、队列未创建或 evt 为 NULL
 * @note   只允许链路任务（唯一消费者）调用；最多阻塞 timeout_ms；队列无事件时按 RX/TX 水位补发事件；非 ISR
 */
bool spp_server_poll_event(spp_server_evt_t *evt, uint32_t timeout_ms);

/**
 * @brief  非阻塞读取会话接收缓冲
 * @param  handle: 会话句柄
 * @param[out] buf: 输出缓冲；只写返回值那么多个字节，其余不修改
 * @param  size: buf 容量
 * @retval 实际读到的字节数
 * @retval 0 buf 为 NULL、size 为 0、句柄未知或当前无数据
 * @note   只允许链路任务调用；不阻塞；登记期间该槽不会被复用或被新连接 reset
 */
size_t spp_server_read(uint32_t handle, uint8_t *buf, size_t size);

/**
 * @brief  同步兼容发送：按 TX_CHUNK 分块提交并等待每块的真实 WRITE_EVT
 * @param  handle: 会话句柄
 * @param  data: 数据，非 NULL
 * @param  len: 字节数，须大于 0
 * @param  timeout_ms: 全次共享的等待预算（0 时不提交即超时）
 * @retval ESP_OK 全部字节已由 SDK 确认写出
 * @retval ESP_ERR_INVALID_ARG data 为 NULL 或 len 为 0
 * @retval ESP_ERR_INVALID_STATE 驱动/发送任务未就绪，或等待期间会话已断开
 * @retval ESP_ERR_NOT_FOUND 句柄无对应会话
 * @retval ESP_ERR_TIMEOUT 预算耗尽；已提交的字节不撤回
 * @retval 其他 提交失败或该块的写完成错误码
 * @note   只允许链路任务调用；会阻塞至多 timeout_ms；非 ISR；不解析数据内容
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

/**
 * @brief  为一条应答预留发射空间（固定 SPP_SERVER_REPLY_RESERVE 字节）
 * @param  handle: 会话句柄
 * @param[out] token: 预留凭证；失败时内容未定义
 * @retval ESP_OK 预留成功，容量已记入 token
 * @retval ESP_ERR_INVALID_ARG token 为 NULL
 * @retval ESP_ERR_INVALID_STATE 驱动未初始化
 * @retval ESP_ERR_NOT_FOUND 句柄无对应会话
 * @retval ESP_ERR_TIMEOUT 断开中、正在复制、已有未提交预留或空闲不足（不接纳任何字节）
 * @note   只允许链路任务、非 ISR；不阻塞；须在 Router 之前调用
 */
esp_err_t spp_server_reply_reserve(uint32_t handle, spp_server_tx_token_t *token);

/**
 * @brief  用预留容量提交一条应答记录
 * @param  token: reply_reserve 得到的凭证
 * @param  data: 待发送数据，非 NULL
 * @param  len: 字节数，须大于 0 且不超过凭证容量
 * @retval ESP_OK 已在锁外复制并整条发布，待发送任务写出
 * @retval ESP_ERR_INVALID_ARG token/data 为 NULL、len 为 0 或超过凭证容量
 * @retval ESP_ERR_INVALID_STATE 凭证已失效（会话关闭、槽复用）或该环正在复制
 * @note   只允许链路任务、非 ISR；复制在锁外（≤280 B），复制期间 users 阻止槽回收
 * @note   发布前更新句末标记，发送任务不会看到半句；失败时预留不自动释放，由调用者 cancel
 */
esp_err_t spp_server_tx_commit(const spp_server_tx_token_t *token, const uint8_t *data, size_t len);

/**
 * @brief  把两段视图拼成一条记录提交（源环跨尾时免中转缓冲）
 * @param  token: 预留凭证
 * @param  a: 第一段，非 NULL
 * @param  alen: 第一段字节数，须大于 0
 * @param  b: 第二段，非 NULL
 * @param  blen: 第二段字节数，须大于 0；alen+blen 不超过凭证容量
 * @retval ESP_OK 整条已发布
 * @retval ESP_ERR_INVALID_ARG 任一段为 NULL/空，或总长超过凭证容量
 * @retval ESP_ERR_INVALID_STATE 凭证已失效或该环正在复制
 * @note   语义同 tx_commit；失败时预留仍在，由调用者 cancel
 */
esp_err_t spp_server_tx_commit2(const spp_server_tx_token_t *token, const uint8_t *a, size_t alen,
                                const uint8_t *b, size_t blen);

/**
 * @brief  释放未提交的预留并唤醒发送任务
 * @param  token: 预留凭证；非法或已失效的凭证被忽略
 * @note   只允许链路任务、非 ISR；不阻塞；正在复制的环不撤销
 */
void spp_server_tx_cancel(const spp_server_tx_token_t *token);
/**
 * @brief  维护唯一 stream 所有者：手机侧由角色所有者绑定，解绑后不再接纳流数据
 * @param  handle: 会话句柄
 * @param  phone: true 请求绑定为 PHONE 流所有者，false 释放本会话的所有权
 * @retval ESP_OK 已绑定、已解绑，或本就不是所有者（无操作）
 * @retval ESP_ERR_NOT_FOUND 句柄无对应会话
 * @retval ESP_ERR_TIMEOUT 已有其他句柄持有所有权（仅 phone=true 路径）
 * @note   只允许链路任务、非 ISR；不阻塞；解绑只置位并回收已发布数据，在途复制由 users 保护
 */
esp_err_t spp_server_stream_bind(uint32_t handle, bool phone);

/**
 * @brief  为一条完整流句预留发射空间（最大 SPP_SERVER_STREAM_CAPACITY）
 * @param  handle: 会话句柄
 * @param  len: 整句字节数，须大于 0 且不超过 SPP_SERVER_STREAM_CAPACITY
 * @param[out] token: 预留凭证；失败时内容未定义
 * @retval ESP_OK 预留成功
 * @retval ESP_ERR_INVALID_ARG token 为 NULL、len 为 0 或超过 4096
 * @retval ESP_ERR_INVALID_STATE 驱动未初始化、调用者不是流所有者或流正在解绑
 * @retval ESP_ERR_NOT_FOUND 句柄无对应会话
 * @retval ESP_ERR_TIMEOUT 断开中、正在复制、已有未提交预留或空闲不足（零接纳）
 * @note   只允许链路任务、非 ISR；整句一次预留，发送任务只在句末插入应答
 */
esp_err_t spp_server_stream_reserve(uint32_t handle, size_t len, spp_server_tx_token_t *token);

/**
 * @brief  用最新一帧覆盖会话的 PUSH 邮箱
 * @param  handle: 会话句柄
 * @param  data: 帧数据，非 NULL，≤ SPP_SERVER_PUSH_CAPACITY
 * @param  len: 字节数，须大于 0
 * @retval ESP_OK 已写入邮箱并唤醒发送任务
 * @retval ESP_ERR_INVALID_ARG data 为 NULL、len 为 0 或超过 40
 * @retval ESP_ERR_NOT_FOUND 句柄无对应会话或正在断开
 * @note   只允许链路任务、非 ISR；旧推送未发送即被覆盖并按取消计数；不占用 reply 预留
 */
esp_err_t spp_server_push_latest(uint32_t handle, const uint8_t *data, size_t len);

/**
 * @brief  查询现在能否预留一条应答
 * @param  handle: 会话句柄
 * @retval true 未断开、未在复制、无未提交预留且空闲不少于 SPP_SERVER_REPLY_RESERVE
 * @retval false 上述任一条件不满足，或句柄无对应会话
 * @note   TX_SPACE 只是合并通知，睡眠前须重新查询本状态；任意任务可调、不阻塞、非 ISR
 */
bool spp_server_reply_ready(uint32_t handle);

/**
 * @brief  读取 TX 累计统计快照
 * @param[out] stats: 统计输出；为 NULL 时忽略
 * @note   任意任务可读、非 ISR；锁内只拷贝小快照
 */
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

/**
 * @brief  安装或清除（fn 为 NULL）接收唤醒钩子
 * @param  fn: 唤醒回调，`fn(ctx)` 形式调用；NULL 表示清除
 * @param  ctx: 透传给回调的上下文
 * @note   任意任务、任意时刻可调（含 init 之前）；指针存在 s_spp 之外，跨重连与重启重试保留
 * @note   回调在 BTC 任务里、离开临界区之后执行，必须非阻塞且不做 I/O；不阻塞调用者
 */
void spp_server_set_rx_wake_hook(spp_server_rx_wake_fn fn, void *ctx);

/**
 * @brief  查询指定会话是否有待读数据
 * @param  handle: 会话句柄
 * @retval true 缓冲有数据，或有尚未消费的漏投递/丢弃标记
 * @retval false 无数据，或驱动未初始化、句柄无对应会话
 * @note   任意任务可调、不阻塞、非 ISR；查询期间登记 users，槽不会被复用；阻塞等待前须复查
 */
bool spp_server_rx_ready(uint32_t handle);

/**
 * @brief  查询是否任一在用会话有待读数据
 * @retval true 至少一个会话有数据或未消费的漏投递/丢弃标记
 * @retval false 全部空闲，或驱动未初始化
 * @note   任意任务可调、不阻塞、非 ISR
 */
bool spp_server_any_rx_ready(void);

/**
 * @brief  读取 RX 累计与高水位快照
 * @param[out] stats: 统计输出，先清零；为 NULL 时直接返回
 * @note   任意任务可读、非 ISR；锁内只拷贝标量，used 在登记 users 后从 StreamBuffer 取
 */
void spp_server_rx_stats(spp_server_rx_stats_t *stats);

/**
 * @brief  请求断开一个会话
 * @param  handle: 会话句柄
 * @retval ESP_OK 已置位断开请求；最终结果以 SPP_SERVER_EVT_CLOSE 事件返回
 * @retval ESP_ERR_NOT_FOUND 句柄无对应会话
 * @note   任意任务可调（重复请求仍返回 ESP_OK）；不阻塞、非 ISR；SDK 的断开调用由发送任务执行
 */
esp_err_t spp_server_disconnect(uint32_t handle);

/**
 * @brief  读取本机蓝牙 MAC（设备名后缀与日志用）
 * @param[out] addr: 输出 SPP_SERVER_ADDR_LEN 字节；失败时不修改
 * @retval ESP_OK 已拷贝
 * @retval ESP_ERR_INVALID_ARG addr 为 NULL
 * @retval ESP_ERR_INVALID_STATE 控制器未上电，拿不到本机地址
 * @note   任意任务可调、不阻塞、非 ISR
 */
esp_err_t spp_server_get_local_addr(uint8_t addr[SPP_SERVER_ADDR_LEN]);

/**
 * @brief  读取事件队列满时被丢弃的事件累计数（诊断）
 * @retval 累计丢弃数
 * @note   任意任务可调；锁内只读一个计数；非 ISR
 */
uint32_t spp_server_dropped_events(void);

/** 调试追踪钩子（ADR-0009 §2.2，类别 spp）：驱动事件码，参数含义写在每个码后面；新增码只能追加在末尾。 */
typedef enum {
    SPP_SERVER_TRACE_SRV_START = 0, /* 服务端启动：a = scn */
    SPP_SERVER_TRACE_CONG,          /* 拥塞状态变化：a = 1 拥塞 / 0 解除 */
    SPP_SERVER_TRACE_WRITE_DONE,    /* 一块写完成：a = 字节数，b = 耗时 ms */
    SPP_SERVER_TRACE_WRITE_ERR,     /* 写失败/超时：a = esp_err_t，b = 字节数 */
    SPP_SERVER_TRACE_RX_DROP,       /* RX 缓冲满丢弃：a = 本次丢弃字节，b = 缓冲已用字节 */
    SPP_SERVER_TRACE_AUTH,          /* GAP 认证完成：a = 状态码 */
    SPP_SERVER_TRACE_EVT_DROP,      /* 事件队列满丢事件：a = 事件类型 */
    SPP_SERVER_TRACE_STAGE, /* SPP 回调阶段结果：a = spp_server_trace_stage_t，b = esp_spp_status_t（0 成功） */
    SPP_SERVER_TRACE_CTRL_FULL, /* 控制表满，GAP 应答/拒绝断开命令丢失：a = spp_server_trace_ctrl_t */
    SPP_SERVER_TRACE_CTRL_ERR, /* 控制命令/断开提交失败：a = spp_server_trace_ctrl_t，b = esp_err_t */
    SPP_SERVER_TRACE_REJECT, /* 无安全槽拒绝连接：a = 1 已发断开 / 0 跳过（同句柄已被新会话接纳） */
    SPP_SERVER_TRACE_WRITE_STRAY, /* 不属于在途请求的写完成事件（被忽略）：a = 写长度，b = esp_spp_status_t */
    SPP_SERVER_TRACE_WRITE_ANON, /* 匿名写失败：a = status，b = 受影响会话位图，不猜 owner */
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
typedef void (*spp_server_trace_fn)(spp_server_trace_code_t code, uint32_t handle, uint32_t a,
                                    uint32_t b);

/**
 * @brief  安装或清除（fn 为 NULL）调试追踪钩子
 * @param  fn: 追踪回调，NULL 表示清除
 * @note   任意任务、任意时刻可调；临界区内只写一个指针，读侧拷贝指针后在锁外调用；未安装时无额外开销
 */
void spp_server_set_trace_hook(spp_server_trace_fn fn);

#ifdef __cplusplus
}
#endif
