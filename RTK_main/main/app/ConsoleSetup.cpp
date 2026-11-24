// 控制台装配与启动阶段门控（ADR-0009）：命令表/动词表在 REPL 启动前一次性登记。
// 线程：initConsole/runConsole 只在启动任务调用，gatedCommand 只在 console REPL 任务调用；非 ISR，不分配堆。
#include "modules/bt_link/BtPairing.hpp"
#include "modules/system/FactoryReset.hpp"
#include "app/App.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"

#include "AppContext.hpp"
#include "app/ConsoleRegistration.hpp"
#include "app/PowerRequests.hpp"
#include "cli/Out.hpp"
#include "fw/Log.hpp"
#include "modules/ota/OtaCmds.hpp"
#include "rtos/EspStatus.hpp"

namespace {

#if defined(ESP_PLATFORM)
constinit std::array<app::GatedCommand, cli::kMaxCmds> s_command_gates{};
constinit std::array<cli::Cmd, cli::kMaxCmds> s_gated_commands{};

#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE)
constexpr bool kWifiConfigEnabled = true;
#else
constexpr bool kWifiConfigEnabled = false;
#endif

/**
 * @brief 显示本次启动的射频选择和原因
 * @param argc: 参数个数，含动词
 * @retval 0 已输出状态
 * @retval 非零 参数错误
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
int cmdRadioStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: radio");
    }
    const auto &self = app::appContext();
    cli::outPrintf("radio=%s reason=%s\n", app::radioModeName(self.radioMode),
                   app::radioReasonName(self.radioReason));
    return 0;
}

/**
 * @brief 回执发出后请求重启到指定射频模式
 * @param argc: 参数个数，含动词
 * @retval 非零 参数错误
 * @note 仅 console REPL 任务，可等待 UART，非 ISR。参数有效时重启，UART 错误只记录告警。
 */
template <app::RadioMode Mode>
int cmdRadioSwitch(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: radio %s", app::radioModeName(Mode));
    }
    if (!app::power::s_power_requests.claimRestart()) {
        return cli::fail("power action already in progress");
    }
    constexpr uint32_t kDrainTimeoutMs = 500U;
    constexpr uint32_t kAckTimeoutMs = 50U;
    // Kconfig 限定 UART 号 0..2，转换后仍是有效端口。
    constexpr auto kPort = static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM);
    const auto drain = [](uint32_t timeoutMs) noexcept {
        const esp_err_t error = uart_wait_tx_done(kPort, pdMS_TO_TICKS(timeoutMs));
        if (error != ESP_OK) {
            FW_LOGW(fw::TraceCat::kSys, "UART drain failed: %s", esp_err_to_name(error));
        }
        return error == ESP_OK;
    };
    const bool drained = drain(kDrainTimeoutMs);
    cli::outPrintf("OK\n");
    if (drained) {
        // OK 经 CLI 的 TX 环写入，重启前给回执单独留出发送时间。
        static_cast<void>(drain(kAckTimeoutMs));
    }
    app::requestRadioMode(Mode, app::RadioReason::kConsole);
}

constexpr std::array kRadioVerbs{
    cli::Verb{"status", nullptr, "show current mode and reason", &cmdRadioStatus},
    cli::Verb{"wifi", nullptr, "restart in Wi-Fi configuration mode",
              &cmdRadioSwitch<app::RadioMode::kWifiConfig>},
    cli::Verb{"bt", nullptr, "restart in Bluetooth mode",
              &cmdRadioSwitch<app::RadioMode::kBluetooth>},
};

/**
 * @brief 分派射频模式命令，缺省显示当前选择
 * @param ctx: 控制台上下文
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 查询成功
 * @retval 非零 参数错误
 * @note 仅 console REPL 任务，可阻塞，非 ISR。成功切换时重启。
 */
int cmdRadio(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kRadioVerbs, ctx, argc, argv, "status");
}

/**
 * @brief 取得射频控制台命令表
 * @retval 固件生命周期有效的静态表
 * @note 仅启动任务登记时调用，不阻塞，非 ISR。
 */
std::span<const cli::Cmd> radioCmds() noexcept
{
    static constexpr std::array kCommands{
        cli::Cmd{"radio", "radio [wifi|bt]: show or switch radio mode", "[wifi|bt]", &cmdRadio,
                 nullptr},
    };
    return kCommands;
}

/**
 * @brief 取得射频命令的 help 动词表
 * @retval 固件生命周期有效的静态表
 * @note 仅启动任务登记时调用，不阻塞，非 ISR。
 */
std::span<const cli::Verb> radioVerbs() noexcept
{
    return kRadioVerbs;
}

