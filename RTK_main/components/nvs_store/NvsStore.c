// NVS 持久化实现：设备 SN 与报警器白名单；每次操作 nvs_open → 操作 → nvs_close，写成功后 commit。
// 仅由启动流程与存储执行器任务串行调用，二者不并发；不可在 ISR 中调用。
// 不分配堆；nvs_* 为阻塞调用，只在任务上下文执行。

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "fw/LogC.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "NvsStore.h"

#define NVS_STORE_NS      "rtk"
#define NVS_STORE_KEY_SN  "sn"
#define NVS_STORE_KEY_ALM "alarm_wl"
/* 标记只在目标原本缺失时创建，重启后允许继续修复未完成的复制。 */
#define NVS_STORE_KEY_SN_MIGRATION "sn_migrating"

static bool s_factory_available;
static bool s_prefer_legacy_sn;

/**
 * @brief 初始化用户分区，遇到已知布局错误时擦除该分区后重试一次。
 * @retval 用户分区初始化或擦除的结果。
 * @note 仅启动期单线程调用，可阻塞，非 ISR。
 */
static esp_err_t nvs_store_init_user(void)
{
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        FW_CLOGW(FW_LOG_CAT_STORE, "NVS init failed: %s, erasing user partition and retrying",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_STORE, "User NVS unavailable: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 打开指定分区的 SN 命名空间。
 * @param factory: true 选择 fac，false 选择旧 rtk。
 * @param mode: 访问模式。
 * @param[out] handle: 成功时取得的 NVS 句柄。
 * @retval NVS 打开结果。
 * @note 任务上下文，可阻塞，非 ISR。
 */
static esp_err_t nvs_store_open_sn(bool factory, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (factory) {
        return nvs_open_from_partition(NVS_STORE_FACTORY_PARTITION, NVS_STORE_FACTORY_NAMESPACE,
                                       mode, handle);
    }
    return nvs_open(NVS_STORE_NS, mode, handle);
}

/**
 * @brief 从打开的命名空间读取并验证 SN。
 * @param handle: NVS 句柄。
 * @param[out] out: 输出缓冲。
 * @param out_size: 缓冲容量，至少一个字节。
 * @retval ESP_OK 读取有效 SN；其他为 NVS 错误或 ESP_ERR_INVALID_SIZE。
 * @note 任务上下文，可阻塞，非 ISR；失败清零 out[0]。
 */
static esp_err_t nvs_store_read_sn(nvs_handle_t handle, char *out, size_t out_size)
{
    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, NVS_STORE_KEY_SN, out, &len);
    if ((err == ESP_ERR_NVS_INVALID_LENGTH) ||
        ((err == ESP_OK) && ((len <= 1U) || ((len - 1U) > NVS_STORE_SN_MAX)))) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

/**
 * @brief 写入并提交打开的命名空间中的 SN。
 * @param handle: 可写 NVS 句柄。
 * @param sn: 已验证长度的 SN。
 * @retval 写入或提交结果。
 * @note 任务上下文，可阻塞，非 ISR。
 */
static esp_err_t nvs_store_write_sn(nvs_handle_t handle, const char *sn)
{
    const esp_err_t err = nvs_set_str(handle, NVS_STORE_KEY_SN, sn);
    return (err == ESP_OK) ? nvs_commit(handle) : err;
}

/**
 * @brief 删除并提交指定键，已删除视为成功。
 * @param handle: 可写 NVS 句柄。
 * @param key: 待删除的键。
 * @retval 删除或提交结果。
 * @note 任务上下文，可阻塞，非 ISR。
 */
static esp_err_t nvs_store_remove_key(nvs_handle_t handle, const char *key)
{
    const esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return (err == ESP_OK) ? nvs_commit(handle) : err;
}

/**
 * @brief 完成或恢复一次 SN 复制，确认读回后才删除旧键。
 * @param legacy: 旧命名空间的可写句柄。
 * @param factory: 出厂命名空间的可写句柄。
 * @param sn: 已验证的旧 SN。
 * @note 仅启动期单线程调用，可阻塞，非 ISR；每次尝试只记一条结果日志。
 */
static void nvs_store_copy_sn(nvs_handle_t legacy, nvs_handle_t factory, const char *sn)
{
    uint8_t marker = 0U;
    esp_err_t err = nvs_get_u8(factory, NVS_STORE_KEY_SN_MIGRATION, &marker);
    char readback[NVS_STORE_SN_MAX + 1U] = {0};
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_store_read_sn(factory, readback, sizeof(readback));
        if (err == ESP_OK) {
            return; /* 已有出厂 SN，保留现值。 */
        }
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            s_prefer_legacy_sn = true;
            err = nvs_set_u8(factory, NVS_STORE_KEY_SN_MIGRATION, 1U);
            if (err == ESP_OK) {
                err = nvs_commit(factory);
            }
        }
    }
    if (err != ESP_OK) {
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migration could not check or mark fac/sn: %s",
                 esp_err_to_name(err));
        return;
    }

    s_prefer_legacy_sn = true;
    err = nvs_store_write_sn(factory, sn);
    if (err != ESP_OK) {
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migration could not write fac/sn: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_store_read_sn(factory, readback, sizeof(readback));
    if (err != ESP_OK || strcmp(sn, readback) != 0) {
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migration could not verify fac/sn: %s",
                 (err == ESP_OK) ? "value mismatch" : esp_err_to_name(err));
        return;
    }

    err = nvs_store_remove_key(legacy, NVS_STORE_KEY_SN);
    if (err != ESP_OK) {
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migration could not remove rtk/sn: %s",
                 esp_err_to_name(err));
        return;
    }
    s_prefer_legacy_sn = false;
    err = nvs_store_remove_key(factory, NVS_STORE_KEY_SN_MIGRATION);
    if (err != ESP_OK) {
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migrated, but marker cleanup failed: %s",
                 esp_err_to_name(err));
        return;
    }
    FW_CLOGI(FW_LOG_CAT_STORE, "SN migrated to factory partition");
}

