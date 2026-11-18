// 两个独立的内存 NVS 命名空间，支持 C1 迁移阶段的单次故障注入。
// 仅由 test/nvs_store 单线程调用。
#include "FakeNvs.h"

#include <stdio.h>
#include <string.h>

#include "fw/LogC.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "NvsStore.h"

enum { NVS_FAKE_KEYS = 4, NVS_FAKE_BLOB_BYTES = 64, NVS_FAKE_KEY_BYTES = 16 };
typedef enum { NVS_FAKE_STR, NVS_FAKE_U8, NVS_FAKE_BLOB } nvs_fake_type_t;

typedef struct {
    char key[NVS_FAKE_KEY_BYTES];
    uint8_t value[NVS_FAKE_BLOB_BYTES];
    size_t len;
    nvs_fake_type_t type;
} nvs_fake_entry_t;

typedef struct {
    bool ready;
    bool exists;
    nvs_fake_entry_t entries[NVS_FAKE_KEYS];
} nvs_fake_partition_t;

enum { NVS_FAKE_USER = 1, NVS_FAKE_FACTORY, NVS_FAKE_PARTITIONS };
static nvs_fake_partition_t s_partitions[NVS_FAKE_PARTITIONS];
static fake_nvs_fault_t s_fault;
static bool s_factory_write;
static unsigned s_erases;
static unsigned s_migrations;
static unsigned s_warnings;

/** @brief 设定下一次匹配的模拟故障。 */
void fake_nvs_inject(fake_nvs_fault_t fault)
{
    s_fault = fault;
}

/** @brief 仅在故障点匹配时消费注入。 */
static bool take_fault(fake_nvs_fault_t fault)
{
    if (s_fault != fault) {
        return false;
    }
    s_fault = FAKE_NVS_OK;
    return true;
}

