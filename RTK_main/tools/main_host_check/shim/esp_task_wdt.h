// Generated-by: Codex / GPT-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_reset(void);
#ifdef __cplusplus
}
#endif
