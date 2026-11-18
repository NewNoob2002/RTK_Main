// GNSS 主机测试使用的 UART 子集，替身实现在 GnssResetCheck.cpp。
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

using uart_port_t = int;
inline constexpr int UART_DATA_8_BITS = 8;
inline constexpr int UART_PARITY_DISABLE = 0;
inline constexpr int UART_STOP_BITS_1 = 1;
inline constexpr int UART_HW_FLOWCTRL_DISABLE = 0;
inline constexpr int UART_SCLK_DEFAULT = 0;
inline constexpr int UART_PIN_NO_CHANGE = -1;

struct uart_config_t {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    unsigned rx_flow_ctrl_thresh;
    unsigned rx_glitch_filt_thresh;
    int source_clk;
    unsigned flags;
};

inline esp_err_t uart_driver_install(uart_port_t, int, int, int, void *, int)
{
    return ESP_OK;
}
inline esp_err_t uart_driver_delete(uart_port_t)
{
    return ESP_OK;
}
inline esp_err_t uart_set_pin(uart_port_t, int, int, int, int)
{
    return ESP_OK;
}
inline esp_err_t uart_wait_tx_done(uart_port_t, TickType_t)
{
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t, const uart_config_t *);
esp_err_t uart_set_baudrate(uart_port_t, uint32_t);
esp_err_t uart_flush_input(uart_port_t);
int uart_write_bytes(uart_port_t, const void *, size_t);
int uart_read_bytes(uart_port_t, void *, size_t, TickType_t);
