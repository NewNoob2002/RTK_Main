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

typedef struct {
    size_t used_entries;
    size_t total_entries;
} nvs_stats_t;

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *stats);
esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);

/* 主机测试故障注入：非 ESP_OK 时使后续 NVS 写入/提交返回该错误。 */
void nvs_shim_set_write_fail(esp_err_t err);
/** @brief 追加按键注入写失败规则（最多 2 条）：key 的前 pass_count 次写入成功，之后返回 err。 */
void nvs_shim_add_key_fail(const char *key, esp_err_t err, unsigned pass_count);
/** @brief 清除全部按键注入规则。 */
void nvs_shim_clear_key_fail(void);
