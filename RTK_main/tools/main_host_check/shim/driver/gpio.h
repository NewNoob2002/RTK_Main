// GNSS 主机测试使用的 GPIO 子集，替身在 GnssResetCheck.cpp 记录驱动操作。
#pragma once

#include <cstdint>

#include "esp_err.h"

using gpio_num_t = int;
inline constexpr int GPIO_MODE_OUTPUT = 1;
inline constexpr int GPIO_PULLUP_DISABLE = 0;
inline constexpr int GPIO_PULLDOWN_DISABLE = 0;
inline constexpr int GPIO_INTR_DISABLE = 0;

struct gpio_config_t {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
};

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level);
esp_err_t gpio_config(const gpio_config_t *config);
