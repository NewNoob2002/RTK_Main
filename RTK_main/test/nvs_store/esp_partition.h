// IDF 分区指针接口的主机替身。
#pragma once

#include <stddef.h>

#include "esp_err.h"

#define ESP_PARTITION_TYPE_DATA           1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 0x82

typedef struct {
    size_t size;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t len);
