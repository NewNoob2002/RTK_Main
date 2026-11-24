// 主机替身：charger_check 用内存中的持锁位验证总线操作的互斥约定。
// 单线程测试，不模拟 RTOS 调度。
#pragma once

#include "freertos/FreeRTOS.h"

struct StaticSemaphore_t {
    bool held = false;
};
using SemaphoreHandle_t = StaticSemaphore_t *;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
