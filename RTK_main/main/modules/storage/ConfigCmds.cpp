// 配置查看命令（ADR-0009 §2.10）：`config [identity|link|gnss|debug|build]`，只读。
// 客户控制台不提供任何写入生产配置的入口：SN/白名单的修改在隐藏的 factory 命令集里，GNSS 用户设置走 `gnss set`。
// 线程：仅在 console REPL 任务调用，非 ISR；读 DataCenter 快照与 NVS（NVS API 自带锁）。

#include "modules/storage/StorageModule.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include "modules/system/SystemModule.hpp"
#include "settings/Registry.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

constexpr size_t kGroupArgs = 2U;
constexpr size_t kGetArgs = 3U;
constexpr size_t kSetArgs = 4U;
constexpr size_t kKeyArgIndex = 2U;
constexpr size_t kValArgIndex = 3U;
constexpr uint32_t kSettingTimeoutMs = 6000U;

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
    cli::printKv("console_profile", "%s", kConsoleProfileName);
    cli::printKv("trace", "%s", FW_TRACE_ENABLED ? "compiled in" : "compiled out");
#if RTK_GNSS_ENABLED
    if (appContext().boardProfile == nullptr) {
        cli::printKv("gnss", "board unknown");
        return;
    }
    const auto &pins = board::current().gnss;
    cli::printKv("gnss_uart", "UART%d RX GPIO%d TX GPIO%d, probe %d / target %" PRIu32 " baud",
                 pins.uart, pins.rxGpio, pins.txGpio, CONFIG_RTK_GNSS_DEFAULT_BAUD,
                 kGnssTargetBaud);
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
 * @brief 按键名读取并打印设置服务中的当前值（`config get <key>`）
 * @param args: 命令行参数视图
 * @retval 0 已输出当前值
 * @retval 非零 参数或键无效，已输出 ERR
 * @note 仅 console REPL 任务，读 DataCenter 快照，非 ISR。
 */
int cmdConfigGet(std::span<char *const> args) noexcept
{
    if ((args.size() != kGetArgs) || (args[2] == nullptr)) {
        return cli::fail("usage: config get <key>");
    }
    const std::string_view key{args[2]};
    const auto &service = appContext().settings;
    if (settings::findKey(key) == nullptr) {
        return cli::fail("unknown key: %s", args[2]);
    }
    settings::Value value{};
    if (!service.get(key, value)) {
        return cli::fail("unreadable key: %s", args[2]);
    }
    switch (value.kind) {
    case settings::ValueKind::kInt:
        cli::outPrintf("%s=%" PRId32 "\n", args[2], value.number);
        return 0;
    case settings::ValueKind::kBool:
        cli::outPrintf("%s=%d\n", args[2], value.boolVal ? 1 : 0);
        return 0;
    case settings::ValueKind::kString:
        // textLen <= 32，转换为 printf 精度无损。
        cli::outPrintf("%s=%.*s\n", args[2], static_cast<int>(value.textLen), value.text.data());
        return 0;
    case settings::ValueKind::kBytes: {
        constexpr std::string_view kHex = "0123456789ABCDEF";
        constexpr uint8_t kNibbleMask = 0x0FU;
        constexpr unsigned kHighShift = 4U;
        std::array<char, (settings::kMaxCanonicalBytes * 2U) + 1U> hex{};
        size_t pos = 0U;
        const size_t count = std::min(static_cast<size_t>(value.textLen), value.text.size());
        for (size_t i = 0U; i < count; ++i) {
            const auto byte = static_cast<uint8_t>(value.text[i]);
            hex[pos++] = kHex[static_cast<size_t>((byte >> kHighShift) & kNibbleMask)];
            hex[pos++] = kHex[static_cast<size_t>(byte & kNibbleMask)];
        }
        hex[pos] = '\0';
        cli::outPrintf("%s=%s\n", args[2], hex.data());
        return 0;
    }
    default:
        return cli::fail("unreadable key: %s", args[2]);
    }
}

/**
 * @brief 经设置服务写入单个配置键（`config set <key> <value>`，仅 DEV 档可用）
 * @param args: 命令行参数视图
 * @retval 0 设置成功
 * @retval 非零 参数、权限、校验或执行失败，已输出 ERR
 * @note 仅 console REPL 任务，轮询等待最长 kSettingTimeoutMs，非 ISR。
 */
int cmdConfigSet(std::span<char *const> args) noexcept
{
    if constexpr (kConsoleProfile != ConsoleProfile::kDev) {
        return cli::fail("read-only in production");
    }
    if ((args.size() != kSetArgs) || (args[kKeyArgIndex] == nullptr) ||
        (args[kValArgIndex] == nullptr)) {
        return cli::fail("usage: config set <key> <value>");
    }
    fw::CommandResult result{};
    settings::Error err = settings::Error::kOk;
    if (!submitSetting(args[kKeyArgIndex], args[kValArgIndex], factoryUnlocked(), kSettingTimeoutMs,
                       result, err)) {
        return cli::fail("%s", settingsErrorText(err));
    }
    if (std::strcmp(args[kKeyArgIndex], "net.enable") == 0) {
        cli::outPrintf("OK net.enable saved; takes effect after reboot (restart pending)\n");
    }
    return 0;
}

/**
 * @brief 按类别显示持久化配置，或经设置服务读写单键（`config get/set`）
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 命令成功
 * @retval 非零 参数、状态、提交或执行失败，已输出 ERR
 * @note 仅 console REPL 任务；可等待输出或执行器，非 ISR。
 */
int cmdConfig(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 1) || (argv == nullptr)) {
        return cli::fail("usage: config [identity|link|gnss|debug|build|get|set]");
    }
    // argc >= 1 已校验，转 size_t 无损。
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    if (args.size() >= kGroupArgs) {
        const std::string_view sub{(args[1] != nullptr) ? args[1] : ""};
        if (sub == "get") {
            return cmdConfigGet(args);
        }
        if (sub == "set") {
            return cmdConfigSet(args);
        }
    }
    if (args.size() > kGroupArgs) {
        return cli::fail("usage: config [identity|link|gnss|debug|build]");
    }
    if (appContext().bootConfig.safeDefaults) {
        cli::printKv("boot_mode", "safe defaults");
    }
    if (args.size() == 1U) {
        for (const Section &section : kSections) {
            section.show();
        }
        return 0;
    }
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
    {"config",
     "config [identity|link|gnss|debug|build|get <k>|set <k> <v>]: show or set saved settings",
     "[identity|link|gnss|debug|build|get|set]", cmdConfig, nullptr, false},
}};

} // namespace

std::span<const cli::Cmd> configCmds() noexcept
{
    return kConfigCmds;
}

} // namespace app