/**
 * @brief  组件日志直写 sink（ADR-0009 §2.12）：文本环未安装时（启动早期、追踪关闭的固件）同步写控制台
 * @param  ctx: 未用
 * @param  hdr: 记录头
 * @param  payload: 文本（不含换行）
 * @note   任意任务调用，非 ISR；格式化缓冲在调用方栈上（kLogLineMax 字节），cli::outWrite 内部加锁。
 */
void directLogSink(void *ctx, // NOLINT(misc-const-correctness): LogSinkFn 固定为 void *ctx
                   const fw::LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    static_cast<void>(ctx);
    std::array<char, fw::kLogLineMax> line{};
    const size_t len = fw::logFormatLine(line, hdr, payload);
    cli::outWrite(line.data(), len);
}
#endif

} // namespace

namespace app {

/**
 * @brief 按启动阶段放行或拒绝控制台命令
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 handler 成功
 * @retval 非零 启动门控拒绝或 handler 失败
 * @note 仅 console REPL 任务；可能随 handler 阻塞，非 ISR。
 */
int gatedCommand(void *ctx, // NOLINT(misc-const-correctness): cli::Handler 固定 ABI 要求 void *ctx
                 int argc, char **argv) noexcept
{
    const auto &gate = *static_cast<const GatedCommand *>(ctx); // ctx 只指向组合根的静态命令 gate。
    const auto phase = appContext().bootPhase.load(std::memory_order_acquire);
    if ((phase == BootPhase::kStarting) || (phase == BootPhase::kStopping)) {
        return cli::fail("unavailable: booting");
    }
    if ((phase == BootPhase::kConsoleOnly) && !gate.safe) {
        return cli::fail("unavailable: console-only");
    }
    return gate.handler(gate.ctx, argc, argv);
}

void setConsoleOnly() noexcept
{
    appContext().bootPhase.store(BootPhase::kConsoleOnly, std::memory_order_release);
}

#if defined(ESP_PLATFORM)
bool installDirectLog() noexcept
{
    // 早于控制台 UART 重装：此前 cli::outWrite 走 stdout（IDF 默认 UART0 VFS），之后走 UART 驱动，行格式不变。
    return fw::logInstallDirect(&directLogSink, nullptr, &rtos::nowMs) == fw::Result::kOk;
}

/** help 分组用的动词表条目；名字与摘要同样来自各模块的命令组。 */
struct VerbGroupDef {
    const char *name;
    const char *summary;
    std::span<const cli::Verb> (*verbs)() noexcept;
};

/**
 * @brief 查询名字表中启用的登记项
 * @param names: 按构建档位生成的名字表，空项标记为禁用
 * @param name: 待查询的命令或动词组名
 * @retval true 名字有效且当前档位启用
 * @retval false 名字为空或未启用
 * @note 纯查询，可在任意任务或 ISR 调用。
 */
constexpr bool isEnabled(std::span<const std::string_view> names, std::string_view name) noexcept
{
    return !name.empty() && std::ranges::find(names, name) != names.end();
}

/**
 * @brief 把命令表中启用的命令追加到启动阶段门控表
 * @param table: 模块提供的原始命令表
 * @param names: 当前档位的命令名字表
 * @param[out] used: 已写入的条目数，容量不足时保留已写入的前缀
 * @retval true 表中启用的命令已全部追加
 * @retval false 门控表容量不足
 * @note 仅启动任务在 REPL 放行前调用，非 ISR，不阻塞。
 */
bool appendGatedCommands(std::span<const cli::Cmd> table, std::span<const std::string_view> names,
                         size_t &used) noexcept
{
    for (const auto &command : table) {
        if (!isEnabled(names, command.name)) {
            continue;
        }
        if (used >= s_gated_commands.size()) {
            return false;
        }
        s_command_gates[used] = {command.handler, command.ctx,
                                 console_registration::isSafe(command.name)};
        s_gated_commands[used] = command;
        s_gated_commands[used].handler = &gatedCommand;
        s_gated_commands[used].ctx = &s_command_gates[used];
        ++used;
    }
    return true;
}

/**
 * @brief 登记当前档位的命令，并把 handler 包进 phase 门控
 * @retval true 登记成功
 * @retval false 表超容量或 cli::add 失败
 * @note 仅启动期单线程调用；输出通道先于业务堆分配建立。
 */
bool registerCommandTables() noexcept
{
    constexpr auto kNames = console_registration::commandNames(
        kConsoleProfile, RTK_GNSS_ENABLED, FW_TRACE_ENABLED, kWifiConfigEnabled);
    constexpr auto kEnabled = [names = kNames](std::string_view name) {
        return isEnabled(names, name);
    };
    // 在编译时去掉关闭的表引用，使链接器可以回收对应处理函数。
    constexpr std::array kCmdTables{
        (kEnabled("status") || kEnabled("log") || kEnabled("sys")) ? &systemCmds : nullptr,
        (kEnabled("bt") || kEnabled("alarm")) ? &linkCmds : nullptr,
        kEnabled("config") ? &configCmds : nullptr,
        kEnabled("rtcm") ? &rtcmCmds : nullptr,
#if RTK_GNSS_ENABLED
        kEnabled("gnss") ? &gnssCmds : nullptr,
#endif
#if FW_TRACE_ENABLED
        kEnabled("debug") ? &debugCmds : nullptr,
#endif
        kEnabled("factory") ? &factoryCmds : nullptr,
        kEnabled("freset") ? &factoryResetCmds : nullptr,
        kEnabled("power") ? &powerCmds : nullptr,
        kEnabled("radio") ? &radioCmds : nullptr,
        kEnabled("ota") ? &otaCmds : nullptr,
    };
    // 命令表地址在 init 后不可变；handler 经 phase 原子门控。
    size_t used = 0U;
    for (const auto table : kCmdTables) {
        if (table != nullptr && !appendGatedCommands(table(), kNames, used)) {
            return false;
        }
    }
    if (cli::add(std::span{s_gated_commands}.first(used)) != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "console add failed");
        return false;
    }
    return true;
}

