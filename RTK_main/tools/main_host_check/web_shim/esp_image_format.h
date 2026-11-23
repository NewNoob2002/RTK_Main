// host 替身：保留 WebOta 静态断言使用的 v6.1 镜像大小与偏移。
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_IMAGE_HEADER_MAGIC 0xe9U
#define ESP_CHIP_ID_ESP32      0

typedef struct {
    uint8_t unused_prefix[12];
    uint16_t chip_id;
    uint8_t unused_suffix[10];
} esp_image_header_t;

typedef struct {
    uint8_t unused[8];
} esp_image_segment_header_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

typedef struct {
    uint32_t image_len;
} esp_image_metadata_t;

esp_err_t esp_image_get_metadata(const esp_partition_pos_t *pos, esp_image_metadata_t *image);

#ifdef __cplusplus
}
#endif
