// 控制台组件（ADR-0007 §3.3 / ADR-0009 §2.4-§2.5）：命令表与动词表登记、REPL 启动、统一回执、自定义 help、
// UART TX 环形缓冲。add()/addVerbs()/start() 只在启动任务调用一次；handler 与 trampoline 在 console REPL
// 任务中串行执行，可阻塞，非 ISR 安全。本组件自身不做堆分配（命令表只保存指针），REPL 的堆由 IDF 分配。

#include "cli/Console.hpp"
#include "cli/Out.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_console.h"
#include "esp_err.h"
#include "fw/Log.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ReplyState.hpp"
#include "sdkconfig.h"

namespace cli {

namespace {

constexpr int kGroupWidth = 12;             // help 列表里组名左对齐宽度
constexpr size_t kConsoleRxBufBytes = 256U; // 与 IDF uart_vfs_dev_port_init 的内部选择保持一致
constexpr uint32_t kTxDrainTimeoutMs =
    50U;                                 // 128 B 硬件 FIFO 在 2 Mbaud 下约 0.64 ms 发完，留足余量
constexpr int kRefTickMaxBaud = 1000000; // IDF uart_vfs_dev_port_init 的 REF_TICK/APB 分界
constexpr size_t kHelpMaxArgs = 2U;      // help [<group>]
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
bool s_started = false; // init() 成功后置位：命令表封存，add/addVerbs 不再接受
esp_console_repl_t *s_repl =
    nullptr; // init() 创建、run() 启动；REPL 任务在 run 之前阻塞在任务通知上
bool s_running = false;

bool isEmptyText(const char *text) noexcept
{
    if (text == nullptr) {
        return true;
    }
    return std::strlen(text) == 0U;
}

bool sameText(const char *lhs, const char *rhs) noexcept
{
    if ((lhs == nullptr) || (rhs == nullptr)) {
        return false;
    }
    return std::strcmp(lhs, rhs) == 0;
}

bool hasCmdName(const char *name) noexcept
{
    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &existing : s_tables[t]) {
            if (sameText(existing.name, name)) {
                return true;
            }
        }
    }
    return false;
}

const Cmd *findCmd(const char *name) noexcept
{
    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &cmd : s_tables[t]) {
            if (sameText(cmd.name, name)) {
                return &cmd;
            }
        }
    }
    return nullptr;
}

const VerbGroup *findVerbGroup(const char *group) noexcept
{
    for (size_t g = 0U; g < s_verb_group_count; ++g) {
        if (sameText(s_verb_groups[g].group, group)) {
            return &s_verb_groups[g];
        }
    }
    return nullptr;
}

/**
 * @brief  释放 REPL 失败路径上已创建的对象
 * @param  repl: esp_console REPL 句柄；nullptr 时忽略
 */
void cleanupRepl(esp_console_repl_t *repl) noexcept
{
    if (repl == nullptr) {
        return;
    }
    const esp_err_t cleanupErr = repl->del(repl);
    if (cleanupErr != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "console cleanup failed: %s", esp_err_to_name(cleanupErr));
    }
}

int printHelpUsage() noexcept
{
    return fail("usage: help [<group>]");
}

void printGroupList() noexcept
{
    // 按登记顺序输出；已登记动词表的组用摘要，否则回落到该命令的单行 help。
    for (size_t t = 0U; t < s_table_count; ++t) {
        for (const Cmd &cmd : s_tables[t]) {
            if (cmd.hidden) {
                continue; // 隐藏命令只在知道名字时使用（ADR-0009 §2.10）
            }
            const VerbGroup *group = findVerbGroup(cmd.name);
            const char *summary = (group != nullptr) ? group->summary : cmd.help;
            outPrintf("  %-*s %s\n", kGroupWidth, cmd.name, (summary != nullptr) ? summary : "");
        }
    }
    outPrintf("  %-*s %s\n", kGroupWidth, kHelpName, kHelpSummary);
}

