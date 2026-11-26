// ADR-0022 双实例 NVS、身份副本与报警器白名单。
// 启动与存储任务串行修改，读取可由普通任务调用。NVS 调用会阻塞，非 ISR。

#include <string.h>

#include "esp_partition.h"
#include "fw/LogC.h"
#include "nvs_flash.h"

#include "NvsStore.h"

#define NVS_STORE_NS       "rtk"
#define NVS_STORE_KEY_SN   "sn"
#define NVS_STORE_KEY_PN   "pn"
#define NVS_STORE_KEY_ALM  "alarm_wl"
#define NVS_STORE_EEPROM   "eeprom"
#define NVS_STORE_ID_BYTES (NVS_STORE_SN_MAX + 1U)

static nvs_store_boot_scratch_fn s_boot_scratch;

void nvs_store_set_boot_scratch(nvs_store_boot_scratch_fn with_scratch)
{
    s_boot_scratch = with_scratch;
}

/** @brief 判断 NVS 初始化错误是否须擦除重试。
 * @param error: 初始化结果。 @retval true 包含不兼容版本或无空闲页。 */
static bool nvs_store_needs_erase(esp_err_t error)
{
    return error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND;
}

esp_err_t nvs_store_open_cfg(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open_from_partition(NVS_STORE_CFG_PARTITION, name, mode, handle);
}

/** @brief 验证 SN 或定长 PN 的有效内容。
 * @param sn: true 表示 SN。 @param value: 字段。 @param len: 字段长度。
 * @retval true 格式合法。 @note 纯函数，可在 ISR 中使用。 */
static bool nvs_store_valid_id(bool sn, const void *value, size_t len)
{
    if (sn) {
        const char *text = value;
        return len > 1U && len <= NVS_STORE_ID_BYTES && text[len - 1U] == '\0' &&
               strlen(text) == len - 1U && strpbrk(text, "\r\n") == NULL;
    }
    if (len != NVS_STORE_PN_BYTES) {
        return false;
    }
    const uint8_t *bytes = value;
    for (size_t i = 0; i < len && bytes[i] != 0U; ++i) {
        if (bytes[i] < ' ' || bytes[i] > '~') {
            return false;
        }
    }
    return true;
}

/** @brief 读取并验证一个身份字段。
 * @param partition: 实例标签。 @param ns: 命名空间。 @param sn: true 读 SN，false 读 PN。
 * @param[out] value: 读取缓冲。 @param[in,out] len: 容量，成功时为实际长度。
 * @retval ESP_OK 有效；其他为读取或格式错误。 @note 可阻塞，非 ISR。 */
