/**
 * @file    rtk_main.c
 * @brief   应用组合根：NVS → SN/白名单 → 链路模块 → SPP → 链路任务 → 控制台。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "alarm_msg.h"
#include "app_cons.h"
#include "app_link.h"
#include "app_version.h"
#include "bt_spp.h"
#include "link_mgr.h"
#include "store_nvs.h"

#define RTK_MAIN_TAG "rtk"
#define RTK_MAIN_NAME_PREFIX "E1-Lite-"
#define RTK_MAIN_SN_FALLBACK "0000"
#define RTK_MAIN_MAC_LEN (6U)
#define RTK_MAIN_MAC_OFS (4U) /* MAC 后 2 字节的起点 */

/* 有界字符串拷贝：长度取 min(strlen, width)，其余补 0。 */
static void
copy_str_field (char * p_dst, char const * p_src, size_t width)
{
    size_t idx = 0U;

    while ((idx < width) && ('\0' != p_src[idx]))
    {
        p_dst[idx] = p_src[idx];
        idx++;
    }
    while (idx < width)
    {
        p_dst[idx] = '\0';
        idx++;
    }
}

/* 优先读 NVS；不存在时用本机 BT MAC 后 2 字节生成。 */
static void
resolve_sn (char * p_sn, size_t size)
{
    esp_err_t err = store_get_sn(p_sn, size);
    uint8_t   mac[RTK_MAIN_MAC_LEN];

    if ((ESP_OK == err) && ('\0' != p_sn[0]))
    {
        return;
    }

    if (ESP_OK == esp_read_mac(mac, ESP_MAC_BT))
    {
        /* MAC 最后 2 字节：偏移 4、5 */
        (void) snprintf(p_sn, size, "%02X%02X", mac[RTK_MAIN_MAC_OFS],
                        mac[RTK_MAIN_MAC_OFS + 1U]);
        return;
    }

    (void) snprintf(p_sn, size, "%s", RTK_MAIN_SN_FALLBACK);
}

static void
load_alarms (link_addr_t * p_alarms, uint8_t * p_count)
{
    uint8_t   raw[LINK_MAX_ALARMS][LINK_ADDR_LEN];
    esp_err_t err = store_get_alarms(raw, LINK_MAX_ALARMS, p_count);

    if (ESP_OK != err)
    {
        *p_count = 0U;
        return;
    }

    for (uint8_t idx = 0U; idx < *p_count; idx++)
    {
        for (uint32_t byte = 0U; byte < LINK_ADDR_LEN; byte++)
        {
            p_alarms[idx].addr[byte] = raw[idx][byte];
        }
    }
}

void
app_main (void)
{
    char        sn[STORE_SN_MAX + 1U]    = {0};
    char        bt_name[BT_SPP_NAME_MAX] = {0};
    link_addr_t alarms[LINK_MAX_ALARMS];
    uint8_t     alarm_count = 0U;
    esp_err_t   err         = ESP_OK;

    ESP_LOGI(RTK_MAIN_TAG, "RTK_main %s boot", APP_VERSION_STR);

    err = store_init();
    if (ESP_OK != err)
    {
        ESP_LOGE(RTK_MAIN_TAG, "store init failed: %s; use defaults",
                 esp_err_to_name(err));
    }

    resolve_sn(sn, sizeof(sn));
    load_alarms(alarms, &alarm_count);

    link_mgr_init(&g_app_link, app_link_policy(), alarms, alarm_count);
    alarm_src_init(&g_app_alarm);
    cmd_disp_init(&g_app_ctx, &g_app_alarm, &g_app_link);
    copy_str_field(g_app_ctx.info.sn, sn, sizeof(g_app_ctx.info.sn));
    copy_str_field(g_app_ctx.info.fw_ver, APP_VERSION_STR,
                   sizeof(g_app_ctx.info.fw_ver));

    (void) snprintf(bt_name, sizeof(bt_name), "%s%s", RTK_MAIN_NAME_PREFIX, sn);
    ESP_LOGI(RTK_MAIN_TAG, "BT name: %s", bt_name);

    err = bt_spp_init(bt_name);
    if (ESP_OK != err)
    {
        ESP_LOGE(RTK_MAIN_TAG, "bt_spp init failed: %s; console only",
                 esp_err_to_name(err));
    }
    else
    {
        err = app_link_start();
        if (ESP_OK != err)
        {
            ESP_LOGE(RTK_MAIN_TAG, "link task start failed: %s",
                     esp_err_to_name(err));
        }
    }

    err = app_cons_start();
    if (ESP_OK != err)
    {
        ESP_LOGE(RTK_MAIN_TAG, "console start failed: %s",
                 esp_err_to_name(err));
    }
}

/*** end of file ***/
