// NVS 持久化：设备 SN、报警器白名单（其余配置预留）。C 实现。
// 命名空间 "rtk"；键 "sn"（字符串）、"alarm_wl"（blob，n×6 字节）。每次操作 nvs_open→操作→nvs_close，写后 nvs_commit。
// 线程安全：否（只由启动流程与链路任务调用，二者不并发）。
// 冻结接口（ADR-0003）。实现方不得修改。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTK_STORE_SN_MAX    (16U) /* 不含结尾 0 */
#define RTK_STORE_ALARM_MAX (4U)
#define RTK_STORE_ADDR_LEN  (6U)

/**
 * nvs_flash_init；遇到 ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND 时只擦除 nvs 分区后重试一次
 * （IDF 标准做法），并以 WARNING 日志记录。
 */
esp_err_t rtk_store_init(void);

/**
 * 读 SN 到 out（保证 0 结尾），out_size 至少 RTK_STORE_SN_MAX + 1。
 * 不存在 → ESP_ERR_NVS_NOT_FOUND；存储值为空或超长 → ESP_ERR_INVALID_SIZE；参数非法 → ESP_ERR_INVALID_ARG。
 */
esp_err_t rtk_store_get_sn(char *out, size_t out_size);

/** sn 为 NULL、空或长度 > RTK_STORE_SN_MAX → ESP_ERR_INVALID_ARG。 */
esp_err_t rtk_store_set_sn(const char *sn);

/**
 * 读白名单到 addrs，*count 返回条数（≤ max）。不存在：*count=0 返回 ESP_OK。
 * blob 长度不是 6 的倍数或超过 RTK_STORE_ALARM_MAX×6：*count=0 返回 ESP_ERR_INVALID_SIZE。
 */
esp_err_t rtk_store_get_alarms(uint8_t (*addrs)[RTK_STORE_ADDR_LEN], size_t max, size_t *count);

/** 覆盖写；count=0 表示删除键。count > RTK_STORE_ALARM_MAX 或 addrs 为 NULL 且 count>0 → ESP_ERR_INVALID_ARG。 */
esp_err_t rtk_store_set_alarms(const uint8_t (*addrs)[RTK_STORE_ADDR_LEN], size_t count);

#ifdef __cplusplus
}
#endif
