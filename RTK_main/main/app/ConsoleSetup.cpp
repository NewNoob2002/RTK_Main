// 控制台装配与启动阶段门控（R8a，自 AppMain.cpp 迁入）：命令表/动词表在 REPL 启动前一次性登记。
// 线程：initConsole/runConsole 只在启动任务调用，gatedCommand 只在 console REPL 任务调用；非 ISR，不分配堆。
#include "app/App.hpp"

#include <array>
#include <span>
#include <string_view>

#include "esp_err.h"
#include "esp_log.h"

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"

namespace {

#if defined(ESP_PLATFORM)
constinit std::array<app::GatedCommand, cli::kMaxCmds> s_command_gates{};
constinit std::array<cli::Cmd, cli::kMaxCmds> s_gated_commands{};

/**
 * @brief  组件日志直写 sink（ADR-0009 §2.12）：文本环未安装时（启动早期、追踪关闭的固件）同步写控制台
 * @param  ctx: 未用
 * @param  hdr: 记录头
 * @param  payload: 文本（不含换行）
 * @note   任意任务调用；格式化缓冲在调用方栈上（kLogLineMax 字节），cli::outWrite 内部加锁。
 */
// NOLINTNEXTLINE(misc-const-correctness): LogSinkFn 固定为 void *ctx
void directLogSink(void *ctx, const fw::LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    static_cast<void>(ctx);
    std::array<char, fw::kLogLineMax> line{};
    const size_t len = fw::logFormatLine(line, hdr, payload);
    cli::outWrite(line.data(), len);
}
#endif

} // namespace

namespace app {

// NOLINTNEXTLINE(misc-const-correctness): cli::Handler 的固定 ABI 要求 void*，gate 本身只读。
int gatedCommand(void *ctx, int argc, char **argv) noexcept
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
    std::span<const cli::Verb> verbs;
};

/**
 * @brief 登记全部命令表，并把 handler 包进 phase 门控
 * @retval true: 全部登记成功；false: 表超容量或 cli::add 失败（已记日志）
 * @note 仅启动期单线程调用；输出通道先于业务堆分配建立。
 */
bool registerCommandTables() noexcept
{
    const std::array cmdTables{
        systemCmds(),  linkCmds(), configCmds(), rtcmCmds(),
#if RTK_GNSS_ENABLED
        gnssCmds(),
#endif
#if FW_TRACE_ENABLED
        debugCmds(),
#endif
        factoryCmds(),
    };
    // 命令表地址在 init 后不可变；handler 经 phase 原子门控。
    size_t used = 0U;
    for (const auto table : cmdTables) {
        for (const auto &command : table) {
            if (used >= s_gated_commands.size()) {
                return false;
            }
            const std::string_view name{command.name};
            s_command_gates[used] = {command.handler, command.ctx,
                                     name == "status" || name == "log" || name == "sys" ||
                                         name == "config"};
            s_gated_commands[used] = command;
            s_gated_commands[used].handler = &gatedCommand;
            s_gated_commands[used].ctx = &s_command_gates[used];
            ++used;
        }
    }
    if (cli::add(std::span{s_gated_commands}.first(used)) != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "console add failed");
        return false;
    }
    return true;
}

/**
 * @brief 登记全部动词组供 help / help <group> 展示
 * @retval true: 全部登记成功；false: cli::addVerbs 失败（已记日志）
 * @note 仅启动期单线程调用；必须在 cli::init 之前完成。
 */
bool registerVerbGroups() noexcept
{
    // 动词表与命令表不是一一对应（system 命令表提供 status/log/sys 三组，link/config 没有动词组），
    // 因此保持两张清单，避免 X-macro 编码出虚假的对应关系。
    const std::array verbGroups{
        VerbGroupDef{"status", "system and link status (default overview)", statusVerbs()},
        VerbGroupDef{"log", "runtime IDF log level", logVerbs()},
        VerbGroupDef{"sys", "system info, heap, mem, tasks, boot and reboot", sysVerbs()},
        VerbGroupDef{"rtcm", "RTCM forwarding stats and reset", rtcmVerbs()},
#if RTK_GNSS_ENABLED
        VerbGroupDef{"gnss", "GNSS receiver control and telemetry", gnssVerbs()},
#endif
#if FW_TRACE_ENABLED
        VerbGroupDef{"debug", "trace categories, report and NVS", debugVerbs()},
#endif
        VerbGroupDef{"factory", "production settings (hidden, needs unlock)", factoryVerbs()},
    };
    for (const auto &group : verbGroups) {
        const esp_err_t verbsErr = cli::addVerbs(group.name, group.summary, group.verbs);
        if (verbsErr != ESP_OK) {
            FW_LOGE(fw::TraceCat::kSys, "console verbs failed: %s", esp_err_to_name(verbsErr));
            return false;
        }
    }
    return true;
}

/**
 * @brief 建立控制台：登记命令与动词表、初始化 REPL、压低启动期 IDF 日志
 * @retval true: 控制台可用；false: 任一登记/初始化失败（降级为无控制台）
 * @note 仅启动期单线程调用；非 ISR，不阻塞。
 */
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
