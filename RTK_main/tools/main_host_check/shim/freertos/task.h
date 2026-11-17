// host 替身：任务与临界区最小桩，并把 portENTER/EXIT_CRITICAL 映射到 task 层（不参与固件）。
#pragma once
#include "FreeRTOS.h" // IWYU pragma: keep（提供 TaskHandle_t/TickType_t 与 host 临界区宏）
// 与驱动替身共用同一把互斥量，覆盖 RAII 守卫的 port 层入口。
#define portENTER_CRITICAL(mux) taskENTER_CRITICAL(mux)
#define portEXIT_CRITICAL(mux)  taskEXIT_CRITICAL(mux)
#ifdef __cplusplus
extern "C" {
#endif
TaskHandle_t xTaskGetCurrentTaskHandle(void);
// 由 t13 的 FakeRtos.c 定义；此处声明可让诊断只出现在 .h（不参与命名检查）且保持 C 链接。
extern __thread StaticTask_t *fake_current_task;
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
#ifdef __cplusplus
}
#endif
