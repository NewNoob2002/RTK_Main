// host 替身：NVS 整数键的内存实现（仅 main_host_check / template_check 链接用）。
// 非线程安全：测试场景按单线程顺序调用，够用；不参与固件。

#include "NvsStore.h"

#include <string.h>

#define SHIM_NVS_MAX_ENTRIES 32
#define SHIM_NVS_MAX_NS      6
#define SHIM_NVS_NAME_MAX    16
#define SHIM_NVS_BLOB_MAX    128

typedef struct {
    char key[SHIM_NVS_NAME_MAX];
    uint32_t value;
    uint8_t blob[SHIM_NVS_BLOB_MAX];
    size_t blob_len;
    int is_blob;
    int used;
} shim_nvs_entry_t;

static char s_ns_names[SHIM_NVS_MAX_NS][SHIM_NVS_NAME_MAX];
static int s_ns_used[SHIM_NVS_MAX_NS];
static shim_nvs_entry_t s_entries[SHIM_NVS_MAX_NS][SHIM_NVS_MAX_ENTRIES];
static esp_err_t s_write_fail = ESP_OK;
/* 按键注入：每条规则对 key 的前 pass 次写入放行，之后返回 err。 */
#define SHIM_NVS_KEY_RULES 2
static struct {
    char key[SHIM_NVS_NAME_MAX];
    esp_err_t err;
    unsigned pass;
} s_key_rules[SHIM_NVS_KEY_RULES];

/** @brief 主机测试注入 NVS 写失败（ESP_OK 恢复正常写入）。 */
void nvs_shim_set_write_fail(esp_err_t err)
{
    s_write_fail = err;
}

static int copy_name(char *out, const char *in);

/** @brief 主机测试清除全部按键注入规则。 */
void nvs_shim_clear_key_fail(void)
{
    memset(s_key_rules, 0, sizeof(s_key_rules));
}

/** @brief 主机测试追加按键注入规则：key 的前 pass_count 次写入成功，之后返回 err；规则槽已满时忽略。 */
void nvs_shim_add_key_fail(const char *key, esp_err_t err, unsigned pass_count)
{
    for (int i = 0; i < SHIM_NVS_KEY_RULES; ++i) {
        if ((s_key_rules[i].err == ESP_OK) && (key != NULL) && (err != ESP_OK) &&
            copy_name(s_key_rules[i].key, key)) {
            s_key_rules[i].err = err;
            s_key_rules[i].pass = pass_count;
            return;
        }
    }
}

/** @brief 命中按键注入时返回错误码，否则 ESP_OK；先放行 pass 次。 */
static esp_err_t key_fail_check(const char *key)
{
    if (key == NULL) {
        return ESP_OK;
    }
    for (int i = 0; i < SHIM_NVS_KEY_RULES; ++i) {
        if ((s_key_rules[i].err == ESP_OK) || (strcmp(key, s_key_rules[i].key) != 0)) {
            continue;
        }
        if (s_key_rules[i].pass > 0U) {
            --s_key_rules[i].pass;
            return ESP_OK;
        }
        return s_key_rules[i].err;
    }
    return ESP_OK;
}

/** @brief 提供启动日志用的主机替身统计。 */
esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *stats)
{
    (void)part_name;
    *stats = (nvs_stats_t){.total_entries = SHIM_NVS_MAX_ENTRIES};
    return ESP_OK;
}

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
    return s_write_fail;
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

/** @brief 删除测试命名空间中的键。 */
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    shim_nvs_entry_t *entry = find_entry(handle, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *entry = (shim_nvs_entry_t){0};
    return ESP_OK;
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
    if (s_write_fail != ESP_OK) {
        return s_write_fail;
    }
    const esp_err_t key_err = key_fail_check(key);
    if (key_err != ESP_OK) {
        return key_err;
    }
    return write_value(handle, key, (uint32_t)value);
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    if (s_write_fail != ESP_OK) {
        return s_write_fail;
    }
    const esp_err_t key_err = key_fail_check(key);
    if (key_err != ESP_OK) {
        return key_err;
    }
    return write_value(handle, key, (uint32_t)value);
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    if (s_write_fail != ESP_OK) {
        return s_write_fail;
    }
    const esp_err_t key_err = key_fail_check(key);
    if (key_err != ESP_OK) {
        return key_err;
    }
    return write_value(handle, key, value);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    if (length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const shim_nvs_entry_t *entry = find_entry(handle, key);
    if ((entry == NULL) || (entry->is_blob == 0)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value == NULL) {
        *length = entry->blob_len;
        return ESP_OK;
    }
    if (*length < entry->blob_len) {
        *length = entry->blob_len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_value, entry->blob, entry->blob_len);
    *length = entry->blob_len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    const int index = ns_index(handle);
    if ((index < 0) || (key == NULL) || (value == NULL) || (length > SHIM_NVS_BLOB_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t key_err = key_fail_check(key);
    if (key_err != ESP_OK) {
        return key_err;
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
    entry->is_blob = 1;
    entry->blob_len = length;
    if (length > 0U) {
        memcpy(entry->blob, value, length);
    }
    return ESP_OK;
}

/** @brief 主机通用键测试仅模拟 cfg 实例；双实例隔离另由 test/nvs_store 验证。 */
esp_err_t nvs_store_open_cfg(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(name, mode, handle);
}

/** @brief 从主机 cfg 替身读定长 PN。 */
esp_err_t nvs_store_get_pn(uint8_t out[NVS_STORE_PN_BYTES])
{
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_store_open_cfg(NVS_STORE_FACTORY_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_OK) {
        size_t len = NVS_STORE_PN_BYTES;
        error = nvs_get_blob(handle, "pn", out, &len);
        if (error == ESP_OK && len != NVS_STORE_PN_BYTES) {
            error = ESP_ERR_INVALID_SIZE;
        }
        nvs_close(handle);
    }
    return error;
}

/** @brief 写定长 PN，保留主机测试的写故障注入。 */
esp_err_t nvs_store_set_pn(const uint8_t pn[NVS_STORE_PN_BYTES])
{
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_store_open_cfg(NVS_STORE_FACTORY_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, "pn", pn, NVS_STORE_PN_BYTES);
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return error;
}
