// 控制台 UART 出口的私有接口：TX 环形驱动替换与统一输出端安装（ADR-0009 §2.4/§2.8）。
// 只在 Console::init() 的启动阶段各调用一次；调用时 console 任务尚未读写 UART。非 ISR 安全。
#pragma once

#include "esp_err.h"

namespace cli::detail {

/**
 * @brief  用带 TX 环形的 UART 驱动替换 esp_console 装的 0 字节 TX 驱动（ADR-0009 §2.4）
 * @retval ESP_OK 成功
 * @retval 其他   uart_driver_delete / uart_driver_install 等返回的 esp_err_t，失败时 UART 可能已停用
 * @note   只在启动任务调用一次：必须在 esp_console_new_repl_uart() 之后、esp_console_start_repl() 之前，
 *         此时 console 任务尚未读写 UART。会先阻塞等已写入 FIFO 的字节发完（最多 50 ms）。
 */
[[nodiscard]] esp_err_t enableUartTxBuffer() noexcept;

/**
 * @brief  安装统一输出端：out 模块走 UART 直写，ESP_LOG 走同一出口与同一把锁
 * @note   只在启动任务调用一次，位于 enableUartTxBuffer() 之后；之后控制台输出即走 uartSink。
 */
void installUartSink() noexcept;

} // namespace cli::detail
