// NVS 持久化：设备 SN 与报警器白名单（ADR-0003：C 实现）。
// 每次操作 nvs_open → 操作 → nvs_close；写操作成功后 commit。仅访问 NVS 分区。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "rtk_store.h"

#define RTK_STORE_TAG     "rtk_store"
#define RTK_STORE_NS      "rtk"
#define RTK_STORE_KEY_SN  "sn"
#define RTK_STORE_KEY_ALM "alarm_wl"

esp_err_t rtk_store_init(void)
{
    esp_err_t err = nvs_flash_init();

    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGW(RTK_STORE_TAG, "nvs init 0x%x, erase nvs partition and retry", err);
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(RTK_STORE_TAG, "nvs erase failed: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(RTK_STORE_TAG, "nvs init failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t rtk_store_get_sn(char *out, size_t out_size)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;
    size_t len = 0U;

    if ((out == NULL) || (out_size < (RTK_STORE_SN_MAX + 1U))) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    err = nvs_open(RTK_STORE_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* 命名空间不存在同样返回 ESP_ERR_NVS_NOT_FOUND（冻结头契约）。 */
        return err;
    }

    len = out_size;
    err = nvs_get_str(handle, RTK_STORE_KEY_SN, out, &len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        /* 存储值超过调用方缓冲：按“超长”契约返回。 */
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        return err;
    }
    /* nvs_get_str 的 len 含结尾 0；空串或超过 16 字节都按损坏处理。 */
    if ((len <= 1U) || ((len - 1U) > RTK_STORE_SN_MAX)) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t rtk_store_set_sn(const char *sn)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;
    size_t len = 0U;

    if (sn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    len = strlen(sn);
    if ((len == 0U) || (len > RTK_STORE_SN_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(RTK_STORE_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, RTK_STORE_KEY_SN, sn);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t rtk_store_get_alarms(uint8_t (*addrs)[RTK_STORE_ADDR_LEN], size_t max, size_t *count)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;
    size_t len = 0U;
    uint8_t blob[RTK_STORE_ALARM_MAX * RTK_STORE_ADDR_LEN];
    size_t entries = 0U;
    size_t copy = 0U;

    if ((count == NULL) || ((addrs == NULL) && (max != 0U))) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;

    err = nvs_open(RTK_STORE_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* 命名空间尚未创建：空白名单。 */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, RTK_STORE_KEY_ALM, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if ((len % RTK_STORE_ADDR_LEN) != 0U) {
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

    err = nvs_get_blob(handle, RTK_STORE_KEY_ALM, blob, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    /* len ≤ 24，entries ≤ 4，result 缩放安全。 */
    entries = len / RTK_STORE_ADDR_LEN;
    copy = (entries < max) ? entries : max;
    for (size_t idx = 0U; idx < copy; idx++) {
        for (size_t byte = 0U; byte < RTK_STORE_ADDR_LEN; byte++) {
            addrs[idx][byte] = blob[(idx * RTK_STORE_ADDR_LEN) + byte];
        }
    }
    *count = copy;
    return ESP_OK;
}

esp_err_t rtk_store_set_alarms(const uint8_t (*addrs)[RTK_STORE_ADDR_LEN], size_t count)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (count > RTK_STORE_ALARM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addrs == NULL) && (count != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(RTK_STORE_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (count == 0U) {
        err = nvs_erase_key(handle, RTK_STORE_KEY_ALM);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        /* count ≤ 4，count×6 ≤ 24；长度扩宽到 size_t 无损。 */
        err = nvs_set_blob(handle, RTK_STORE_KEY_ALM, addrs, count * RTK_STORE_ADDR_LEN);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