static esp_err_t nvs_store_read_id(const char *partition, const char *ns, bool sn, void *value,
                                   size_t *len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(partition, ns, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = sn ? nvs_get_str(handle, NVS_STORE_KEY_SN, value, len)
                 : nvs_get_blob(handle, NVS_STORE_KEY_PN, value, len);
        nvs_close(handle);
        if (err == ESP_ERR_NVS_INVALID_LENGTH ||
            (err == ESP_OK && !nvs_store_valid_id(sn, value, *len))) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    return err;
}

/** @brief 写入身份字段或删除原本不存在的字段，然后提交。
 * @param system: 选择系统备份或 cfg。 @param sn: true 为 SN，false 为 PN。
 * @param value: 有效字段；NULL 表示删除。
 * @retval NVS 操作结果。 @note 可阻塞，非 ISR。 */
static esp_err_t nvs_store_write_id(bool system, bool sn, const void *value)
{
    const char *partition = system ? NVS_STORE_SYSTEM_PARTITION : NVS_STORE_CFG_PARTITION;
    const char *ns = system ? NVS_STORE_BACKUP_NAMESPACE : NVS_STORE_FACTORY_NAMESPACE;
    const char *key = sn ? NVS_STORE_KEY_SN : NVS_STORE_KEY_PN;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(partition, ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (value == NULL) {
        err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = sn ? nvs_set_str(handle, key, value)
                 : nvs_set_blob(handle, key, value, NVS_STORE_PN_BYTES);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** @brief 判断身份字段是否缺失或存储格式损坏。
 * @param err: 身份字段读取结果。 @retval true 可用有效副本替换。 */
static bool nvs_store_replaceable(esp_err_t err)
{
    return err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH ||
           err == ESP_ERR_INVALID_SIZE;
}

/** @brief 先写备份；主副本提交失败时尝试恢复原备份。
 * @param sn: true 写 SN，false 写 PN。 @param value: 已验证字段。
 * @retval 两处提交完成为 ESP_OK；其他为读取或写入错误。 @note 存储任务串行调用，非 ISR。 */
static esp_err_t nvs_store_write_both(bool sn, const void *value)
{
    uint8_t previous[NVS_STORE_ID_BYTES];
    size_t len = sizeof(previous);
    const esp_err_t read = nvs_store_read_id(NVS_STORE_SYSTEM_PARTITION, NVS_STORE_BACKUP_NAMESPACE,
                                             sn, previous, &len);
    if (read != ESP_OK && !nvs_store_replaceable(read)) {
        return read;
    }
    esp_err_t err = nvs_store_write_id(true, sn, value);
    if (err == ESP_OK) {
        err = nvs_store_write_id(false, sn, value);
        if (err != ESP_OK) {
            const esp_err_t restore =
                nvs_store_write_id(true, sn, read == ESP_OK ? previous : NULL);
            if (restore != ESP_OK) {
                FW_CLOGE(FW_LOG_CAT_STORE, "Identity backup restore failed: %s",
                         esp_err_to_name(restore));
            }
        }
    }
    return err;
}

/** @brief 删除已由双副本接管的旧 PN。
 * @retval ESP_OK 已删除或不存在；其他为 NVS 错误。 @note 仅启动期，可阻塞，非 ISR。 */
static esp_err_t nvs_store_remove_legacy_pn(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_store_open_cfg(NVS_STORE_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, NVS_STORE_KEY_PN);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return err;
}

/** @brief 修复一个身份字段；两处不同时以 cfg 为准，旧 PN 只补缺。
 * @param sn: true 修复 SN，false 修复 PN。 @note 仅启动期，可阻塞，非 ISR。 */
static void nvs_store_reconcile_id(bool sn)
{
    uint8_t cfg[NVS_STORE_ID_BYTES];
    uint8_t backup[NVS_STORE_ID_BYTES];
    uint8_t legacy[NVS_STORE_PN_BYTES];
    size_t cfg_len = sizeof(cfg);
    size_t backup_len = sizeof(backup);
    size_t legacy_len = sizeof(legacy);
    const esp_err_t cfg_err =
        nvs_store_read_id(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE, sn, cfg, &cfg_len);
    const esp_err_t bk_err = nvs_store_read_id(NVS_STORE_SYSTEM_PARTITION,
                                               NVS_STORE_BACKUP_NAMESPACE, sn, backup, &backup_len);
    const esp_err_t legacy_err =
        sn ? ESP_ERR_NVS_NOT_FOUND
           : nvs_store_read_id(NVS_STORE_CFG_PARTITION, NVS_STORE_NS, false, legacy, &legacy_len);
    esp_err_t err = ESP_OK;
    if (cfg_err != ESP_OK && !nvs_store_replaceable(cfg_err)) {
        err = cfg_err;
    } else if (bk_err != ESP_OK && !nvs_store_replaceable(bk_err)) {
        err = bk_err;
    } else if (cfg_err == ESP_OK) {
        if (bk_err == ESP_OK && (cfg_len != backup_len || memcmp(cfg, backup, cfg_len) != 0)) {
            FW_CLOGW(FW_LOG_CAT_STORE, "Identity %s differs across NVS instances, using cfg",
                     sn ? NVS_STORE_KEY_SN : NVS_STORE_KEY_PN);
            err = nvs_store_write_id(true, sn, cfg);
        } else if (bk_err != ESP_OK) {
            err = nvs_store_write_id(true, sn, cfg);
        }
    } else if (bk_err == ESP_OK) {
        err = nvs_store_write_id(false, sn, backup);
    } else if (legacy_err == ESP_OK) {
        err = nvs_store_write_both(false, legacy);
    }

    if (err == ESP_OK && legacy_err == ESP_OK) {
        err = nvs_store_remove_legacy_pn();
    }
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_STORE, "Identity %s repair failed: %s",
                 sn ? NVS_STORE_KEY_SN : NVS_STORE_KEY_PN, esp_err_to_name(err));
    }
}

/** @brief 保护系统擦除前还能只读访问的数据。
 * @param[out] blob: 启动期临时缓冲。 @param[in,out] len: 容量，成功时为 blob 长度。
 * @retval 读取结果。 @note 可阻塞，非 ISR。 */
static esp_err_t nvs_store_read_eeprom(uint8_t *blob, size_t *len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_STORE_EEPROM, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, NVS_STORE_EEPROM, blob, len);
        nvs_close(handle);
    }
    return err;
}

/** @brief 将系统擦除前读到的 EEPROM blob 按原始字节写回。
 * @param blob: 已读到的原始字节。 @param len: 实际长度。 @note 可阻塞，非 ISR。 */
static void nvs_store_restore_eeprom(const uint8_t *blob, size_t len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_STORE_EEPROM, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_STORE_EEPROM, blob, len);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_STORE, "System NVS EEPROM restore failed: %s", esp_err_to_name(err));
    }
}

