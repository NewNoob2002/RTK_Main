// host 替身：esp_timer 最小桩（仅 web_module_check 使用，不参与固件）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
