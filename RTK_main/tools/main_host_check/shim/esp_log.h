// host 替身：转引 t13_host_check 的 esp_log.h 桩（main_host_check 编译用，不参与固件）。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "../../t13_host_check/shim/inc/esp_log.h"
#ifdef __cplusplus
}
#endif
