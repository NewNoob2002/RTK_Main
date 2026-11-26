// Web 主机测试的 FreeRTOS 任务快照替身声明，非固件构建。
#pragma once

#include "freertos/task.h"

struct TaskSnapshot_t {
    void *pxTCB;
    StackType_t *pxTopOfStack;
    StackType_t *pxEndOfStack;
};

UBaseType_t uxTaskGetNumberOfTasks();
TaskHandle_t xTaskGetHandle(const char *name);
StackType_t *xTaskGetStackStart(TaskHandle_t task);
void vTaskSuspendAll();
BaseType_t xTaskResumeAll();
BaseType_t vTaskGetSnapshot(TaskHandle_t task, TaskSnapshot_t *snapshot);
