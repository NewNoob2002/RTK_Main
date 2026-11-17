// 控制台组件（ADR-0007 §3.3）：命令表/动词表登记、REPL 启动、统一回执、自定义 help、UART TX 环形缓冲。
// 线程：add()/addVerbs()/init()/run() 只在启动任务各调一次；handler 与 trampoline 在 console 任务串行执行，
//       可阻塞，非 ISR 安全。堆：本组件只保存指针、不分配堆；REPL 任务栈与命令行缓冲由 IDF 分配。

#include "cli/Console.hpp"
#include "cli/Out.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fw/Log.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ConsoleUart.hpp"
#include "ReplyState.hpp"
#include "sdkconfig.h"

namespace cli {

namespace {

constexpr int kGroupWidth = 12;     // help 列表里组名左对齐宽度
constexpr size_t kHelpMaxArgs = 2U; // help [<group>]
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

/**
 * @brief  串为 nullptr 或空串时返回 true
 * @param  text: 待检查的 NUL 结尾串
 * @retval true  text 为 nullptr 或长度为 0
 * @retval false 其余情况
 */
bool isEmptyText(const char *text) noexcept
{
    if (text == nullptr) {
        return true;
    }
    return std::strlen(text) == 0U;
}

/**
 * @brief  比较两个 NUL 结尾串
 * @param  lhs: 左串
 * @param  rhs: 右串
 * @retval true  两串内容相同
 * @retval false 内容不同，或任一为 nullptr
 */
bool sameText(const char *lhs, const char *rhs) noexcept
{
    if ((lhs == nullptr) || (rhs == nullptr)) {
        return false;
    }
    return std::strcmp(lhs, rhs) == 0;
}

/**
 * @brief  已登记的命令表里是否有同名命令（add() 的重名检查）
 * @param  name: 待查名字
 * @retval true  已登记
 * @retval false 未登记
 */
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

/**
 * @brief  按名字在已登记命令表中查找
 * @param  name: 待查名字
 * @retval 找到时返回表内条目指针（静态存储，不拷贝）
 * @retval nullptr 未找到
 */
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

/**
 * @brief  按组名查找已登记的动词表
 * @param  group: 组名
 * @retval 找到时返回表内条目指针
 * @retval nullptr 未登记
 */
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

/**
 * @brief  打印 help 的用法
 * @retval kCmdFail 总是返回它（fail() 的返回值），由 trampoline 决定是否补打错误行
 */
int printHelpUsage() noexcept
{
    return fail("usage: help [<group>]");
}

/**
 * @brief  按登记顺序列出可见命令组；有动词表的用组摘要，否则用命令的单行 help
 * @note   隐藏命令（Cmd::hidden）不列出；末尾固定补一行 help 自身（ADR-0009 §2.10）。
 */
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

/**
 * @brief  help 命令实现：无参列出各组，带参显示该组用法
 * @param  ctx: 控制台上下文（未使用）
 * @param  argc: 参数个数
 * @param  argv: 参数数组
 * @retval 0 成功；其他为用法错误时的返回值
 * @note   由本组件实现：esp_console_common_init() 已注册 IDF 自带的 help，registerHelp() 先注销再登记。
 */
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

// NOLINTBEGIN(misc-const-correctness): esp_console_cmd_func_t 固定为 void *ctx 回调签名
/**
 * @brief  每条命令的统一入口：复位回执状态、调用 handler，再按返回值打印 OK / ERR
 * @param  ctx: 指向静态 Entry（handler + 模块 ctx），由 init() 登记
 * @param  argc: 参数个数
 * @param  argv: 参数数组
 * @retval 0 总是返回它，避免 esp_console 再打印 "Command returned non-zero error code"
 * @note   在 console REPL 任务中串行执行（ADR-0009 §2.5）；可能阻塞在输出上，不分配堆。
 */
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
// NOLINTEND(misc-const-correctness)

/**
 * @brief  注册自定义 help 命令（先注销 IDF 自带的同名命令）
 * @retval ESP_OK 成功
 * @retval 其他 注销或注册返回的 esp_err_t（ESP_ERR_INVALID_ARG 的注销结果视为未注册，继续登记）
 * @note   只在 init() 中调用，此时命令表已封存、REPL 任务尚未启动；不分配堆。
 */
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

// 线性初始化序列，拆开反而难读（ADR-0013 §2.4）。
esp_err_t init(const char *prompt) noexcept // NOLINT(readability-function-*): 线性序列
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
    err = detail::enableUartTxBuffer();
    if (err != ESP_OK) {
        cleanupRepl(repl);
        return err;
    }
#if CONFIG_RTK_CONSOLE_UART_TX_BUF > 0
    detail::installUartSink();
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

/**
 * @brief  --wrap 后的 esp_console_run：未知命令也以 "ERR unknown command" 结尾
 * @param  cmdline: 命令行文本
 * @param[out] cmdRet: 命令返回码，由原函数写入；未知命令时不改动
 * @retval 原函数的返回值
 * @retval ESP_ERR_INVALID_ARG 原函数返回 ESP_ERR_NOT_FOUND 时的替换值：已打印 "ERR unknown command"，
 *         REPL 按“空行”处理，不再追加输出
 * @note   C ABI 链接接缝：CMakeLists 用 -Wl,--wrap=esp_console_run 把 REPL 的调用引到这里；
 *         在 console REPL 任务中调用，输出可能阻塞。
 */
extern "C" esp_err_t __wrap_esp_console_run(const char *cmdline, int *cmdRet)
{
    const esp_err_t err = __real_esp_console_run(cmdline, cmdRet);
    if (err == ESP_ERR_NOT_FOUND) {
        cli::outPrintf("ERR unknown command\n");
        return ESP_ERR_INVALID_ARG;
    }
    return err;
}
