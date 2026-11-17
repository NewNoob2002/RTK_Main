// spp_server 启动期外供存储视图（ADR-0011 §7-8；非冻结头，纯 C 接口，不依赖 C++ 构造）。
// 线程：只在 spp_server_init() 之前由主启动线程绑定一次；所传缓冲区寿命须覆盖整个运行期。
// 堆与阻塞：本头不分配内存；stop_boot 会轮询等待发送任务停泊，其余接口不阻塞。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "SppServer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 发送任务静态栈字节数（与 SppServerPriv.h 一致）。 */
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

/**
 * @brief  绑定启动期外供缓冲区视图（须在 spp_server_init() 之前调用）
 * @param  storage: 缓冲区视图，非 NULL；各项指针非空、长度不小于对应容量
 * @retval ESP_OK 已绑定
 * @retval ESP_ERR_INVALID_ARG storage 为 NULL、某项为空或长度不足，栈长度非 StackType_t 整数倍或栈未按 4 字节对齐
 * @retval ESP_ERR_INVALID_STATE 已初始化、已请求停止，或发送任务已用另一组缓冲创建
 * @note   只在启动线程调用一次；临界区内只比较指针并拷贝结构体；不分配内存、不阻塞、非 ISR
 */
esp_err_t spp_server_bind_storage(const spp_server_storage_t *storage);

/**
 * @brief  清除外供存储引用
 * @retval ESP_OK 已清除（之后未绑定任何缓冲区）
 * @retval ESP_ERR_INVALID_STATE 已初始化、发送任务仍存在或已请求停止
 * @note   仅主启动任务在消费者尚未启动时调用；只清指针，不释放缓冲区；不阻塞、非 ISR
 */
esp_err_t spp_server_unbind_storage(void);

/**
 * @brief  停机启动期资源：先停发送任务，再关闭 SPP、Bluedroid 与控制器
 * @retval ESP_OK 已停机；驱动未初始化时同样返回 ESP_OK
 * @retval ESP_ERR_TIMEOUT 发送任务 2 s 内未停泊，或 SPP UNINIT 完成位未到
 * @retval 其他 esp_spp / Bluedroid / 控制器关闭步骤的 IDF 错误码
 * @note   仅主启动任务在链路任务停稳后调用；轮询等待，每步上限 2 s；非 ISR
 * @note   中途失败不回滚；事件组与会话对象保留（迟到回调仍有稳定地址），此后只允许重启
 */
esp_err_t spp_server_stop_boot(void);

#ifdef __cplusplus
}
#endif
