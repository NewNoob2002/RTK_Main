// 冻结接口（ADR-0003）。实现方不得修改。
// ESP-IDF Bluedroid 经典蓝牙 SPP 多会话服务端，C 实现（仅 SOC_BT_CLASSIC_SUPPORTED 目标）。
// 线程模型：
// - Bluedroid GAP/SPP 回调（BTC 任务）中只做：把数据拷进该会话的静态 StreamBuffer、更新拥塞标志、
//   向静态事件队列非阻塞投递。不解析协议、不阻塞、不打印数据内容。
// - poll_event/read/write/disconnect 只允许同一个链路任务调用（单一所有者）。
// IDF v6.1 已核实（reference/c-v1/docs/reports/T04.md）：esp_spp_write 深拷贝；SRV_OPEN 后服务端继续监听，
// 最多 ESP_SPP_MAX_SESSION(=2) 个会话；对端地址在 srv_open.rem_bda。
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

#define RTK_BT_SPP_MAX_SESSIONS    (2U)
/* 每会话接收 StreamBuffer（ADR-0006）：手机经 NTRIP 转发 RTCM 时一次突发可达数 KB，由 Kconfig 配置；
 * 非 IDF 构建（host 检查）没有 sdkconfig，回退到原值 2048。 */
#if defined(CONFIG_RTK_BT_SPP_RX_BUF_SIZE)
#define RTK_BT_SPP_RX_BUF_SIZE ((size_t)CONFIG_RTK_BT_SPP_RX_BUF_SIZE)
#else
#define RTK_BT_SPP_RX_BUF_SIZE (2048U)
#endif
#define RTK_BT_SPP_EVENT_QUEUE_LEN (16U)
#define RTK_BT_SPP_NAME_MAX        (31U) /* 设备名最大字节数（不含结尾 0） */
#define RTK_BT_SPP_ADDR_LEN        (6U)
#define RTK_BT_SPP_LEGACY_PIN      "1234" /* 旧设备 PIN_REQ 应答，与 5.4 注释一致 */

typedef enum {
    RTK_BT_SPP_EVT_OPEN = 0, /* 新会话建立，addr 有效 */
    RTK_BT_SPP_EVT_CLOSE,    /* 会话关闭 */
    RTK_BT_SPP_EVT_DATA,     /* 该会话 StreamBuffer 有新数据 */
    RTK_BT_SPP_EVT_RX_DROP,  /* 回调中缓冲满，丢弃 dropped 字节 */
} rtk_bt_spp_evt_type_t;

typedef struct {
    rtk_bt_spp_evt_type_t type;
    uint32_t handle;
    uint8_t addr[RTK_BT_SPP_ADDR_LEN];
    uint32_t dropped;
} rtk_bt_spp_evt_t;

/**
 * 初始化控制器（BR/EDR only，释放 BLE 内存）、Bluedroid、GAP（SSP，IO 能力 NONE，CFM_REQ 自动确认，
 * PIN_REQ 回 RTK_BT_SPP_LEGACY_PIN）、SPP（ESP_SPP_MODE_CB）；在 SPP INIT 事件中启动服务端
 * （ESP_SPP_SEC_NONE，与 5.4 一致）、设置设备名、可连接+可发现。
 * name 为 NULL/空/超过 RTK_BT_SPP_NAME_MAX/含 '\n' 或 '\r' → ESP_ERR_INVALID_ARG；重复调用 → ESP_ERR_INVALID_STATE；
 * 其他步骤失败：按逆序释放已初始化部分，返回该步骤的错误码。
 */
esp_err_t rtk_bt_spp_init(const char *name);

/** 等待事件，timeout_ms=0 不等待。有事件返回 true；未初始化或 evt 为 NULL 返回 false。 */
bool rtk_bt_spp_poll_event(rtk_bt_spp_evt_t *evt, uint32_t timeout_ms);

/** 从会话 StreamBuffer 非阻塞读取，返回字节数；未知句柄或参数非法返回 0。 */
size_t rtk_bt_spp_read(uint32_t handle, uint8_t *buf, size_t size);

/**
 * 发送。拥塞时等待 ESP_SPP_CONG_EVT 解除，总等待不超过 timeout_ms，超时返回 ESP_ERR_TIMEOUT。
 * 未知句柄 → ESP_ERR_NOT_FOUND；data 为 NULL 或 len 为 0 → ESP_ERR_INVALID_ARG。
 */
esp_err_t rtk_bt_spp_write(uint32_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms);

/** 请求断开；结果以 RTK_BT_SPP_EVT_CLOSE 事件返回。未知句柄 → ESP_ERR_NOT_FOUND。 */
esp_err_t rtk_bt_spp_disconnect(uint32_t handle);

/** 本机 BT MAC（设备名后缀与日志用）。 */
esp_err_t rtk_bt_spp_get_local_addr(uint8_t addr[RTK_BT_SPP_ADDR_LEN]);

/** 事件队列满被丢弃的事件数（诊断）。 */
uint32_t rtk_bt_spp_dropped_events(void);

#ifdef __cplusplus
}
#endif
