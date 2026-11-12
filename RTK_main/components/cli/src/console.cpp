// 控制台组件（ADR-0007 §3.3 / ADR-0009 §2.4-§2.5）：命令表与动词表登记、REPL 启动、统一回执、自定义 help、
// UART TX 环形缓冲。add()/add_verbs()/start() 只在启动任务调用一次；handler 与 trampoline 在 console REPL
// 任务中串行执行，可阻塞，非 ISR 安全。本组件自身不做堆分配（命令表只保存指针），REPL 的堆由 IDF 分配。

#include "cli/console.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "reply_state.hpp"
#include "sdkconfig.h"

namespace cli {

namespace {

constexpr const char *kTag = "cli";
constexpr int kGroupWidth = 12;             // help 列表里组名左对齐宽度
constexpr size_t kConsoleRxBufBytes = 256U; // 与 IDF uart_vfs_dev_port_init 的内部选择保持一致
constexpr size_t kHelpMaxArgs = 2U;         // help [<group>]
constexpr const char *kHelpName = "help";
constexpr const char *kHelpSummary = "show command list, or usage of one group";

// 只保存调用方静态表的 span（pointer + length），不拷贝 Cmd 结构体以节省静态 DRAM。
// 每个非空表至少含 1 条命令，因此非空表数不会超过 kMaxCmds。
std::array<std::span<const Cmd>, kMaxCmds> s_tables{};
size_t s_table_count = 0U;
size_t s_cmd_count = 0U;

// 组 -> 动词表；只供 help 显示，不重复保存命令本身。
struct VerbGroup {
    const char *group = nullptr;
    const char *summary = nullptr;
    std::span<const Verb> verbs{};
};

std::array<VerbGroup, kMaxCmds> s_verb_groups{};
size_t s_verb_group_count = 0U;

// trampoline 需要的 (handler, ctx)；与命令表分开存放，避免为取回 handler 而对 const 命令表做 const_cast。
struct Entry {
    Handler handler = nullptr;
    void *ctx = nullptr;
};

std::array<Entry, kMaxCmds> s_entries{};
size_t s_entry_count = 0U;
Entry s_help_entry{};
bool s_started = false;

bool is_empty_text(const char *text) noexcept
{
    if (text == nullptr) {
        return true;
    }
    return std::strlen(text) == 0U;
}

bool same_text(const char *lhs, const char *rhs) noexcept
{
    if ((lhs == nullptr) || (rhs == nullptr)) {
        return false;
    }
    return std::strcmp(lhs, rhs) == 0;
}

bool has_cmd_name(const char *name) noexcept
{
    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &existing : s_tables[t]) {
            if (same_text(existing.name, name)) {
                return true;
            }
        }
    }
    return false;
}

const Cmd *find_cmd(const char *name) noexcept
{
    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &cmd : s_tables[t]) {
            if (same_text(cmd.name, name)) {
                return &cmd;
            }
        }
    }
    return nullptr;
}

const VerbGroup *find_verb_group(const char *group) noexcept
{
    for (size_t g = 0U; g < s_verb_group_count; ++g) {
        if (same_text(s_verb_groups[g].group, group)) {
            return &s_verb_groups[g];
        }
    }
    return nullptr;
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

int print_help_usage() noexcept
{
    return fail("usage: help [<group>]");
}

void print_group_list() noexcept
{
    // 按登记顺序输出；已登记动词表的组用摘要，否则回落到该命令的单行 help。
    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &cmd : s_tables[t]) {
            const VerbGroup *group = find_verb_group(cmd.name);
            const char *summary = (group != nullptr) ? group->summary : cmd.help;
            std::printf("  %-*s %s\n", kGroupWidth, cmd.name, (summary != nullptr) ? summary : "");
        }
    }
    std::printf("  %-*s %s\n", kGroupWidth, kHelpName, kHelpSummary);
}

// help 由本组件实现：无参列出各组，带参显示该组用法。
// esp_console_common_init() 已注册 IDF 自带的 help，register_help() 先注销再登记本实现。
int cmd_help(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 0) || (argv == nullptr)) {
        return print_help_usage();
    }
    // argc >= 0 已校验，转 size_t 无损。
    const std::span<char *const> args(argv, static_cast<size_t>(argc));
    if (args.size() <= 1U) {
        print_group_list();
        return 0;
    }
    if (args.size() > kHelpMaxArgs) {
        return print_help_usage();
    }
    const char *target = args[1];
    if (same_text(target, kHelpName)) {
        return print_help_usage();
    }
    const VerbGroup *group = find_verb_group(target);
    if (group != nullptr) {
        print_usage(group->group, group->verbs);
        return 0;
    }
    const Cmd *cmd = find_cmd(target);
    if (cmd != nullptr) {
        std::printf("%s\n", (cmd->help != nullptr) ? cmd->help : "");
        return 0;
    }
    return fail("unknown group");
}

// 每条命令的入口：先复位回执状态，再按 handler 返回值统一打印 OK / ERR（ADR-0009 §2.5）。
// 始终返回 0，避免 esp_console 再打印一遍 "Command returned non-zero error code"。
int trampoline(void *ctx, int argc, char **argv) noexcept
{
    const auto *entry = static_cast<const Entry *>(ctx);
    if ((entry == nullptr) || (entry->handler == nullptr)) {
        std::printf("ERR internal: command entry missing\n");
        return 0;
    }
    detail::reply_reset();
    const int rc = entry->handler(entry->ctx, argc, argv);
    if (rc == 0) {
        std::printf("OK\n");
    } else if (!detail::reply_failed()) {
        // handler 没解释原因时只报返回码，保证每条命令最后一行都是 OK / ERR。
        std::printf("ERR %d\n", rc);
    }
    return 0;
}

