// host 替身：esp_wifi 最小桩（仅 web_module_check 使用，不参与固件）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

extern esp_event_base_t const WIFI_EVENT;

enum {
    WIFI_EVENT_AP_STACONNECTED = 14,
    WIFI_EVENT_AP_STADISCONNECTED = 15,
};

typedef enum {
    WIFI_MODE_NULL = 0,
    WIFI_MODE_AP = 2,
} wifi_mode_t;

typedef enum {
    WIFI_IF_AP = 1,
} wifi_interface_t;

typedef enum {
    WIFI_STORAGE_FLASH = 0,
    WIFI_STORAGE_RAM = 1,
} wifi_storage_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WPA2_PSK = 3,
} wifi_auth_mode_t;

typedef struct {
    bool capable;
    bool required;
} wifi_pmf_config_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    uint8_t ssid_len;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    uint8_t max_connection;
    wifi_pmf_config_t pmf_cfg;
} wifi_ap_config_t;

typedef union {
    wifi_ap_config_t ap;
} wifi_config_t;

typedef struct {
    int magic;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() {0x1F2F3F4F}

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);

#ifdef __cplusplus
}
#endif
