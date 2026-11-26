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

#ifdef __cplusplus
}
#endif
