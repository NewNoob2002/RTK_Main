// 组合根使用的 NVS 启动期接口。仅启动任务，可阻塞，非 ISR。
#pragma once

#include "NvsStore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nvs_store_boot_migration_fn)(nvs_store_boot_scratch_fn with_scratch);

/**
 * @brief 登记由下一次 init 消费的迁移钩子，两个实例初始化且身份已比对后调用。
 * @param migrate: 迁移钩子，可空；调用时传入本次启动的 scratch 提供者。
 * @note 仅启动任务，非 ISR。
 */
void nvs_store_set_boot_migration(nvs_store_boot_migration_fn migrate);

/**
 * @brief 只读访问系统 NVS 的 EEPROM blob。
 * @param[out] blob: 目标缓冲，为 NULL 时仅查询长度。
 * @param[in,out] len: 缓冲容量，成功时为实际长度。
 * @retval NVS 读取结果。
 * @note 启动任务，可阻塞，非 ISR。
 */
esp_err_t nvs_store_read_legacy_eeprom(uint8_t *blob, size_t *len);

/**
 * @brief 仅当两个身份键均缺失时提交并读回 SN 或 PN 的双副本。
 * @param sn: true 为 SN，false 为 PN。
 * @param value: 经校验的 SN 字符串（含 NUL）或定长 PN 字节。
 * @param len: value 字节数，SN 含 NUL，PN 为 NVS_STORE_PN_BYTES。
 * @param[out] written: 两副本写入并验证后为 true，已存在时为 false。
 * @retval ESP_OK 已迁移或保留现有键；其他为参数或 NVS 错误。
 * @note 仅在两实例就绪且 C6 副本比对完成后调用，可阻塞，非 ISR。
 */
esp_err_t nvs_store_migrate_identity(bool sn, const void *value, size_t len, bool *written);

#ifdef __cplusplus
}
#endif
