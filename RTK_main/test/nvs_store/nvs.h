// 主机替身的 NVS 子集，与组件使用的 IDF 签名一致。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_NVS_NOT_INITIALIZED   0x1101
#define ESP_ERR_NVS_TYPE_MISMATCH     0x1103
#define ESP_ERR_NVS_INVALID_LENGTH    0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES     0x110d
#define ESP_ERR_NVS_PART_NOT_FOUND    0x110f
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110

typedef uint32_t nvs_handle_t;

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_open_from_partition(const char *partition, const char *name, nvs_open_mode_t mode,
                                  nvs_handle_t *out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t len);
