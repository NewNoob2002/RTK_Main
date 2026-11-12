// 控制台组件（ADR-0007 §3.3）：命令表原子登记（只保存静态表 span）与 esp_console REPL 启动。
// add/start 只在启动任务调用，回调在 console 任务中执行；不分配堆（命令表只存指针）。

#include "rtk/console/console.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace rtk::console {

namespace {

constexpr const char *kTag = "rtk_console";

// 只保存调用方静态表的 span（pointer + length），不拷贝 Cmd 结构体以节省静态 DRAM。
// 每个非空表至少含 1 条命令，因此非空表数不会超过 kMaxCmds。
std::array<std::span<const Cmd>, kMaxCmds> s_tables{};
size_t s_table_count = 0U;
size_t s_cmd_count = 0U;
bool s_started = false;

bool has_cmd_name(const char *name) noexcept
{
    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &existing : s_tables[t]) {
            if (std::strcmp(existing.name, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

void cleanup_repl(esp_console_repl_t *repl) noexcept
{
    if (repl == nullptr) {
        return;
    }
    const esp_err_t cleanup_err = repl->del(repl);
    if (cleanup_err != ESP_OK) {
        ESP_LOGW(kTag, "console cleanup failed: %s", esp_err_to_name(cleanup_err));
    }
}

} // namespace

esp_err_t add(std::span<const Cmd> cmds) noexcept
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cmds.empty()) {
        return ESP_OK;
    }

    // 原子校验整张表：任一校验失败不得登记任何一项。
    for (size_t i = 0U; i < cmds.size(); ++i) {
        const Cmd &cmd = cmds[i];
        if ((cmd.name == nullptr) || (cmd.name[0] == '\0') || (cmd.help == nullptr) || (cmd.help[0] == '\0') ||
            (cmd.handler == nullptr)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (has_cmd_name(cmd.name)) {
            return ESP_ERR_INVALID_STATE;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (std::strcmp(cmds[j].name, cmd.name) == 0) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    if ((cmds.size() > (kMaxCmds - s_cmd_count)) || (s_table_count >= kMaxCmds)) {
        return ESP_ERR_NO_MEM;
    }

    s_tables[s_table_count] = cmds;
    ++s_table_count;
    s_cmd_count += cmds.size();
    return ESP_OK;
}

esp_err_t start(const char *prompt) noexcept
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((prompt == nullptr) || (prompt[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = prompt;
    repl_cfg.max_cmdline_length = CONFIG_RTK_CONSOLE_CMDLINE_MAX;
    repl_cfg.task_stack_size = CONFIG_RTK_CONSOLE_STACK_SIZE;
    repl_cfg.task_priority = CONFIG_RTK_CONSOLE_TASK_PRIO;
    // 默认 tskNO_AFFINITY；这里显式绑到配置核（默认 core 0，链路任务独占 core 1）。
    repl_cfg.task_core_id = CONFIG_RTK_CONSOLE_TASK_CORE;

    esp_err_t err = ESP_OK;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    uart_cfg.channel = CONFIG_RTK_CONSOLE_UART_NUM;
    uart_cfg.baud_rate = CONFIG_RTK_CONSOLE_UART_BAUD;
    uart_cfg.tx_gpio_num = -1;
    uart_cfg.rx_gpio_num = -1;
    err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t cdc_cfg = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&cdc_cfg, &repl_cfg, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t jtag_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&jtag_cfg, &repl_cfg, &repl);
#else
    err = esp_console_new_repl_stdio(&repl_cfg, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &entry : s_tables[t]) {
            esp_console_cmd_t command{};
            command.command = entry.name;
            command.help = entry.help;
            command.hint = entry.hint;
            command.func_w_context = entry.handler;
            command.context = entry.ctx;
            err = esp_console_cmd_register(&command);
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "register %s failed: %s", entry.name, esp_err_to_name(err));
                cleanup_repl(repl);
                return err;
            }
        }
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "console start failed: %s", esp_err_to_name(err));
        cleanup_repl(repl);
        return err;
    }

    s_started = true;
    return ESP_OK;
}

} // namespace rtk::console
