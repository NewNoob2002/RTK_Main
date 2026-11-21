// host 替身：esp_netif 最小桩（仅 web_module_check 使用，不参与固件）。
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5004

typedef struct esp_netif_obj esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

static inline void esp_netif_set_ip4_addr(esp_ip4_addr_t *addr, uint8_t a, uint8_t b, uint8_t c,
                                          uint8_t d)
{
    addr->addr = (uint32_t)a | ((uint32_t)b << 8U) | ((uint32_t)c << 16U) | ((uint32_t)d << 24U);
}

typedef enum {
    ESP_NETIF_OP_START = 0,
    ESP_NETIF_OP_SET,
    ESP_NETIF_OP_GET,
    ESP_NETIF_OP_MAX,
} esp_netif_dhcp_option_mode_t;

typedef enum {
    ESP_NETIF_SUBNET_MASK = 1,
    ESP_NETIF_DOMAIN_NAME_SERVER = 6,
    ESP_NETIF_CAPTIVEPORTAL_URI = 114,
} esp_netif_dhcp_option_id_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
void esp_netif_destroy_default_wifi(void *esp_netif);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *esp_netif);
esp_err_t esp_netif_set_ip_info(esp_netif_t *esp_netif, const esp_netif_ip_info_t *ip_info);
esp_err_t esp_netif_dhcps_option(esp_netif_t *esp_netif, esp_netif_dhcp_option_mode_t opt_op,
                                 esp_netif_dhcp_option_id_t opt_id, void *opt_val,
                                 uint32_t opt_len);
esp_err_t esp_netif_dhcps_start(esp_netif_t *esp_netif);

#ifdef __cplusplus
}
#endif
