#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool ssp_en;
} esp_bluedroid_config_t;

#define BT_BLUEDROID_INIT_CONFIG_DEFAULT() \
    {                                      \
        true                               \
    }

typedef enum {
    ESP_BLUEDROID_STATUS_UNINITIALIZED = 0,
    ESP_BLUEDROID_STATUS_INITIALIZED,
    ESP_BLUEDROID_STATUS_ENABLED,
} esp_bluedroid_status_t;

esp_err_t esp_bluedroid_init_with_cfg(esp_bluedroid_config_t *cfg);
esp_err_t esp_bluedroid_enable(void);
esp_err_t esp_bluedroid_disable(void);
esp_err_t esp_bluedroid_deinit(void);
esp_bluedroid_status_t esp_bluedroid_get_status(void);