// help 由本组件实现：无参列出各组，带参显示该组用法。
// esp_console_common_init() 已注册 IDF 自带的 help，registerHelp() 先注销再登记本实现。
int cmdHelp(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 0) || (argv == nullptr)) {
        return printHelpUsage();
    }
    // argc >= 0 已校验，转 size_t 无损。
    const std::span<char *const> args(argv, static_cast<size_t>(argc));
    if (args.size() <= 1U) {
        printGroupList();
        return 0;
    }
    if (args.size() > kHelpMaxArgs) {
        return printHelpUsage();
    }
    const char *target = args[1];
    if (sameText(target, kHelpName)) {
        return printHelpUsage();
    }
    const VerbGroup *group = findVerbGroup(target);
    if (group != nullptr) {
        printUsage(group->group, group->verbs);
        return 0;
    }
    const Cmd *cmd = findCmd(target);
    if (cmd != nullptr) {
        outPrintf("%s\n", (cmd->help != nullptr) ? cmd->help : "");
        return 0;
    }
    return fail("unknown group");
}

// 每条命令的入口：先复位回执状态，再按 handler 返回值统一打印 OK / ERR（ADR-0009 §2.5）。
// 始终返回 0，避免 esp_console 再打印一遍 "Command returned non-zero error code"。
// NOLINTNEXTLINE(misc-const-correctness): esp_console_cmd_func_t 固定为 void *ctx 回调签名
int trampoline(void *ctx, int argc, char **argv) noexcept
{
    const auto *entry = static_cast<const Entry *>(ctx);
    if ((entry == nullptr) || (entry->handler == nullptr)) {
        outPrintf("ERR internal: command entry missing\n");
        return 0;
    }
    detail::replyReset();
    const int rc = entry->handler(entry->ctx, argc, argv);
    if (rc == 0) {
        outPrintf("OK\n");
    } else if (!detail::replyFailed()) {
        // handler 没解释原因时只报返回码，保证每条命令最后一行都是 OK / ERR。
        outPrintf("ERR %d\n", rc);
    }
    return 0;
}

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
/**
 * 输出端：一次 uart_write_bytes 写完整段（ADR-0009 §2.8）。只有装了 TX 环形驱动才安装它；
 * 环形满时阻塞到有空间，调用方都是任务上下文。
 */
void uartSink(const char *text, size_t len) noexcept
{
    (void)uart_write_bytes(static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM), text, len);
}

vprintf_like_t s_orig_log_vprintf = nullptr;

/**
 * ESP_LOG 钩子：日志与命令回复走同一出口和同一把锁，整行写出、互不穿插。
 * ISR、调度器未运行或同任务重入时回落到原 vprintf（IDF 的 VFS 路径）。
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

void installUartSink() noexcept
{
    // 绕过 VFS 后由 out 模块负责 "\n" → "\r\n"，与 VFS 的 TX 行尾设置（CRLF）一致。
    setOutSink(uartSink, true);
    s_orig_log_vprintf = esp_log_set_vprintf(logVprintf);
}
#endif
#endif

esp_err_t registerHelp() noexcept
{
    s_help_entry = Entry{cmdHelp, nullptr};
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
 * @brief  用带 TX 环形的 UART 驱动替换 esp_console 装的 0 字节 TX 驱动（ADR-0009 §2.4）
 * @retval ESP_OK 成功；其他为 UART 驱动删除/安装失败码
 * @note   经典 ESP32 没有 UART DMA，长输出靠大 TX 环形 + 中断搬运，避免命令任务阻塞在 FIFO 上。
 *       调用点：esp_console_new_repl_uart() 之后、esp_console_start_repl() 之前，console 任务尚未读写 UART。
 */
