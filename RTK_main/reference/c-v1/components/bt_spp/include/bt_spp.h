/**
 * @file    bt_spp.h
 * @brief   ESP-IDF Bluedroid 经典蓝牙 SPP 多会话服务端封装（driver/bus 层）。
 *
 * 线程模型：
 * - Bluedroid 回调（BTC 任务）中只做：拷贝数据进该会话的 StreamBuffer、
 *   更新拥塞标志、投递事件到事件队列。不解析协议，不阻塞。
 * - bt_spp_poll_event()/bt_spp_read()/bt_spp_write() 只能由同一个
 *   “链路任务”调用（单一所有者）。
 * 冻结接口，实现方不得修改。
 */

#ifndef BT_SPP_H
#define BT_SPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BT_SPP_MAX_SESSIONS (2U)
#define BT_SPP_RX_BUF_SIZE (2048U) /* 每会话接收 StreamBuffer */
#define BT_SPP_NAME_MAX (32U)

typedef enum
{
    BT_SPP_EVT_OPEN = 0, /* 新会话建立，addr 有效 */
    BT_SPP_EVT_CLOSE,    /* 会话关闭 */
    BT_SPP_EVT_DATA,     /* 该会话 StreamBuffer 有新数据 */
    BT_SPP_EVT_RX_DROP   /* 回调中 StreamBuffer 满，丢弃了 dropped 字节 */
} bt_spp_evt_type_t;

typedef struct
{
    bt_spp_evt_type_t type;
    uint32_t          handle;
    uint8_t           addr[6];
    uint32_t          dropped;
} bt_spp_evt_t;

/**
 * 初始化控制器（BR/EDR only）、Bluedroid、GAP（SSP，IO 能力 NONE，
 * 可连接可发现）、SPP（ESP_SPP_MODE_CB），启动 SPP 服务端
 * （ESP_SPP_SEC_NONE，与 5.4 一致）。p_name 为蓝牙设备名，
 * 长度 ≤ BT_SPP_NAME_MAX-1，不得含换行。
 * 任一步失败：返回错误码，记录日志，已初始化的部分按逆序释放。
 */
esp_err_t bt_spp_init (char const * p_name);

/** 等待事件，timeout_ms=0 不等待。有事件返回 true。 */
bool bt_spp_poll_event (bt_spp_evt_t * p_evt, uint32_t timeout_ms);

/** 从会话 StreamBuffer 非阻塞读取，返回字节数；未知句柄返回 0。 */
size_t bt_spp_read (uint32_t handle, uint8_t * p_buf, size_t size);

/**
 * 发送。esp_spp_write 为异步；本函数在会话拥塞时最多等待 timeout_ms
 * （等待 ESP_SPP_CONG_EVT 解除），超时返回 ESP_ERR_TIMEOUT。
 * 必须先核实 IDF v6.1 btc_spp.c 中 esp_spp_write 是否拷贝数据，
 * 若不拷贝，须保证缓冲区在 ESP_SPP_WRITE_EVT 前有效。
 */
esp_err_t bt_spp_write (uint32_t handle, uint8_t const * p_data, size_t len,
                        uint32_t timeout_ms);

/** 主动断开会话（结果以 BT_SPP_EVT_CLOSE 事件返回）。 */
esp_err_t bt_spp_disconnect (uint32_t handle);

/** 本机 BT MAC，用于设备名后缀与日志。 */
esp_err_t bt_spp_get_local_addr (uint8_t p_addr[6]);

#endif /* BT_SPP_H */

/*** end of file ***/
