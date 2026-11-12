#pragma once

/* BT 控制器/设备基础类型的 host 替身。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef uint8_t esp_bd_addr_t[6];
typedef uint8_t esp_bt_pin_code_t[16];

typedef enum {
    ESP_BT_MODE_IDLE = 0,
    ESP_BT_MODE_BLE = 1,
    ESP_BT_MODE_CLASSIC_BT = 2,
    ESP_BT_MODE_BTDM = 3,
} esp_bt_mode_t;

typedef enum {
    ESP_BT_CONTROLLER_STATUS_IDLE = 0,
    ESP_BT_CONTROLLER_STATUS_INITED,
    ESP_BT_CONTROLLER_STATUS_ENABLED,
} esp_bt_controller_status_t;

typedef struct {
    const char *controller_task_name;
    int controller_task_stack_size;
    int controller_task_prio;
    int hci_uart_no;
    int hci_uart_baudrate;
} esp_bt_controller_config_t;

#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() \
    {                                       \
        "BTu", 4096, 8, 1, 921600           \
    }

esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *cfg);
esp_err_t esp_bt_controller_enable(esp_bt_mode_t mode);
esp_err_t esp_bt_controller_disable(void);
esp_err_t esp_bt_controller_deinit(void);
esp_err_t esp_bt_controller_mem_release(esp_bt_mode_t mode);
esp_bt_controller_status_t esp_bt_controller_get_status(void);
