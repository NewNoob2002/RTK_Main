// host 替身：NVS 整数键的内存实现（仅 main_host_check / template_check 链接用）。
// 非线程安全：测试场景按单线程顺序调用，够用；不参与固件。

#include "nvs.h"

#include <string.h>

#define SHIM_NVS_MAX_ENTRIES 16
#define SHIM_NVS_MAX_NS      4
#define SHIM_NVS_NAME_MAX    16

typedef struct {
    char key[SHIM_NVS_NAME_MAX];
    uint32_t value;
    int used;
} shim_nvs_entry_t;

static char s_ns_names[SHIM_NVS_MAX_NS][SHIM_NVS_NAME_MAX];
static int s_ns_used[SHIM_NVS_MAX_NS];
static shim_nvs_entry_t s_entries[SHIM_NVS_MAX_NS][SHIM_NVS_MAX_ENTRIES];

static int copy_name(char *out, const char *in)
{
    if ((in == NULL) || (strlen(in) >= SHIM_NVS_NAME_MAX)) {
        return 0;
    }
    (void)strcpy(out, in);
    return 1;
}

/* handle 是命名空间槽位 + 1；0 表示无效。 */
static int ns_index(nvs_handle_t handle)
{
    if ((handle == 0U) || (handle > SHIM_NVS_MAX_NS)) {
        return -1;
    }
    return (int)(handle - 1U);
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    /* 替身不区分读写：命名空间按需创建（真实 NVS 在首写时创建）。 */
    (void)open_mode;
    if ((name == NULL) || (out_handle == NULL) || (strlen(name) >= SHIM_NVS_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < SHIM_NVS_MAX_NS; ++i) {
        if (s_ns_used[i] != 0) {
            if (strcmp(s_ns_names[i], name) == 0) {
                *out_handle = (nvs_handle_t)(i + 1);
                return ESP_OK;
            }
            continue;
        }
        if (!copy_name(s_ns_names[i], name)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_ns_used[i] = 1;
        *out_handle = (nvs_handle_t)(i + 1);
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

static shim_nvs_entry_t *find_entry(nvs_handle_t handle, const char *key)
{
    const int index = ns_index(handle);
    if ((index < 0) || (key == NULL)) {
        return NULL;
    }
    for (int i = 0; i < SHIM_NVS_MAX_ENTRIES; ++i) {
        shim_nvs_entry_t *entry = &s_entries[index][i];
        if ((entry->used != 0) && (strcmp(entry->key, key) == 0)) {
            return entry;
        }
    }
    return NULL;
}

static esp_err_t read_value(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    const shim_nvs_entry_t *entry = find_entry(handle, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = entry->value;
    return ESP_OK;
}

static esp_err_t write_value(nvs_handle_t handle, const char *key, uint32_t value)
{
    const int index = ns_index(handle);
    if ((index < 0) || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    shim_nvs_entry_t *entry = find_entry(handle, key);
    if (entry == NULL) {
        for (int i = 0; i < SHIM_NVS_MAX_ENTRIES; ++i) {
            if (s_entries[index][i].used == 0) {
                entry = &s_entries[index][i];
                if (!copy_name(entry->key, key)) {
                    return ESP_ERR_INVALID_ARG;
                }
                entry->used = 1;
                break;
            }
        }
    }
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }
    entry->value = value;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    uint32_t value = 0U;
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = read_value(handle, key, &value);
    if (error == ESP_OK) {
        *out_value = (uint8_t)value;
    }
    return error;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value)
{
    uint32_t value = 0U;
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = read_value(handle, key, &value);
    if (error == ESP_OK) {
        *out_value = (uint16_t)value;
    }
    return error;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_value(handle, key, out_value);
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    return write_value(handle, key, (uint32_t)value);
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    return write_value(handle, key, (uint32_t)value);
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    return write_value(handle, key, value);
}