/** @brief 仅擦除系统实例，尽力备份与恢复 EEPROM 和身份备份。
 * @param[out] blob: 恢复期间借用的缓冲；无可用内存时为 NULL。
 * @param capacity: 缓冲容量，最大 NVS_STORE_EEPROM_MAX。
 * @retval 系统擦除或重新初始化结果。 @note 仅启动期，可阻塞，非 ISR。 */
static esp_err_t nvs_store_recover_system(uint8_t *blob, size_t capacity)
{
    uint8_t sn[NVS_STORE_ID_BYTES];
    uint8_t pn[NVS_STORE_PN_BYTES];
    size_t blob_len = capacity;
    size_t sn_len = sizeof(sn);
    size_t pn_len = sizeof(pn);
    const esp_err_t saved_blob =
        blob != NULL ? nvs_store_read_eeprom(blob, &blob_len) : ESP_ERR_NO_MEM;
    const esp_err_t saved_sn = nvs_store_read_id(NVS_STORE_SYSTEM_PARTITION,
                                                 NVS_STORE_BACKUP_NAMESPACE, true, sn, &sn_len);
    const esp_err_t saved_pn = nvs_store_read_id(NVS_STORE_SYSTEM_PARTITION,
                                                 NVS_STORE_BACKUP_NAMESPACE, false, pn, &pn_len);
    FW_CLOGE(FW_LOG_CAT_STORE, "Erasing system NVS; backup eeprom=%s sn=%s pn=%s",
             esp_err_to_name(saved_blob), esp_err_to_name(saved_sn), esp_err_to_name(saved_pn));
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        if (saved_blob != ESP_OK) {
            FW_CLOGE(
                FW_LOG_CAT_STORE,
                "Legacy EEPROM lost after system NVS erase (%s). C7 migration will be skipped.",
                esp_err_to_name(saved_blob));
        }
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        if (saved_blob == ESP_OK) {
            nvs_store_restore_eeprom(blob, blob_len);
        }
        if (saved_sn == ESP_OK) {
            const esp_err_t restore = nvs_store_write_id(true, true, sn);
            if (restore != ESP_OK) {
                FW_CLOGE(FW_LOG_CAT_STORE, "System NVS SN restore failed: %s",
                         esp_err_to_name(restore));
            }
        }
        if (saved_pn == ESP_OK) {
            const esp_err_t restore = nvs_store_write_id(true, false, pn);
            if (restore != ESP_OK) {
                FW_CLOGE(FW_LOG_CAT_STORE, "System NVS PN restore failed: %s",
                         esp_err_to_name(restore));
            }
        }
    }
    return err;
}

/** @brief 初始化 cfg 并在布局错误时仅擦除 cfg。
 * @retval cfg 初始化或擦除结果。 @note 仅启动期，可阻塞，非 ISR。 */
static esp_err_t nvs_store_init_cfg(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NVS_STORE_CFG_PARTITION);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = nvs_flash_init_partition_ptr(part);
    if (nvs_store_needs_erase(err)) {
        FW_CLOGE(FW_LOG_CAT_STORE, "Erasing cfg NVS after init failed: %s", esp_err_to_name(err));
        err = esp_partition_erase_range(part, 0, part->size);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition_ptr(part);
        }
    }
    return err;
}

esp_err_t nvs_store_init(void)
{
    const nvs_store_boot_scratch_fn with_scratch = s_boot_scratch;
    s_boot_scratch = NULL;
    esp_err_t system = nvs_flash_init();
    if (nvs_store_needs_erase(system)) {
        system = with_scratch != NULL ? with_scratch(nvs_store_recover_system)
                                      : nvs_store_recover_system(NULL, 0U);
    }
    const esp_err_t cfg = nvs_store_init_cfg();
    if (system != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_STORE, "System NVS unavailable: %s", esp_err_to_name(system));
    }
    if (cfg != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_STORE, "cfg NVS unavailable: %s", esp_err_to_name(cfg));
    }
    if (system == ESP_OK && cfg == ESP_OK) {
        nvs_store_reconcile_id(true);
        nvs_store_reconcile_id(false);
    }
    return cfg;
}

/** @brief 读取首个有效身份副本，优先 cfg。
 * @param sn: true 读 SN，false 读 PN。 @param[out] value: 字段缓冲。 @param capacity: 容量。
 * @retval ESP_OK 已读取；其他为 NVS 或格式错误。 @note 可阻塞，非 ISR。 */
static esp_err_t nvs_store_get_id(bool sn, void *value, size_t capacity)
{
    size_t len = capacity;
    const esp_err_t cfg =
        nvs_store_read_id(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE, sn, value, &len);
    if (cfg == ESP_OK) {
        return ESP_OK;
    }
    len = capacity;
    const esp_err_t backup =
        nvs_store_read_id(NVS_STORE_SYSTEM_PARTITION, NVS_STORE_BACKUP_NAMESPACE, sn, value, &len);
    return backup == ESP_OK || cfg == ESP_ERR_NVS_NOT_FOUND ? backup : cfg;
}