/** @brief 查询现有键或为空槽创建键。 */
static nvs_fake_entry_t *find_entry(nvs_handle_t handle, const char *key, bool create)
{
    if (handle == 0U || handle >= NVS_FAKE_PARTITIONS || strlen(key) >= NVS_FAKE_KEY_BYTES) {
        return NULL;
    }
    nvs_fake_entry_t *empty = NULL;
    for (size_t idx = 0U; idx < NVS_FAKE_KEYS; ++idx) {
        nvs_fake_entry_t *entry = &s_partitions[handle].entries[idx];
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
static esp_err_t save_entry(nvs_handle_t handle, const char *key, nvs_fake_type_t type,
                            const void *value, size_t len)
{
    if (len > NVS_FAKE_BLOB_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_fake_entry_t *entry = find_entry(handle, key, true);
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(entry->value, value, len);
    entry->type = type;
    entry->len = len;
    return ESP_OK;
}

/** @brief 查询长度或复制指定类型的值。 */
static esp_err_t read_entry(nvs_handle_t handle, const char *key, nvs_fake_type_t type, void *out,
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
    }
    *len = entry->len;
    return ESP_OK;
}

/** @brief 预填出厂或用户分区的 SN。 */
void fake_nvs_seed(bool factory, const char *sn)
{
    const nvs_handle_t handle = factory ? NVS_FAKE_FACTORY : NVS_FAKE_USER;
    s_partitions[handle].exists = true;
    const esp_err_t result = save_entry(handle, "sn", NVS_FAKE_STR, sn, strlen(sn) + 1U);
    if (result != ESP_OK) {
        (void)fprintf(stderr, "Could not seed fake NVS\n");
    }
}

/** @brief 直接观察持久化值，NULL 表示应当缺失。 */
bool fake_nvs_has_sn(bool factory, const char *sn)
{
    const nvs_handle_t handle = factory ? NVS_FAKE_FACTORY : NVS_FAKE_USER;
    const nvs_fake_entry_t *entry = find_entry(handle, "sn", false);
    if (sn == NULL) {
        return entry == NULL;
    }
    return entry != NULL && entry->len == strlen(sn) + 1U &&
           memcmp(entry->value, sn, entry->len) == 0;
}

/** @brief 查询用户分区擦除次数。 */
unsigned fake_nvs_user_erases(void)
{
    return s_erases;
}

/** @brief 查询迁移成功的日志次数。 */
unsigned fake_nvs_migrations(void)
{
    return s_migrations;
}

/** @brief 查询迁移告警次数。 */
unsigned fake_nvs_warnings(void)
{
    return s_warnings;
}

esp_err_t nvs_flash_init(void)
{
    if (take_fault(FAKE_NVS_USER_INIT)) {
        return ESP_ERR_NVS_NO_FREE_PAGES;
    }
    s_partitions[NVS_FAKE_USER].ready = true;
    return ESP_OK;
}

esp_err_t nvs_flash_init_partition(const char *partition)
{
    if (strcmp(partition, NVS_STORE_FACTORY_PARTITION) != 0) {
        return ESP_ERR_NVS_PART_NOT_FOUND;
    }
    if (take_fault(FAKE_NVS_FACTORY_INIT)) {
        s_partitions[NVS_FAKE_FACTORY].ready = false;
        return ESP_ERR_NVS_NEW_VERSION_FOUND;
    }
    s_partitions[NVS_FAKE_FACTORY].ready = true;
    s_factory_write = false;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    ++s_erases;
    s_partitions[NVS_FAKE_USER] = (nvs_fake_partition_t){0};
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char *partition, const char *name, nvs_open_mode_t mode,
                                  nvs_handle_t *out)
{
    const bool factory = strcmp(partition, NVS_STORE_FACTORY_PARTITION) == 0;
    const nvs_handle_t handle = factory ? NVS_FAKE_FACTORY : NVS_FAKE_USER;
    if ((!factory && strcmp(partition, "nvs") != 0) ||
        strcmp(name, factory ? NVS_STORE_FACTORY_NAMESPACE : "rtk") != 0) {
        return ESP_ERR_NVS_PART_NOT_FOUND;
    }
    if (!s_partitions[handle].ready) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    if (mode == NVS_READONLY && !s_partitions[handle].exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_partitions[handle].exists = true;
    *out = handle;
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
    if (handle == NVS_FAKE_FACTORY && s_factory_write && take_fault(FAKE_NVS_COMMIT)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *len)
{
    if (take_fault(FAKE_NVS_DIRTY_READ)) {
        out[0] = 'X';
        return ESP_FAIL;
    }
    const esp_err_t result = read_entry(handle, key, NVS_FAKE_STR, out, len);
    if (result == ESP_OK && handle == NVS_FAKE_FACTORY && s_factory_write &&
        take_fault(FAKE_NVS_MISMATCH)) {
        out[0] = 'X';
    }
    return result;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    if (handle == NVS_FAKE_FACTORY) {
        if (take_fault(FAKE_NVS_WRITE)) {
            return ESP_FAIL;
        }
        s_factory_write = true;
    }
    return save_entry(handle, key, NVS_FAKE_STR, value, strlen(value) + 1U);
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    size_t len = sizeof(*out);
    return read_entry(handle, key, NVS_FAKE_U8, out, &len);
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    return save_entry(handle, key, NVS_FAKE_U8, &value, sizeof(value));
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    return read_entry(handle, key, NVS_FAKE_BLOB, out, len);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t len)
{
    return save_entry(handle, key, NVS_FAKE_BLOB, value, len);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    if (handle == NVS_FAKE_USER && strcmp(key, "sn") == 0 && take_fault(FAKE_NVS_DELETE)) {
        return ESP_FAIL;
    }
    nvs_fake_entry_t *entry = find_entry(handle, key, false);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *entry = (nvs_fake_entry_t){0};
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    return (err == ESP_OK) ? "ESP_OK" : "ESP_ERR_TEST";
}

void fw_log_c(int level, int cat, const char *fmt, ...)
{
    if (cat == FW_LOG_CAT_STORE && level == FW_LOG_LEVEL_INFO &&
        strcmp(fmt, "SN migrated to factory partition") == 0) {
        ++s_migrations;
    }
    if (cat == FW_LOG_CAT_STORE && level == FW_LOG_LEVEL_WARN) {
        ++s_warnings;
    }
}
