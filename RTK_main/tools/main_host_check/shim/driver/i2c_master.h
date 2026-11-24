// 主机替身：提供 Charger.cpp 使用的 I2C master 类型和入口。
// 仅由 charger_check 测试，串行调用。
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

struct I2cBus;
struct I2cDevice;
using i2c_master_bus_handle_t = I2cBus *;
using i2c_master_dev_handle_t = I2cDevice *;
using i2c_port_num_t = int;

enum i2c_clock_source_t { I2C_CLK_SRC_DEFAULT };
enum i2c_addr_bit_len_t { I2C_ADDR_BIT_LEN_7 };

struct i2c_master_bus_config_t {
    i2c_port_num_t i2c_port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    i2c_clock_source_t clk_source;
    uint8_t glitch_ignore_cnt;
    struct {
        unsigned enable_internal_pullup : 1;
    } flags;
};

struct i2c_device_config_t {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
};

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *out);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeoutMs);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle);
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus_handle);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t count,
                              int timeoutMs);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx,
                                      size_t txCount, uint8_t *rx, size_t rxCount, int timeoutMs);
