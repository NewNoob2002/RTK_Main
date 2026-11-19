// 配置查看命令（ADR-0009 §2.10）：`config [identity|link|gnss|debug|build]`，只读。
// 客户控制台不提供任何写入生产配置的入口：SN/白名单的修改在隐藏的 factory 命令集里，GNSS 用户设置走 `gnss set`。
// 线程：仅在 console REPL 任务调用，非 ISR；读 DataCenter 快照与 NVS（NVS API 自带锁）。

#include "modules/storage/StorageModule.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "esp_err.h"
#include "sdkconfig.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "app/ConsoleProfile.hpp"
#include "board/BoardProfile.hpp"
#include "cli/Out.hpp"
#include "fw/Trace.hpp" // FW_TRACE_ENABLED
#include "modules/storage/StoreCatalog.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

constexpr size_t kGroupArgs = 2U;

/**
 * @brief 显示身份与设备名
 * @note 仅 console REPL 任务；输出和 NVS 访问可阻塞，非 ISR。
 */
void showIdentity() noexcept
{
    const auto &data = appContext().data;
    const auto sys = data.systemStatus.read();
    const auto store = data.storeStatus.read();
    cli::printSection("identity");
    cli::printKv("sn", "%s", sys.sn.data());
    cli::printKv("bt_name", "%s", sys.deviceName.data());
    if (store.snError != ESP_OK) {
        cli::printKv("sn_store", "%s (SN derived from BT MAC)", esp_err_to_name(store.snError));
    }
}

/**
 * @brief 显示链路策略与白名单
 * @note 仅 console REPL 任务；输出和 NVS 访问可阻塞，非 ISR。
 */
void showLink() noexcept
{
    const auto &data = appContext().data;
    const auto link = data.linkStatus.read();
    const auto store = data.storeStatus.read();
    cli::printSection("link");
    cli::printKv("policy", "%s", policyName(link.policy));
    // 白名单计数上限为 kMaxAlarms，转 unsigned 无损。
    cli::printKv("whitelist", "%u / %u", static_cast<unsigned>(link.whitelistCount),
                 static_cast<unsigned>(kMaxAlarms));
    std::array<char, kMacStrBuf> macBuf{};
    for (size_t idx = 0U; idx < link.whitelistCount; ++idx) {
        formatMac(link.whitelist[idx], macBuf);
        cli::printKv("  alarm", "%s", macBuf.data());
    }
    if (store.whitelistPending || (store.whitelistError != ESP_OK)) {
        cli::printKv("wl_sync", "%s, last error %s, failures %" PRIu32 ", retry %" PRIu32 " ms",
                     store.whitelistPending ? "pending" : "idle",
                     esp_err_to_name(store.whitelistError), store.failures, store.retryMs);
    }
}

/**
 * @brief  打印一个持久化类别：每键一行“键名 当前值 说明”；数值键有显示名时打印为 "1 (unicore)"
 * @param  group: 类别（名字、说明、键表、可空的数值显示名钩子）
 */
void showGroup(const StoreGroup &group) noexcept
{
    // 类别名长度为常量字面量，远小于 int 上限。
    cli::outPrintf("[%.*s] %.*s\n", static_cast<int>(group.name.size()), group.name.data(),
                   static_cast<int>(group.title.size()), group.title.data());
    if (group.keys.empty()) {
        cli::outPrintf("  (feature disabled in this build)\n");
        return;
    }
    for (const fw::StoreKey &key : group.keys) {
        std::array<char, store::kKeyLineBytes> line{};
        store::formatKey(line, key, group.valueName);
        cli::outPrintf("%s\n", line.data());
    }
}

/**
 * @brief 列出全部持久化类别
 * @param prefix: 类别名前缀筛选，空串表示全部
 * @note 仅 console REPL 任务；输出和 NVS 访问可阻塞，非 ISR。
 */
void showGroups(std::string_view prefix) noexcept
{
    for (const StoreGroup &group : storeCatalog()) {
        if (group.name.substr(0U, prefix.size()) == prefix) {
            showGroup(group);
        }
    }
}

/**
 * @brief 显示当前固件构建配置
 * @note 仅 console REPL 任务；输出和 NVS 访问可阻塞，非 ISR。
 */
void showBuild() noexcept
{
    cli::printSection("build");
    cli::printKv("console", "UART%d %d baud", CONFIG_RTK_CONSOLE_UART_NUM,
                 CONFIG_RTK_CONSOLE_UART_BAUD);
    cli::outPrintf("console profile %s\n", kConsoleProfileName);
    cli::printKv("trace", "%s", FW_TRACE_ENABLED ? "compiled in" : "compiled out");
#if RTK_GNSS_ENABLED
    if (appContext().boardProfile == nullptr) {
        cli::printKv("gnss", "board unknown");
        return;
    }
    const auto &pins = board::current().gnss;
    cli::printKv("gnss_uart", "UART%d RX GPIO%d TX GPIO%d, %d baud", pins.uart, pins.rxGpio,
                 pins.txGpio, CONFIG_RTK_GNSS_DEFAULT_BAUD);
    if (pins.powerGpio >= 0) {
        cli::printKv("gnss_power", "GPIO%d active %s", pins.powerGpio,
                     pins.powerActiveHigh ? "high" : "low");
    } else {
        cli::printKv("gnss_power", "none");
    }
    cli::printKv("gnss_port", "COM%d", board::gnssReceiverPort());
#else
    cli::printKv("gnss", "disabled");
#endif
}

struct Section {
    std::string_view name;
    void (*show)() noexcept;
};

/**
 * @brief 显示 GNSS 持久化配置
 * @note 仅 console REPL 任务；输出和 NVS 访问可阻塞，非 ISR。
 * @note 节选表项：签名由 Section::show 固定；节名在此注入。
 */
void showGnss() noexcept
{
    showGroups("gnss");
}

/**
 * @brief 显示调试持久化配置
 * @note 仅 console REPL 任务；输出和 NVS 访问可阻塞，非 ISR。
 * @note 节选表项（Section::show），同 showGnss。
 */
void showDebug() noexcept
{
    showGroups("debug");
}

constexpr std::array<Section, 5U> kSections{{
    {"identity", showIdentity},
    {"link", showLink},
    {"gnss", showGnss},
    {"debug", showDebug},
    {"build", showBuild},
}};

/**
 * @brief 按类别显示持久化配置
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdConfig(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 1) || (argc > static_cast<int>(kGroupArgs)) || (argv == nullptr)) {
        return cli::fail("usage: config [identity|link|gnss|debug|build]");
    }
    if (appContext().bootConfig.safeDefaults) {
        cli::printKv("boot_mode", "safe defaults");
    }
    if (argc == 1) {
        for (const Section &section : kSections) {
            section.show();
        }
        return 0;
    }
    const std::span<char *const> args(argv, kGroupArgs);
    const std::string_view want{args[1]};
    for (const Section &section : kSections) {
        if (section.name == want) {
            section.show();
            return 0;
        }
    }
    return cli::fail("unknown group (identity|link|gnss|debug|build)");
}

constexpr std::array<cli::Cmd, 1U> kConfigCmds{{
    {"config", "config [identity|link|gnss|debug|build]: show saved settings (read-only)",
     "[identity|link|gnss|debug|build]", cmdConfig, nullptr, false},
}};

} // namespace

std::span<const cli::Cmd> configCmds() noexcept
{
    return kConfigCmds;
}

} // namespace app
