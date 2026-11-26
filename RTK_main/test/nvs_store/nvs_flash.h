// 主机替身的 NVS 分区初始化接口。
#pragma once

#include "esp_partition.h"

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_init_partition_ptr(const esp_partition_t *partition);
esp_err_t nvs_flash_erase(void);
