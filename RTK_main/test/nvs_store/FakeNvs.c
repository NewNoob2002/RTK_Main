// 两个独立内存 NVS 实例，支持命名空间及一次性故障注入。
// 仅由 test/nvs_store 单线程调用。
#include "FakeNvs.h"

#include <string.h>

#include "fw/LogC.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "NvsStore.h"

enum { NVS_FAKE_KEYS = 6, NVS_FAKE_BLOB_BYTES = 2561, NVS_FAKE_KEY_BYTES = 16 };

typedef struct {
    char key[NVS_FAKE_KEY_BYTES];
    uint8_t value[NVS_FAKE_BLOB_BYTES];
    size_t len;
    nvs_type_t type;
} nvs_fake_entry_t;

enum { NVS_FAKE_NAMESPACES = 16 };
typedef struct {
    char name[NVS_FAKE_KEY_BYTES];
    nvs_fake_entry_t entries[NVS_FAKE_KEYS];
} nvs_fake_namespace_t;

typedef struct {
    bool ready;
    esp_err_t init_error;
    esp_err_t write_error;
    unsigned erases;
    nvs_fake_namespace_t spaces[NVS_FAKE_NAMESPACES];
} nvs_fake_partition_t;

static nvs_fake_partition_t s_partitions[2];
static const esp_partition_t k_cfg = {.size = 0x20000U};
static bool s_fail_delete;
static const char *s_fail_key_write;
static const char *s_fail_key_commit;
static const char *s_corrupt_key_read;
static nvs_handle_t s_failed_commit_handle;
static unsigned s_writes;
static unsigned s_warnings;
static unsigned s_errors;
static unsigned s_eeprom_loss_logs;

/** @brief 模拟保留 Flash 内容的冷启动，并为指定分区注入一次初始化错误。 */
void fake_nvs_fail_init(bool cfg, esp_err_t error)
{
    s_partitions[false].ready = false;
    s_partitions[true].ready = false;
    s_partitions[cfg].init_error = error;
}

/** @brief 为指定分区注入一次 set 错误。 */
void fake_nvs_fail_write(bool cfg, esp_err_t error)
{
    s_partitions[cfg].write_error = error;
}

/** @brief 下次 set 指定键时失败，键名的生命周期由测试保持。 @param key: 键名。 */
void fake_nvs_fail_key_write(const char *key)
{
    s_fail_key_write = key;
}

/** @brief 下次 set 指定键后的 commit 失败一次。 @param key: 键名。 */
void fake_nvs_fail_key_commit(const char *key)
{
    s_fail_key_commit = key;
}

/** @brief 下次读回指定键时翻转返回值首字节，不改存储值。 @param key: 键名。 */
void fake_nvs_corrupt_key_read(const char *key)
{
    s_corrupt_key_read = key;
}

/** @brief 为下次键删除注入错误。 */
void fake_nvs_fail_delete(void)
{
    s_fail_delete = true;
}

/** @brief 将句柄映射为所属实例。 */
static bool is_cfg(nvs_handle_t handle)
{
    return handle > NVS_FAKE_NAMESPACES;
}

/** @brief 句柄从 1 开始，先系统实例，再 cfg 实例。 */
static nvs_fake_namespace_t *find_namespace(nvs_handle_t handle)
{
    if (handle == 0U || handle > 2U * NVS_FAKE_NAMESPACES) {
        return NULL;
    }
    return &s_partitions[is_cfg(handle)].spaces[(handle - 1U) % NVS_FAKE_NAMESPACES];
}

/** @brief 查询现有键或为空槽创建键。 */
static nvs_fake_entry_t *find_entry(nvs_handle_t handle, const char *key, bool create)
{
    nvs_fake_namespace_t *space = find_namespace(handle);
    if (space == NULL || strlen(key) >= NVS_FAKE_KEY_BYTES) {
        return NULL;
    }
    nvs_fake_entry_t *empty = NULL;
    for (size_t idx = 0U; idx < NVS_FAKE_KEYS; ++idx) {
        nvs_fake_entry_t *entry = &space->entries[idx];
        if (strcmp(entry->key, key) == 0) {
            return entry;
        }
        if (entry->key[0] == '\0' && empty == NULL) {
            empty = entry;
        }
    }
    if (create && empty != NULL) {
        memcpy(empty->key, key, strlen(key) + 1U);
        return empty;
    }
    return NULL;
}

