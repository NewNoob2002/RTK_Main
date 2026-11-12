// host 替身：只提供堆用量查询的 esp_system 最小桩（main_host_check 编译用，不参与固件）。
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
#ifdef __cplusplus
}
#endif
