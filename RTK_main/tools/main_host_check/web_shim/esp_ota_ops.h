// host 替身：WebOta 和 OtaRuntime 使用的 ESP-IDF v6.1 OTA 接口。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_APP_DESC_MAGIC_WORD            0xabcd5432U
#define ESP_ERR_OTA_VALIDATE_FAILED        0x1503
#define ESP_ERR_OTA_ROLLBACK_INVALID_STATE 0x1506
#define OTA_WITH_SEQUENTIAL_WRITES         0xfffffffeU

typedef struct {
    uint32_t magic_word;
    char unused_prefix[12];
    char version[32];
    char project_name[32];
    char unused_suffix[176];
} esp_app_desc_t;

typedef struct {
    uint32_t address;
    uint32_t size;
    char label[17];
} esp_partition_t;

typedef enum {
    ESP_OTA_IMG_NEW,
    ESP_OTA_IMG_PENDING_VERIFY,
    ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID,
    ESP_OTA_IMG_ABORTED,
    ESP_OTA_IMG_UNDEFINED = UINT32_MAX,
} esp_ota_img_states_t;

typedef uint32_t esp_ota_handle_t;

const esp_app_desc_t *esp_app_get_description(void);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *partition);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
                                      esp_ota_img_states_t *state);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition, esp_app_desc_t *desc);
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t size, esp_ota_handle_t *handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);

#ifdef __cplusplus
}
#endif
