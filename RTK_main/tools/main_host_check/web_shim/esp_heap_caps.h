// Web 主机测试的 ESP-IDF heap_caps 替身声明，非固件构建。
#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr uint32_t MALLOC_CAP_8BIT = 1U << 2U;
inline constexpr uint32_t MALLOC_CAP_INTERNAL = 1U << 11U;

struct multi_heap_info_t {
    size_t total_free_bytes;
    size_t total_allocated_bytes;
    size_t largest_free_block;
    size_t minimum_free_bytes;
    size_t allocated_blocks;
    size_t free_blocks;
    size_t total_blocks;
};

size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps);
