// host 替身：转引 t13_host_check 的 FreeRTOS.h 桩（main_host_check 编译用，不参与固件）。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "../../../t13_host_check/shim/inc/freertos/FreeRTOS.h"
#ifdef __cplusplus
}
#endif
#define portENTER_CRITICAL(mux) taskENTER_CRITICAL(mux)
#define portEXIT_CRITICAL(mux)  taskEXIT_CRITICAL(mux)
#define configNUMBER_OF_CORES   2
#define configMAX_PRIORITIES    25
