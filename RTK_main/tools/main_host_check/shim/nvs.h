// host 替身：NVS 的最小可用子集，只实现 storage 模块通用键表用到的整数读写。
// 仅 main_host_check 编译用（内存表，进程退出即丢），不参与固件。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 纯 C 头：C++ 调用方在 include 处自行加 extern "C"（避免用保留标识符宏做守卫）。 */

/* 与 IDF 的 esp_err.h 同值；替身 esp_err.h 只在需要时提供。 */
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
