// 控制台装配与启动阶段门控（R8a，自 AppMain.cpp 迁入）：命令表/动词表在 REPL 启动前一次性登记。
// 线程：init_console/run_console 只在启动任务调用，gated_command 只在 console REPL 任务调用；非 ISR，不分配堆。
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

constexpr const char *kTag = "app";

#if defined(ESP_PLATFORM)
constinit std::array<app::GatedCommand, cli::kMaxCmds> s_command_gates{};
constinit std::array<cli::Cmd, cli::kMaxCmds> s_gated_commands{};

/**
 * @brief  组件日志直写 sink（ADR-0009 §2.12）：文本环未安装时（启动早期、追踪关闭的固件）同步写控制台
 * @param  ctx: 未用
 * @param  hdr: 记录头
 * @param  payload: 文本（不含换行）
 * @note   任意任务调用；格式化缓冲在调用方栈上（kLogLineMax 字节），cli::out_write 内部加锁。
 */
void direct_log_sink(void *ctx, const fw::LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    static_cast<void>(ctx);
    std::array<char, fw::kLogLineMax> line{};
    const size_t len = fw::log_format_line(line, hdr, payload);
    cli::out_write(line.data(), len);
}
#endif

} // namespace

namespace app {

// NOLINTNEXTLINE(misc-const-correctness): cli::Handler 的固定 ABI 要求 void*，gate 本身只读。
int gated_command(void *ctx, int argc, char **argv) noexcept
{
    const auto &gate = *static_cast<const GatedCommand *>(ctx); // ctx 只指向组合根的静态命令 gate。
    const auto phase = app_context().boot_phase.load(std::memory_order_acquire);
    if ((phase == BootPhase::kStarting) || (phase == BootPhase::kStopping)) {
        return cli::fail("unavailable: booting");
    }
    if ((phase == BootPhase::kConsoleOnly) && !gate.safe) {
        return cli::fail("unavailable: console-only");
    }
    return gate.handler(gate.ctx, argc, argv);
}

void set_console_only() noexcept
{
    app_context().boot_phase.store(BootPhase::kConsoleOnly, std::memory_order_release);
}

#if defined(ESP_PLATFORM)
bool init_console() noexcept
{
    constexpr const char *kConsolePrompt = "console> ";
    struct VerbGroupDef {
        const char *name;
        const char *summary;
        std::span<const cli::Verb> verbs;
    };

    const std::array cmd_tables{
        system_cmds(),  link_cmds(), config_cmds(), rtcm_cmds(),
#if RTK_GNSS_ENABLED
        gnss_cmds(),
#endif
#if FW_TRACE_ENABLED
        debug_cmds(),
#endif
        factory_cmds(),
    };
    // 命令表地址在 init 后不可变；handler 经 phase 原子门控。输出通道先于业务堆分配建立，REPL 到启动末尾才放行。
    size_t used = 0U;
    for (const auto table : cmd_tables) {
        for (const auto &command : table) {
            if (used >= s_gated_commands.size()) {
                return false;
            }
            const std::string_view name{command.name};
            s_command_gates[used] = {command.handler, command.ctx,
                                     name == "status" || name == "log" || name == "sys" || name == "config"};
            s_gated_commands[used] = command;
            s_gated_commands[used].handler = &gated_command;
            s_gated_commands[used].ctx = &s_command_gates[used];
            ++used;
        }
    }
    if (cli::add(std::span{s_gated_commands}.first(used)) != ESP_OK) {
        ESP_LOGE(kTag, "console add failed");
        return false;
    }
    // 动词表与命令表不是一一对应（system 命令表提供 status/log/sys 三组，link/config 没有动词组），
    // 因此保持两张清单，避免 X-macro 编码出虚假的对应关系。
    const std::array verb_groups{
        VerbGroupDef{"status", "system and link status (default overview)", status_verbs()},
        VerbGroupDef{"log", "runtime IDF log level", log_verbs()},
        VerbGroupDef{"sys", "system info, heap, mem, tasks, boot and reboot", sys_verbs()},
        VerbGroupDef{"rtcm", "RTCM forwarding stats and reset", rtcm_verbs()},
#if RTK_GNSS_ENABLED
        VerbGroupDef{"gnss", "GNSS receiver control and telemetry", gnss_verbs()},
#endif
#if FW_TRACE_ENABLED
        VerbGroupDef{"debug", "trace categories, report and NVS", debug_verbs()},
#endif
        VerbGroupDef{"factory", "production settings (hidden, needs unlock)", factory_verbs()},
    };
    /* 必须在 init 之前登记所有动词表，供 help / help <group> 展示。 */
    for (const auto &group : verb_groups) {
        const esp_err_t verbs_err = cli::add_verbs(group.name, group.summary, group.verbs);
        if (verbs_err != ESP_OK) {
            ESP_LOGE(kTag, "console verbs failed: %s", esp_err_to_name(verbs_err));
            return false;
        }
    }
    const esp_err_t console_err = cli::init(kConsolePrompt);
    if (console_err != ESP_OK) {
        ESP_LOGE(kTag, "console init failed: %s", esp_err_to_name(console_err));
        return false;
    }
    // 控制台输出通道已建立：此后组件日志（FW_LOG*）在文本环安装前直写控制台，不再丢失。
    if (fw::log_install_direct(&direct_log_sink, nullptr, &rtos::now_ms) != fw::Result::kOk) {
        ESP_LOGW(kTag, "component log direct sink not installed");
    }
    // 蓝牙控制器与 PHY 的 IDF 启动 INFO 已由 "Bluetooth online" 摘要行覆盖；需要时可用 `log` 命令调回。
    esp_log_level_set("BTDM_INIT", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    // app_main 返回时 IDF 打印 "Returned from app_main()"，此时提示符已出，会接在提示符后面。
    esp_log_level_set("main_task", ESP_LOG_WARN);
    return true;
}

void run_console() noexcept
{
    // init 失败（降级路径）时 run 返回 INVALID_STATE：没有可用控制台，只能记录。
    const esp_err_t err = cli::run();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "console run failed: %s", esp_err_to_name(err));
    }
}
#endif // ESP_PLATFORM

} // namespace app
