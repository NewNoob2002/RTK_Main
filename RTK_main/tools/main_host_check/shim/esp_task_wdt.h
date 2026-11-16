// host 替身：任务看门狗 add/reset 最小桩（main_host_check 编译用，不参与固件）。
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_status(TaskHandle_t task);
esp_err_t esp_task_wdt_delete(TaskHandle_t task);
#ifdef __cplusplus
}
#endif
