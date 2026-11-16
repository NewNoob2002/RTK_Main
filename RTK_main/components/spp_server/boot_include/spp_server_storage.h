/* spp_server 启动期外供存储视图（ADR-0011 §7-8；非冻结头，纯 C 接口）。
 * 线程安全：仅在 spp_server_init() 之前由主启动线程调用一次；所传入缓冲区寿命须覆盖整个运行期。
 * 本头不依赖 C++ 构造，可用于 C 驱动与 C++ 组合根。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "spp_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 发送任务静态栈字节数（与 spp_server_priv.h 一致）。 */
#define SPP_SERVER_WORKER_STACK_BYTES (2560U)

typedef struct {
    uint8_t *rx_buf[SPP_SERVER_MAX_SESSIONS];
    size_t rx_buf_bytes[SPP_SERVER_MAX_SESSIONS];
    uint8_t *reply_buf[SPP_SERVER_MAX_SESSIONS];
    size_t reply_buf_bytes[SPP_SERVER_MAX_SESSIONS];
    uint8_t *stream_buf;
    size_t stream_buf_bytes;
    uint8_t *merge_buf;
    size_t merge_buf_bytes;
    StackType_t *worker_stack;
    size_t worker_stack_bytes;
} spp_server_storage_t;

/* 绑定启动期分配的外供缓冲区视图（须在 spp_server_init() 前调用）。
 * 参数空、长度不足或栈指针/长度未对齐返回 ESP_ERR_INVALID_ARG；已初始化或 worker 已绑定不同栈返回
 * ESP_ERR_INVALID_STATE。 */
esp_err_t spp_server_bind_storage(const spp_server_storage_t *storage);

/* 仅主启动任务在消费者尚未启动时调用；清除外供引用，worker 已存在时拒绝。非 ISR。 */
esp_err_t spp_server_unbind_storage(void);

/* 仅主启动任务在应用任务停稳后调用；停 worker 并等待 SPP UNINIT，再关闭 BT。
 * 外供块与 RTOS 控制对象保留到重启；失败也不得释放或重绑，非 ISR。 */
esp_err_t spp_server_stop_boot(void);

#ifdef __cplusplus
}
#endif