/** @brief 保存指定类型的字节值。 */
static esp_err_t save_entry(nvs_handle_t handle, const char *key, nvs_type_t type,
                            const void *value, size_t len)
{
    if (len > NVS_FAKE_BLOB_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t fault = s_partitions[is_cfg(handle)].write_error;
    s_partitions[is_cfg(handle)].write_error = ESP_OK;
    if (fault != ESP_OK) {
        return fault;
    }
    if (s_fail_key_write != NULL && strcmp(s_fail_key_write, key) == 0) {
        s_fail_key_write = NULL;
        return ESP_FAIL;
    }
    nvs_fake_entry_t *entry = find_entry(handle, key, true);
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(entry->value, value, len);
    entry->type = type;
    entry->len = len;
    ++s_writes;
    if (s_fail_key_commit != NULL && strcmp(s_fail_key_commit, key) == 0) {
        s_fail_key_commit = NULL;
        s_failed_commit_handle = handle;
    }
    return ESP_OK;
}

/** @brief 查询长度或复制指定类型的值。 */
static esp_err_t read_entry(nvs_handle_t handle, const char *key, nvs_type_t type, void *out,
                            size_t *len)
{
    const nvs_fake_entry_t *entry = find_entry(handle, key, false);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->type != type) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (out != NULL && *len < entry->len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (out != NULL) {
        memcpy(out, entry->value, entry->len);
        if (entry->len != 0U && s_corrupt_key_read != NULL &&
            strcmp(s_corrupt_key_read, key) == 0) {
            s_corrupt_key_read = NULL;
            uint8_t *bytes = out;
            bytes[0] ^= 1U;
        }
    }
    *len = entry->len;
    return ESP_OK;
}

/** @brief 指定实例整片擦除次数。 */
unsigned fake_nvs_erases(bool cfg)
{
    return s_partitions[cfg].erases;
}

/** @brief 统计成功写入次数以检查幂等。 */
unsigned fake_nvs_writes(void)
{
    return s_writes;
}

/** @brief 统计双副本不一致等警告日志。 */
unsigned fake_nvs_warnings(void)
{
    return s_warnings;
}

/** @brief 统计擦除与修复错误日志。 */
unsigned fake_nvs_errors(void)
{
    return s_errors;
}

/** @brief 统计明确报告 EEPROM 丢失和跳过 C7 迁移的错误日志。 */
unsigned fake_nvs_eeprom_loss_logs(void)
{
    return s_eeprom_loss_logs;
}

/** @brief 初始化内存分区，消费一次性错误。 */
static esp_err_t init(bool cfg)
{
    nvs_fake_partition_t *part = &s_partitions[cfg];
    const esp_err_t result = part->init_error;
    part->init_error = ESP_OK;
    /* IDF 冷启动时只登记初始化成功的实例。 */
    part->ready = result == ESP_OK;
    return result;
}

esp_err_t nvs_flash_init(void)
{
    return init(false);
}

esp_err_t nvs_flash_init_partition_ptr(const esp_partition_t *partition)
{
    return partition == &k_cfg ? init(true) : ESP_ERR_INVALID_ARG;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    return type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS &&
                   strcmp(label, NVS_STORE_CFG_PARTITION) == 0
               ? &k_cfg
               : NULL;
}

/** @brief 擦除指定实例，保留擦除计数。 */
static void erase(bool cfg)
{
    nvs_fake_partition_t *part = &s_partitions[cfg];
    memset(part->spaces, 0, sizeof(part->spaces));
    part->ready = false;
    ++part->erases;
}

esp_err_t nvs_flash_erase(void)
{
    erase(false);
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t len)
{
    if (part != &k_cfg || offset != 0U || len != k_cfg.size) {
        return ESP_ERR_INVALID_ARG;
    }
    erase(true);
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char *partition, const char *name, nvs_open_mode_t mode,
                                  nvs_handle_t *out)
{
    const bool cfg = strcmp(partition, NVS_STORE_CFG_PARTITION) == 0;
    if (!cfg && strcmp(partition, NVS_STORE_SYSTEM_PARTITION) != 0) {
        return ESP_ERR_NVS_PART_NOT_FOUND;
    }
    nvs_fake_partition_t *part = &s_partitions[cfg];
    if (!part->ready) {
        return s_partitions[!cfg].ready ? ESP_ERR_NVS_PART_NOT_FOUND : ESP_ERR_NVS_NOT_INITIALIZED;
    }
    size_t free_slot = NVS_FAKE_NAMESPACES;
    for (size_t i = 0; i < NVS_FAKE_NAMESPACES; ++i) {
        if (strcmp(part->spaces[i].name, name) == 0) {
            *out = (nvs_handle_t)(i + 1U + (cfg ? NVS_FAKE_NAMESPACES : 0U));
            return ESP_OK;
        }
        if (part->spaces[i].name[0] == '\0' && free_slot == NVS_FAKE_NAMESPACES) {
            free_slot = i;
        }
    }
    if (mode == NVS_READONLY) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (free_slot == NVS_FAKE_NAMESPACES || strlen(name) >= NVS_FAKE_KEY_BYTES) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(part->spaces[free_slot].name, name, strlen(name) + 1U);
    *out = (nvs_handle_t)(free_slot + 1U + (cfg ? NVS_FAKE_NAMESPACES : 0U));
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open_from_partition("nvs", name, mode, out);
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    if (handle != 0U && handle == s_failed_commit_handle) {
        s_failed_commit_handle = 0U;
        return ESP_FAIL;
    }
    return find_namespace(handle) != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t nvs_find_key(nvs_handle_t handle, const char *key, nvs_type_t *out_type)
{
    const nvs_fake_entry_t *entry = find_entry(handle, key, false);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_type != NULL) {
        *out_type = entry->type;
    }
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *len)
{
    return read_entry(handle, key, NVS_TYPE_STR, out, len);
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    return save_entry(handle, key, NVS_TYPE_STR, value, strlen(value) + 1U);
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    size_t len = sizeof(*out);
    return read_entry(handle, key, NVS_TYPE_U8, out, &len);
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    return save_entry(handle, key, NVS_TYPE_U8, &value, sizeof(value));
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out)
{
    size_t len = sizeof(*out);
    return read_entry(handle, key, NVS_TYPE_U16, out, &len);
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    return save_entry(handle, key, NVS_TYPE_U16, &value, sizeof(value));
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    size_t len = sizeof(*out);
    return read_entry(handle, key, NVS_TYPE_U32, out, &len);
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    return save_entry(handle, key, NVS_TYPE_U32, &value, sizeof(value));
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    return read_entry(handle, key, NVS_TYPE_BLOB, out, len);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t len)
{
    return save_entry(handle, key, NVS_TYPE_BLOB, value, len);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    if (s_fail_delete) {
        s_fail_delete = false;
        return ESP_FAIL;
    }
    nvs_fake_entry_t *entry = find_entry(handle, key, false);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *entry = (nvs_fake_entry_t){0};
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    nvs_fake_namespace_t *space = find_namespace(handle);
    if (space == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(space->entries, 0, sizeof(space->entries));
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    return (err == ESP_OK) ? "ESP_OK" : "ESP_ERR_TEST";
}

void fw_log_c(int level, int cat, const char *fmt, ...)
{
    if (cat == FW_LOG_CAT_STORE && level == FW_LOG_LEVEL_WARN) {
        ++s_warnings;
    }
    if (cat == FW_LOG_CAT_STORE && level == FW_LOG_LEVEL_ERROR) {
        ++s_errors;
        if (strstr(fmt, "Legacy EEPROM lost") != NULL &&
            strstr(fmt, "C7 migration will be skipped") != NULL) {
            ++s_eeprom_loss_logs;
        }
    }
}
