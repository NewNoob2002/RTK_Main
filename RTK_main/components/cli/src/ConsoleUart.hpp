// 控制台 UART 出口的私有接口：TX 环形驱动替换与统一输出端安装（ADR-0009 §2.4/§2.8）。
// 只在 Console::init() 的启动阶段各调用一次；调用时 console 任务尚未读写 UART。非 ISR 安全。
#pragma once

#include "esp_err.h"

namespace cli::detail {

/**
 * @brief  用带 TX 环形的 UART 驱动替换 esp_console 装的 0 字节 TX 驱动
 * @retval ESP_OK 成功；其他为 UART 驱动删除/安装失败码
 * @note   只在启动任务调用：esp_console_new_repl_uart() 之后、esp_console_start_repl() 之前。
 */
[[nodiscard]] esp_err_t enableUartTxBuffer() noexcept;

/**
 * @brief  安装统一输出端：out 模块走 UART 直写，ESP_LOG 走同一出口与同一把锁
 * @note   只在启动任务调用一次，位于 enableUartTxBuffer() 之后。
 */
void installUartSink() noexcept;

} // namespace cli::detail