/**
 * @brief 搜索旧 SN，有效时打开两个命名空间执行迁移。
 * @note 仅两个分区均已初始化时调用；可阻塞，非 ISR。
 */
static void nvs_store_migrate_sn(void)
{
    nvs_handle_t legacy = 0;
    nvs_handle_t factory = 0;
    char sn[NVS_STORE_SN_MAX + 1U] = {0};
    esp_err_t err = nvs_store_open_sn(false, NVS_READONLY, &legacy);
    if (err == ESP_OK) {
        err = nvs_store_read_sn(legacy, sn, sizeof(sn));
        nvs_close(legacy);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migration could not read rtk/sn: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_store_open_sn(false, NVS_READWRITE, &legacy);
    if (err != ESP_OK) {
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migration could not open rtk for cleanup: %s",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_store_open_sn(true, NVS_READWRITE, &factory);
    if (err == ESP_OK) {
        nvs_store_copy_sn(legacy, factory, sn);
        nvs_close(factory);
    } else {
        s_prefer_legacy_sn = true;
        FW_CLOGW(FW_LOG_CAT_STORE, "SN migration could not open fac: %s", esp_err_to_name(err));
    }
    nvs_close(legacy);
}

esp_err_t nvs_store_init(void)
{
    const esp_err_t user_err = nvs_store_init_user();
    const esp_err_t factory_err = nvs_flash_init_partition(NVS_STORE_FACTORY_PARTITION);
    s_factory_available = factory_err == ESP_OK;
    s_prefer_legacy_sn = false;
    if (!s_factory_available) {
        FW_CLOGE(FW_LOG_CAT_STORE, "Factory NVS unavailable: %s", esp_err_to_name(factory_err));
    } else if (user_err == ESP_OK) {
        nvs_store_migrate_sn();
    }
    return user_err;
}

bool nvs_store_factory_available(void)
{
    return s_factory_available;
}

esp_err_t nvs_store_get_sn(char *out, size_t out_size)
{
    nvs_handle_t handle = 0;
    if ((out != NULL) && (out_size != 0U)) {
        out[0] = '\0';
    }
    if ((out == NULL) || (out_size < (NVS_STORE_SN_MAX + 1U))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        nvs_store_open_sn(s_factory_available && !s_prefer_legacy_sn, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_store_read_sn(handle, out, out_size);
        nvs_close(handle);
    }
    return err;
}

esp_err_t nvs_store_set_sn(const char *sn)
{
    nvs_handle_t handle = 0;
    if (sn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strlen(sn);
    if ((len == 0U) || (len > NVS_STORE_SN_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_factory_available) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_store_open_sn(true, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    /* 产线显式设置优先于未完成的旧 SN 迁移。 */
    err = nvs_store_remove_key(handle, NVS_STORE_KEY_SN_MIGRATION);
    if (err == ESP_OK) {
        err = nvs_store_write_sn(handle, sn);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        s_prefer_legacy_sn = false;
    }
    return err;
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

    err = nvs_open(NVS_STORE_NS, NVS_READONLY, &handle);
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

    err = nvs_open(NVS_STORE_NS, NVS_READWRITE, &handle);
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