esp_err_t nvs_store_get_sn(char *out, size_t out_size)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (out != NULL && out_size != 0U) {
        if (out_size >= NVS_STORE_ID_BYTES) {
            err = nvs_store_get_id(true, out, out_size);
        }
        if (err != ESP_OK) {
            out[0] = '\0';
        }
    }
    return err;
}

esp_err_t nvs_store_set_sn(const char *sn)
{
    if (sn == NULL || !nvs_store_valid_id(true, sn, strlen(sn) + 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_store_write_both(true, sn);
}

esp_err_t nvs_store_get_pn(uint8_t out[NVS_STORE_PN_BYTES])
{
    return out == NULL ? ESP_ERR_INVALID_ARG : nvs_store_get_id(false, out, NVS_STORE_PN_BYTES);
}

esp_err_t nvs_store_set_pn(const uint8_t pn[NVS_STORE_PN_BYTES])
{
    if (pn == NULL || !nvs_store_valid_id(false, pn, NVS_STORE_PN_BYTES)) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_store_write_both(false, pn);
}

/** @brief 清空已有命名空间并提交，缺失视为已清空。
 * @param partition: 分区标签。 @param ns: 命名空间。
 * @retval NVS 操作结果。 @note 仅调用方已停止设置写入时执行，可阻塞，非 ISR。 */
static esp_err_t nvs_store_clear_namespace(const char *partition, const char *ns)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(partition, ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_close(handle);
    err = nvs_open_from_partition(partition, ns, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return err;
}

esp_err_t nvs_store_erase_user(void)
{
    const char *const namespaces[] = {"gnss",
                                      NVS_STORE_NS,
                                      "link",
                                      "dbg",
                                      "boot",
                                      NVS_STORE_NET_NAMESPACE,
                                      NVS_STORE_NTRIP_NAMESPACE,
                                      NVS_STORE_FENCE_NAMESPACE};
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); ++i) {
        const esp_err_t err = nvs_store_clear_namespace(NVS_STORE_CFG_PARTITION, namespaces[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    /* IDF 6.1 Bluedroid 默认使用 bt_config.conf；也清理旧 bt_config 命名空间。 */
    const char *const radio[] = {"bt_config.conf", "bt_config", "nvs.net80211"};
    for (size_t i = 0; i < sizeof(radio) / sizeof(radio[0]); ++i) {
        const esp_err_t err = nvs_store_clear_namespace(NVS_STORE_SYSTEM_PARTITION, radio[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t nvs_store_erase_factory(void)
{
    const esp_err_t backup =
        nvs_store_clear_namespace(NVS_STORE_SYSTEM_PARTITION, NVS_STORE_BACKUP_NAMESPACE);
    return backup == ESP_OK
               ? nvs_store_clear_namespace(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE)
               : backup;
}

esp_err_t nvs_store_get_alarms(uint8_t (*addrs)[NVS_STORE_ADDR_LEN], size_t max, size_t *count)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;
    size_t len = 0U;
    uint8_t blob[NVS_STORE_ALARM_MAX * NVS_STORE_ADDR_LEN];
    size_t entries = 0U;
    size_t copy = 0U;

    if ((count == NULL) || ((addrs == NULL) && (max != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;

    err = nvs_store_open_cfg(NVS_STORE_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* 命名空间尚未创建：空白名单。 */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, NVS_STORE_KEY_ALM, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if ((len % NVS_STORE_ADDR_LEN) != 0U) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    if (len > sizeof(blob)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0U) {
        nvs_close(handle);
        return ESP_OK;
    }

    err = nvs_get_blob(handle, NVS_STORE_KEY_ALM, blob, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    /* len ≤ 24，entries ≤ 4，result 缩放安全。 */
    entries = len / NVS_STORE_ADDR_LEN;
    copy = (entries < max) ? entries : max;
    for (size_t idx = 0U; idx < copy; idx++) {
        for (size_t byte = 0U; byte < NVS_STORE_ADDR_LEN; byte++) {
            addrs[idx][byte] = blob[(idx * NVS_STORE_ADDR_LEN) + byte];
        }
    }
    *count = copy;
    return ESP_OK;
}

esp_err_t nvs_store_set_alarms(const uint8_t (*addrs)[NVS_STORE_ADDR_LEN], size_t count)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (count > NVS_STORE_ALARM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addrs == NULL) && (count != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_store_open_cfg(NVS_STORE_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (count == 0U) {
        err = nvs_erase_key(handle, NVS_STORE_KEY_ALM);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        /* count ≤ 4，count×6 ≤ 24；长度扩宽到 size_t 无损。 */
        err = nvs_set_blob(handle, NVS_STORE_KEY_ALM, addrs, count * NVS_STORE_ADDR_LEN);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
