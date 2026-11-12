/**
 * @file    store_nvs.c
 * @brief   NVS 持久化：设备 SN 与报警器白名单（driver 层）。
 *
 * 每次操作 nvs_open → 操作 → nvs_close；写操作成功后 nvs_commit。
 * 命名空间与键见 store_nvs.h。仅访问 nvs 分区。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "store_nvs.h"

#define STORE_TAG "store"
#define STORE_NAMESPACE "rtk"
#define STORE_KEY_SN "sn"
#define STORE_KEY_ALARMS "alarm_wl"
#define STORE_ADDR_LEN (6U)

esp_err_t
store_init (void)
{
    esp_err_t err = nvs_flash_init();

    if ((ESP_ERR_NVS_NO_FREE_PAGES == err) ||
        (ESP_ERR_NVS_NEW_VERSION_FOUND == err))
    {
        ESP_LOGW(STORE_TAG, "nvs init 0x%x, erase nvs partition and retry",
                 err);
        err = nvs_flash_erase();
        if (ESP_OK != err)
        {
            ESP_LOGE(STORE_TAG, "nvs erase failed: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }

    if (ESP_OK != err)
    {
        ESP_LOGE(STORE_TAG, "nvs init failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t
store_get_sn (char * p_out, size_t out_size)
{
    nvs_handle_t h   = 0;
    esp_err_t    err = ESP_OK;
    size_t       len = 0U;

    if ((NULL == p_out) || (0U == out_size))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &h);
    if (ESP_OK != err)
    {
        return err;
    }

    len = out_size;
    err = nvs_get_str(h, STORE_KEY_SN, p_out, &len);
    nvs_close(h);
    return err;
}

esp_err_t
store_set_sn (char const * p_sn)
{
    nvs_handle_t h   = 0;
    esp_err_t    err = ESP_OK;
    size_t       len = 0U;

    if (NULL == p_sn)
    {
        return ESP_ERR_INVALID_ARG;
    }
    len = strlen(p_sn);
    if ((0U == len) || (len > STORE_SN_MAX))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &h);
    if (ESP_OK != err)
    {
        return err;
    }

    err = nvs_set_str(h, STORE_KEY_SN, p_sn);
    if (ESP_OK == err)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t
store_get_alarms (uint8_t (*p_addrs)[STORE_ADDR_LEN], uint8_t max,
                  uint8_t * p_count)
{
    nvs_handle_t h   = 0;
    esp_err_t    err = ESP_OK;
    size_t       len = 0U;
    uint8_t      blob[STORE_ALARM_MAX * STORE_ADDR_LEN];
    uint8_t      count = 0U;

    if ((NULL == p_count) || ((NULL == p_addrs) && (0U != max)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *p_count = 0U;

    err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &h);
    if (ESP_ERR_NVS_NOT_FOUND == err)
    {
        /* 命名空间尚未创建：视为空白名单。 */
        return ESP_OK;
    }
    if (ESP_OK != err)
    {
        return err;
    }

    err = nvs_get_blob(h, STORE_KEY_ALARMS, NULL, &len);
    if (ESP_ERR_NVS_NOT_FOUND == err)
    {
        nvs_close(h);
        return ESP_OK;
    }
    if (ESP_OK != err)
    {
        nvs_close(h);
        return err;
    }
    if ((0U != (len % STORE_ADDR_LEN)) ||
        (len > (STORE_ALARM_MAX * STORE_ADDR_LEN)))
    {
        nvs_close(h);
        return ESP_ERR_INVALID_SIZE;
    }
    if (0U == len)
    {
        nvs_close(h);
        return ESP_OK;
    }

    err = nvs_get_blob(h, STORE_KEY_ALARMS, blob, &len);
    nvs_close(h);
    if (ESP_OK != err)
    {
        return err;
    }

    /* len ≤ STORE_ALARM_MAX×6，count ≤ 4，u8 截断安全 */
    count = (uint8_t) (len / STORE_ADDR_LEN);
    if (count > max)
    {
        count = max;
    }
    for (uint8_t idx = 0U; idx < count; idx++)
    {
        memcpy(p_addrs[idx], &blob[idx * STORE_ADDR_LEN], STORE_ADDR_LEN);
    }
    *p_count = count;
    return ESP_OK;
}

esp_err_t
store_set_alarms (uint8_t const (*p_addrs)[STORE_ADDR_LEN], uint8_t count)
{
    nvs_handle_t h   = 0;
    esp_err_t    err = ESP_OK;

    /* count ≤ 255，转换为 uint32 仅扩宽 */
    if ((uint32_t) count > STORE_ALARM_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if ((NULL == p_addrs) && (0U != count))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &h);
    if (ESP_OK != err)
    {
        return err;
    }

    if (0U == count)
    {
        err = nvs_erase_key(h, STORE_KEY_ALARMS);
        if (ESP_ERR_NVS_NOT_FOUND == err)
        {
            err = ESP_OK;
        }
    }
    else
    {
        /* count ≤ 4，count×6 ≤ 24，长度参数扩宽到 size_t 无损 */
        err = nvs_set_blob(h, STORE_KEY_ALARMS, p_addrs,
                           (size_t) count * STORE_ADDR_LEN);
    }

    if (ESP_OK == err)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/*** end of file ***/
