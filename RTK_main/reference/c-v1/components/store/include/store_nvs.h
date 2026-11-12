/**
 * @file    store_nvs.h
 * @brief   NVS 持久化：设备 SN、报警器白名单（其余配置预留）。
 *
 * 命名空间 "rtk"；键："sn"（字符串），"alarm_wl"（blob，n×6 字节）。
 * 冻结接口，实现方不得修改。
 */

#ifndef STORE_NVS_H
#define STORE_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STORE_SN_MAX (16U) /* 不含结尾 0 */
#define STORE_ALARM_MAX (4U)

/**
 * 初始化 nvs_flash。遇到 ESP_ERR_NVS_NO_FREE_PAGES 或
 * ESP_ERR_NVS_NEW_VERSION_FOUND 时擦除 nvs 分区后重试一次
 * （仅 nvs 分区，IDF 标准做法），并在日志中写 WARNING。
 */
esp_err_t store_init (void);

/** 读 SN 到 p_out（保证 0 结尾）。不存在返回 ESP_ERR_NVS_NOT_FOUND。 */
esp_err_t store_get_sn (char * p_out, size_t out_size);
esp_err_t store_set_sn (char const * p_sn);

/** 读白名单，*p_count 返回条数（≤ max）。不存在时 *p_count=0 返回 ESP_OK。 */
esp_err_t store_get_alarms (uint8_t (*p_addrs)[6], uint8_t max,
                            uint8_t * p_count);

/** 覆盖写白名单，count=0 表示清空（删除键）。写后 nvs_commit。 */
esp_err_t store_set_alarms (uint8_t const (*p_addrs)[6], uint8_t count);

#endif /* STORE_NVS_H */

/*** end of file ***/
