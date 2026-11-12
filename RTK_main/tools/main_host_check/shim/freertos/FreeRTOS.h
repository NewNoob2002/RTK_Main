// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
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
