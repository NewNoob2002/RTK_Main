#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_bt.h"
#include "esp_err.h"

typedef enum {
    ESP_BT_STATUS_SUCCESS = 0,
    ESP_BT_STATUS_FAIL = 1,
    ESP_BT_STATUS_NOT_READY = 2,
} esp_bt_status_t;

typedef enum {
    ESP_BT_IO_CAP_OUT = 0,
    ESP_BT_IO_CAP_IO,
    ESP_BT_IO_CAP_IN,
    ESP_BT_IO_CAP_NONE,
} esp_bt_io_cap_t;

typedef enum {
    ESP_BT_SP_IOCAP_MODE = 0,
} esp_bt_sp_param_t;

typedef enum {
    ESP_BT_PIN_TYPE_VARIABLE = 0,
    ESP_BT_PIN_TYPE_FIXED,
} esp_bt_pin_type_t;

typedef enum {
    ESP_BT_NON_CONNECTABLE = 0,
    ESP_BT_CONNECTABLE = 1,
} esp_bt_connection_mode_t;

typedef enum {
    ESP_BT_NON_DISCOVERABLE = 0,
    ESP_BT_LIMITED_DISCOVERABLE = 1,
    ESP_BT_GENERAL_DISCOVERABLE = 2,
} esp_bt_discovery_mode_t;

typedef enum {
    ESP_BT_GAP_AUTH_CMPL_EVT = 0,
    ESP_BT_GAP_PIN_REQ_EVT,
    ESP_BT_GAP_CFM_REQ_EVT,
    ESP_BT_GAP_MODE_CHG_EVT,
} esp_bt_gap_cb_event_t;

typedef union {
    struct {
        esp_bt_status_t stat;
        esp_bd_addr_t bda;
        char device_name[248];
    } auth_cmpl;
    struct {
        esp_bd_addr_t bda;
        int pin_code_len;
        esp_bt_pin_code_t pin_code;
    } pin_req;
    struct {
        esp_bd_addr_t bda;
        uint32_t num_val;
    } cfm_req;
    struct {
        uint32_t mode;
    } mode_chg;
} esp_bt_gap_cb_param_t;

typedef void (*esp_bt_gap_cb_t)(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t callback);
esp_err_t esp_bt_gap_set_device_name(const char *name);
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c_mode, esp_bt_discovery_mode_t d_mode);
esp_err_t esp_bt_gap_set_pin(esp_bt_pin_type_t pin_type, uint8_t pin_code_len,
                             esp_bt_pin_code_t pin_code);
esp_err_t esp_bt_gap_set_security_param(esp_bt_sp_param_t param_type, void *value, uint8_t len);
esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t bd_addr, bool accept, uint8_t pin_code_len,
                               esp_bt_pin_code_t pin_code);
esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t bd_addr, bool accept);
