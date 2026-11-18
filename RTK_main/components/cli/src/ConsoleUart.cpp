// 控制台 UART 出口（ADR-0009 §2.4/§2.8）：带 TX 环形的 UART 驱动替换、ESP_LOG vprintf 钩子与统一输出端。
// enableUartTxBuffer()/installUartSink() 各只在启动任务调用一次（console 任务尚未读写 UART）；uartSink 由
// out 模块在任务上下文调用，环形满时阻塞，非 ISR 安全。

#include "ConsoleUart.hpp"

#include <cstddef>
#include <cstdio>

#include "cli/Out.hpp"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fw/Log.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace cli {
namespace detail {
namespace {

constexpr size_t kConsoleRxBufBytes = 256U; // 与 IDF uart_vfs_dev_port_init 的内部选择保持一致
constexpr uint32_t kTxDrainTimeoutMs =
    50U;                                 // 128 B 硬件 FIFO 在 2 Mbaud 下约 0.64 ms 发完，留足余量
constexpr int kRefTickMaxBaud = 1000000; // IDF uart_vfs_dev_port_init 的 REF_TICK/APB 分界

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
/**
 * @brief  控制台输出端：把文本一次写给 UART 输出环（ADR-0009 §2.8）
 * @param  text: 文本
 * @param  len: 字节数
 * @note   环形满时阻塞到有空间，只在任务上下文调用，不可在 ISR 中调用；与 logVprintf 共用同一条排空路径。
 *         只有装了 TX 环形驱动（CONFIG_RTK_CONSOLE_UART_TX_BUF > 0）才安装它。
 */
void uartSink(const char *text, size_t len) noexcept
{
    (void)uart_write_bytes(static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM), text, len);
}

vprintf_like_t s_orig_log_vprintf = nullptr;

/**
 * @brief  ESP_LOG 钩子：日志与命令回复走同一出口和同一把锁，整行写出、互不穿插
 * @param  fmt: 格式串
 * @param  args: 参数列表
 * @retval >=0 格式化后的字符数
 * @retval <0  回落路径（原 vprintf）的格式化失败
 * @note   ISR 上下文、调度器未运行或同任务重入（outLogVprintf 返回 -1）时回落到原 vprintf（IDF 的 VFS 路径）。
 */
int logVprintf(const char *fmt, va_list args)
{
    if ((xPortInIsrContext() == 0) && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
        const int n = outLogVprintf(fmt, args);
        if (n >= 0) {
            return n;
        }
    }
    return (s_orig_log_vprintf != nullptr) ? s_orig_log_vprintf(fmt, args)
                                           : std::vprintf(fmt, args);
}
#endif
#endif

} // namespace

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
void installUartSink() noexcept
{
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
    // 绕过 VFS 后由 out 模块负责 "\n" → "\r\n"，与 VFS 的 TX 行尾设置（CRLF）一致。
    setOutSink(uartSink, true);
    s_orig_log_vprintf = esp_log_set_vprintf(logVprintf);
#endif
}

// UART 驱动替换的步骤顺序（排空→删驱动→参数→引脚→安装→VFS 行尾）即内容，每步失败即返回；
// 经典 ESP32 没有 UART DMA，长输出靠大 TX 环形 + 中断搬运，避免命令任务阻塞在 FIFO 上（ADR-0013 §2.4）。
// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
esp_err_t enableUartTxBuffer() noexcept
{
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
    // Kconfig range 0..2 保证端口号合法；uart_port_t 是枚举，转 static_cast 不改变数值。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM);
    // uart_param_config() 会复位 TX FIFO（uart_hal_txfifo_rst）：先等已写入 FIFO 的启动日志发完，
    // 否则切换瞬间尚未移出的字节被丢弃，表现为启动日志行被截断（HIL 观察到 "W (260) uart_vfs: light slee"）。
    esp_err_t err = uart_wait_tx_done(port, pdMS_TO_TICKS(kTxDrainTimeoutMs));
    if (err != ESP_OK) {
        // 超时只意味着可能丢少量启动日志，不影响控制台功能，继续切换。
        FW_LOGW(fw::TraceCat::kSys, "uart_wait_tx_done: %s", esp_err_to_name(err));
    }
    err = uart_driver_delete(port);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "uart_driver_delete failed: %s", esp_err_to_name(err));
        return err;
    }

    constexpr int kBaudRate = CONFIG_RTK_CONSOLE_UART_BAUD;
    // 零初始化后逐字段赋值：枚举字段的 0 值虽无对应枚举名，但下面会全部覆盖，
    // 而 IDF 结构体成员随版本增减（v6.1 有 rx_glitch_filt_thresh/flags），逐字段指定会触发 missing-field-initializers。
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization): 上述成员随后全部赋有效枚举值
    uart_config_t cfg{};
    cfg.baud_rate = kBaudRate;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
#if SOC_UART_SUPPORT_REF_TICK
    // 与 IDF uart_vfs_dev_port_init 的选择一致：<= 1 MHz 用 REF_TICK，更高波特率必须用 APB（DEFAULT）。
    // IDF 以匿名 union 提供时钟字段，只能直接访问。
    cfg.source_clk = (kBaudRate > kRefTickMaxBaud) ? UART_SCLK_DEFAULT : UART_SCLK_REF_TICK;
#else
    cfg.source_clk = UART_SCLK_DEFAULT;
#endif
    err = uart_param_config(port, &cfg);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 控制台使用默认 UART 引脚（IOMUX 直连），这里不重路由：重路由反而会覆盖 ROM/console 的既有配置。
    err = uart_set_pin(port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(port, static_cast<int>(kConsoleRxBufBytes),
                              CONFIG_RTK_CONSOLE_UART_TX_BUF, 0, nullptr, 0);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // VFS 仍走驱动，行尾转换重新声明一遍，保证换驱动后行为不变（两处调用都是幂等的）。
    uart_vfs_dev_use_driver(port);
    if (uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_CR) != 0) {
        FW_LOGE(fw::TraceCat::kSys, "set rx line endings failed");
        return ESP_FAIL;
    }
    if (uart_vfs_dev_port_set_tx_line_endings(port, ESP_LINE_ENDINGS_CRLF) != 0) {
        FW_LOGE(fw::TraceCat::kSys, "set tx line endings failed");
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)
#endif

} // namespace detail
} // namespace cli
