// Generated-by: Codex / GPT-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include "FreeRTOS.h" // IWYU pragma: keep（提供 TaskHandle_t/TickType_t 与 host 临界区宏）
// 与驱动替身共用同一把互斥量，覆盖 RAII 守卫的 port 层入口。
#define portENTER_CRITICAL(mux) taskENTER_CRITICAL(mux)
#define portEXIT_CRITICAL(mux)  taskEXIT_CRITICAL(mux)
#ifdef __cplusplus
extern "C" {
#endif
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
#ifdef __cplusplus
}
#endif