esp_err_t enableUartTxBuffer() noexcept
{
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
    // Kconfig range 0..2 保证端口号合法；uart_port_t 是枚举，转 static_cast 不改变数值。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM);
    // uart_param_config() 会复位 TX FIFO（uart_hal_txfifo_rst）：先等已写入 FIFO 的启动日志发完，
    // 否则切换瞬间尚未移出的字节被丢弃，表现为启动日志行被截断（R8a HIL 观察到 "W (260) uart_vfs: light slee"）。
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): IDF 以匿名 union 提供时钟字段，只能直接访问
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
        if (isEmptyText(cmd.name) || isEmptyText(cmd.help) || (cmd.handler == nullptr)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (hasCmdName(cmd.name)) {
            return ESP_ERR_INVALID_STATE;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (sameText(cmds[j].name, cmd.name)) {
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

esp_err_t addVerbs(const char *group, const char *summary, std::span<const Verb> verbs) noexcept
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (isEmptyText(group) || isEmptyText(summary) || verbs.empty()) {
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

esp_err_t init(const char *prompt) noexcept
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (isEmptyText(prompt)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t replCfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    replCfg.prompt = prompt;
    replCfg.max_cmdline_length = CONFIG_RTK_CONSOLE_CMDLINE_MAX;
    replCfg.task_stack_size = CONFIG_RTK_CONSOLE_STACK_SIZE;
    replCfg.task_priority = CONFIG_RTK_CONSOLE_TASK_PRIO;
    // 默认 tskNO_AFFINITY；这里显式绑到配置核（默认 core 0，链路任务独占 core 1）。
    replCfg.task_core_id = CONFIG_RTK_CONSOLE_TASK_CORE;

    esp_err_t err = ESP_OK;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    // 控制台固定 2 Mbaud（> 1 MHz 只能用 APB 时钟）；本固件不用 UART light-sleep 唤醒，
    // uart_vfs 每次启动打印的 "light sleep UART wakeup might not work" 告警无意义，只保留其错误级日志。
    esp_log_level_set("uart_vfs", ESP_LOG_ERROR);
    esp_console_dev_uart_config_t uartCfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    uartCfg.channel = CONFIG_RTK_CONSOLE_UART_NUM;
    uartCfg.baud_rate = CONFIG_RTK_CONSOLE_UART_BAUD;
    uartCfg.tx_gpio_num = -1;
    uartCfg.rx_gpio_num = -1;
    err = esp_console_new_repl_uart(&uartCfg, &replCfg, &repl);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "console init failed: %s", esp_err_to_name(err));
        return err;
    }
    // 此处 UART 驱动已由 uart_vfs_dev_port_init() 以 0 字节 TX 缓冲装好，且 console 任务尚未启动。
    err = enableUartTxBuffer();
    if (err != ESP_OK) {
        cleanupRepl(repl);
        return err;
    }
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
    installUartSink();
#endif
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t cdcCfg = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&cdcCfg, &replCfg, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t jtagCfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&jtagCfg, &replCfg, &repl);
#else
    err = esp_console_new_repl_stdio(&replCfg, &repl);
#endif
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = registerHelp();
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "register help failed: %s", esp_err_to_name(err));
        cleanupRepl(repl);
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
                FW_LOGE(fw::TraceCat::kSys, "register %s failed: %s", entry.name,
                        esp_err_to_name(err));
                cleanupRepl(repl);
                return err;
            }
        }
    }

    s_repl = repl;
    s_started = true;
    return ESP_OK;
}

esp_err_t run() noexcept
{
    if ((s_repl == nullptr) || s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    // 只是通知 init() 创建的 REPL 任务开始：此后它打印 IDF 提示语与 prompt 并读取输入。
    const esp_err_t err = esp_console_start_repl(s_repl);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "console start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_running = true;
    return ESP_OK;
}

esp_err_t start(const char *prompt) noexcept
{
    const esp_err_t err = init(prompt);
    return (err != ESP_OK) ? err : run();
}

} // namespace cli

// 未知命令也要以 ERR 结尾（ADR-0009 §2.5）：IDF v6.1 REPL 循环（console/esp_console_common.c:93-94）
// 对 ESP_ERR_NOT_FOUND 硬编码打印 "Unrecognized command"，没有回调可挂。CMakeLists 用
// -Wl,--wrap=esp_console_run 把 REPL 的调用引到这里：未找到时自己输出 "ERR unknown command"，
// 并返回 ESP_ERR_INVALID_ARG（REPL 按“空行”处理，不再追加输出）。其余结果原样透传。
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming): --wrap 规定 __real_/__wrap_ 前缀
extern "C" esp_err_t __real_esp_console_run(const char *cmdline, int *cmdRet);
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming): 同上
extern "C" esp_err_t __wrap_esp_console_run(const char *cmdline, int *cmdRet);

extern "C" esp_err_t __wrap_esp_console_run(const char *cmdline, int *cmdRet)
{
    const esp_err_t err = __real_esp_console_run(cmdline, cmdRet);
    if (err == ESP_ERR_NOT_FOUND) {
        cli::outPrintf("ERR unknown command\n");
        return ESP_ERR_INVALID_ARG;
    }
    return err;
}