esp_err_t register_help() noexcept
{
    s_help_entry = Entry{cmd_help, nullptr};
    esp_console_cmd_t command{};
    command.command = kHelpName;
    command.help = kHelpSummary;
    command.hint = "<group>";
    command.func_w_context = trampoline;
    command.context = &s_help_entry;
    // 必须先注销 IDF 自带的 help：IDF v6.1 commands.c:156-160 同名覆盖时不清空旧的 func，
    // 结果 func 与 func_w_context 都会被调用（:257-260），help 输出两遍。
    const esp_err_t err = esp_console_cmd_deregister(kHelpName);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_ARG)) {
        return err;
    }
    return esp_console_cmd_register(&command);
}

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
/**
 * 用带 TX 环形的 UART 驱动替换 esp_console 装的 0 字节 TX 驱动（ADR-0009 §2.4）。
 * 经典 ESP32 没有 UHCI/UART DMA，长输出只能靠大 TX 环形缓冲 + 中断搬运，避免命令任务阻塞在 FIFO 上。
 * 调用点：esp_console_new_repl_uart() 之后、esp_console_start_repl() 之前，console 任务还在等通知，不会读写 UART。
 */
esp_err_t enable_uart_tx_buffer() noexcept
{
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
    // Kconfig range 0..2 保证端口号合法；uart_port_t 是枚举，转 static_cast 不改变数值。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM);
    esp_err_t err = uart_driver_delete(port);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_driver_delete failed: %s", esp_err_to_name(err));
        return err;
    }

    uart_config_t cfg{};
    cfg.baud_rate = CONFIG_RTK_CONSOLE_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
#if SOC_UART_SUPPORT_REF_TICK
    // 与 IDF uart_vfs_dev_port_init 的选择一致：<= 1 MHz 用 REF_TICK，更高波特率必须用 APB（DEFAULT）。
    cfg.source_clk = (cfg.baud_rate > 1000000) ? UART_SCLK_DEFAULT : UART_SCLK_REF_TICK;
#else
    cfg.source_clk = UART_SCLK_DEFAULT;
#endif
    err = uart_param_config(port, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 控制台使用默认 UART 引脚（IOMUX 直连），这里不重路由：重路由反而会覆盖 ROM/console 的既有配置。
    err = uart_set_pin(port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err =
        uart_driver_install(port, static_cast<int>(kConsoleRxBufBytes), CONFIG_RTK_CONSOLE_UART_TX_BUF, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // VFS 仍走驱动，行尾转换重新声明一遍，保证换驱动后行为不变（两处调用都是幂等的）。
    uart_vfs_dev_use_driver(port);
    if (uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_CR) != 0) {
        ESP_LOGE(kTag, "set rx line endings failed");
        return ESP_FAIL;
    }
    if (uart_vfs_dev_port_set_tx_line_endings(port, ESP_LINE_ENDINGS_CRLF) != 0) {
        ESP_LOGE(kTag, "set tx line endings failed");
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}
#endif

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
        if (is_empty_text(cmd.name) || is_empty_text(cmd.help) || (cmd.handler == nullptr)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (has_cmd_name(cmd.name)) {
            return ESP_ERR_INVALID_STATE;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (same_text(cmds[j].name, cmd.name)) {
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

esp_err_t add_verbs(const char *group, const char *summary, std::span<const Verb> verbs) noexcept
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (is_empty_text(group) || is_empty_text(summary) || verbs.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const Verb &verb : verbs) {
        if ((verb.name == nullptr) || (verb.handler == nullptr)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (s_verb_group_count >= kMaxCmds) {
        return ESP_ERR_NO_MEM;
    }
    s_verb_groups[s_verb_group_count].group = group;
    s_verb_groups[s_verb_group_count].summary = summary;
    s_verb_groups[s_verb_group_count].verbs = verbs;
    ++s_verb_group_count;
    return ESP_OK;
}

esp_err_t start(const char *prompt) noexcept
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (is_empty_text(prompt)) {
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
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "console init failed: %s", esp_err_to_name(err));
        return err;
    }
    // 此处 UART 驱动已由 uart_vfs_dev_port_init() 以 0 字节 TX 缓冲装好，且 console 任务尚未启动。
    err = enable_uart_tx_buffer();
    if (err != ESP_OK) {
        cleanup_repl(repl);
        return err;
    }
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

    err = register_help();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "register help failed: %s", esp_err_to_name(err));
        cleanup_repl(repl);
        return err;
    }

    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &entry : s_tables[t]) {
            esp_console_cmd_t command{};
            command.command = entry.name;
            command.help = entry.help;
            command.hint = entry.hint;
            command.func_w_context = trampoline;
            // context 指向静态 Entry（不是命令表条目），trampoline 由它取回真正的 handler 与 ctx。
            s_entries[s_entry_count] = Entry{entry.handler, entry.ctx};
            command.context = &s_entries[s_entry_count];
            ++s_entry_count;
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

} // namespace cli