/**
 * @brief 登记当前档位的动词组供 help / help <group> 展示
 * @retval true 登记成功
 * @retval false cli::addVerbs 失败（已记日志）
 * @note 仅启动期单线程调用；必须在 cli::init 之前完成。
 */
bool registerVerbGroups() noexcept
{
    constexpr auto kNames = console_registration::verbGroupNames(
        kConsoleProfile, RTK_GNSS_ENABLED, FW_TRACE_ENABLED, kWifiConfigEnabled);
    constexpr auto kGroup = [names = kNames](const char *name, const char *summary, auto verbs) {
        return isEnabled(names, name) ? VerbGroupDef{name, summary, verbs} : VerbGroupDef{};
    };
    constexpr std::array kVerbGroups{
        kGroup("status", "system and link status (default overview)", &statusVerbs),
        kGroup("log", "runtime IDF log level", &logVerbs),
        kGroup("sys", "system info, heap, mem, tasks, boot and reboot", &sysVerbs),
        kGroup("rtcm", "RTCM forwarding stats and reset", &rtcmVerbs),
#if RTK_GNSS_ENABLED
        kGroup("gnss", "GNSS receiver control and telemetry", &gnssVerbs),
#endif
#if FW_TRACE_ENABLED
        kGroup("debug", "trace categories, report and NVS", &debugVerbs),
#endif
        kGroup("bt", "Bluetooth pairing and alarm trust", &btVerbs),
        kGroup("factory", "production settings (hidden, needs unlock)", &factoryVerbs),
        kGroup("power", "power key status and clean shutdown", &powerVerbs),
        kGroup("radio", "radio mode and restart", &radioVerbs),
        kGroup("ota", "application OTA and rollback", &otaVerbs),
    };
    for (const auto &group : kVerbGroups) {
        if (group.verbs == nullptr) {
            continue;
        }
        const esp_err_t verbsErr = cli::addVerbs(group.name, group.summary, group.verbs());
        if (verbsErr != ESP_OK) {
            FW_LOGE(fw::TraceCat::kSys, "console verbs failed: %s", esp_err_to_name(verbsErr));
            return false;
        }
    }
    return true;
}

bool initConsole() noexcept
{
    constexpr const char *kConsolePrompt = "console> ";
    if (!registerCommandTables() || !registerVerbGroups()) {
        return false;
    }
    const esp_err_t consoleErr = cli::init(kConsolePrompt);
    if (consoleErr != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "console init failed: %s", esp_err_to_name(consoleErr));
        return false;
    }
    // 蓝牙控制器与 PHY 的 IDF 启动 INFO 已由 "Bluetooth online" 摘要行覆盖；需要时可用 `log` 命令调回。
    esp_log_level_set("BTDM_INIT", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    // app_main 返回时 IDF 打印 "Returned from app_main()"，此时提示符已出，会接在提示符后面。
    esp_log_level_set("main_task", ESP_LOG_WARN);
    return true;
}

void runConsole() noexcept
{
    // init 失败（降级路径）时 run 返回 INVALID_STATE：没有可用控制台，只能记录。
    const esp_err_t err = cli::run();
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "console run failed: %s", esp_err_to_name(err));
    }
}
#endif // ESP_PLATFORM

} // namespace app
