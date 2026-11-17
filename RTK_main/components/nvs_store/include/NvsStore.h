// NVS 持久化：设备 SN 与报警器白名单；命名空间 "rtk"，键 "sn"（字符串）、"alarm_wl"（n×6 字节 blob）。
// 仅由启动流程与存储执行器任务串行调用，二者不并发；不可在 ISR 中调用；nvs_* 调用会阻塞。
// 不分配堆（读白名单用固定 24 字节栈缓冲）。冻结接口（ADR-0003）。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_STORE_SN_MAX    (16U) /* 不含结尾 0 */
#define NVS_STORE_ALARM_MAX (4U)
#define NVS_STORE_ADDR_LEN  (6U)

/**
 * @brief  初始化 NVS 分区；分区无空页或版本不符时擦除后重试一次
 * @retval ESP_OK 初始化成功
 * @retval ESP_ERR_NVS_NO_FREE_PAGES 擦除或重试后仍无空页
 * @retval ESP_ERR_NVS_NEW_VERSION_FOUND 擦除或重试后版本仍不符
 * @retval 其他 nvs_flash_erase/nvs_flash_init 的错误码（已记 E 级日志）
 * @note   只在启动期单线程调用；非 ISR 安全；会阻塞；分支内的擦除会丢失分区全部数据
 */
esp_err_t nvs_store_init(void);

/**
 * @brief  读取设备 SN 字符串
 * @param[out] out: 输出缓冲；成功时为 0 结尾字符串，检查参数后即清空，参数非法时不写入
 * @param  out_size: out 的字节容量，必须 ≥ NVS_STORE_SN_MAX + 1
 * @retval ESP_OK 读到长度 1..NVS_STORE_SN_MAX 的 SN
 * @retval ESP_ERR_INVALID_ARG out 为 NULL 或 out_size 过小
 * @retval ESP_ERR_NVS_NOT_FOUND 命名空间或 "sn" 键不存在
 * @retval ESP_ERR_INVALID_SIZE 存储值为空串、超过 NVS_STORE_SN_MAX，或长于 out_size（长度不符时已清空 out）
 * @retval 其他 nvs_open/nvs_get_str 错误码
 * @note   非 ISR 安全；nvs 调用会阻塞；不分配堆
 */
esp_err_t nvs_store_get_sn(char *out, size_t out_size);

/**
 * @brief  写入设备 SN，覆盖旧值
 * @param  sn: 非空 0 结尾字符串，长度 1..NVS_STORE_SN_MAX
 * @retval ESP_OK 已写入并 commit
 * @retval ESP_ERR_INVALID_ARG sn 为 NULL、空串或超长
 * @retval 其他 nvs_open/nvs_set_str/nvs_commit 错误码；未 commit 时 flash 中旧值不变
 * @note   非 ISR 安全；nvs 调用与 commit 会阻塞；不分配堆
 */
esp_err_t nvs_store_set_sn(const char *sn);

/**
 * @brief  读取报警器白名单，每项为 6 字节地址
 * @param[out] addrs: 输出行数组，最多 max 项；仅 max==0 时可传 NULL
 * @param  max: addrs 可容纳的条目数
 * @param[out] count: 实际读出的条目数（≤ max）；参数合法但读取失败或空白名单时为 0，INVALID_ARG 时不写入
 * @retval ESP_OK 读取成功，或白名单不存在/为空（count=0）
 * @retval ESP_ERR_INVALID_ARG count 为 NULL，或 addrs 为 NULL 且 max≠0
 * @retval ESP_ERR_INVALID_SIZE blob 长度不是 6 的倍数，或超过 NVS_STORE_ALARM_MAX×6
 * @retval 其他 nvs_open/nvs_get_blob 错误码
 * @note   非 ISR 安全；nvs 调用会阻塞；使用固定 24 字节栈缓冲，不分配堆
 */
esp_err_t nvs_store_get_alarms(uint8_t (*addrs)[NVS_STORE_ADDR_LEN], size_t max, size_t *count);

/**
 * @brief  覆盖写报警器白名单；count=0 时删除该键
 * @param  addrs: count 行 6 字节地址；count=0 时可传 NULL
 * @param  count: 条目数，不得超过 NVS_STORE_ALARM_MAX
 * @retval ESP_OK 已写入或删除并 commit
 * @retval ESP_ERR_INVALID_ARG count 超限，或 addrs 为 NULL 且 count≠0
 * @retval 其他 nvs_open/nvs_set_blob/nvs_erase_key/nvs_commit 错误码；未 commit 时旧白名单不变
 * @note   非 ISR 安全；nvs 调用与 commit 会阻塞；不分配堆
 */
esp_err_t nvs_store_set_alarms(const uint8_t (*addrs)[NVS_STORE_ADDR_LEN], size_t count);

#ifdef __cplusplus
}
#endif
