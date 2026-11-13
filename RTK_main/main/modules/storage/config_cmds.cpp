// 配置查看命令（ADR-0009 §2.10）：`config [identity|link|gnss|debug|build]`，只读。
// 客户控制台不提供任何写入生产配置的入口：SN/白名单的修改在隐藏的 factory 命令集里，GNSS 用户设置走 `gnss set`。
// 线程：仅在 console REPL 任务调用，非 ISR；读 DataCenter 快照与 NVS（NVS API 自带锁）。

#include "modules/storage/storage_module.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "esp_err.h"
#include "nvs.h" // ESP_ERR_NVS_NOT_FOUND
#include "sdkconfig.h"

#include "app_config.hpp"
#include "app_context.hpp"
#include "cli/out.hpp"
#include "fw/trace.hpp" // FW_TRACE_ENABLED
#include "modules/storage/store_catalog.hpp"
#include "modules/storage/store_keys.hpp"
#include "support/console_support.hpp"

namespace app {

namespace {

constexpr size_t kGroupArgs = 2U;
constexpr int kNameWidth = 14; // 键名列宽，与 cli::print_kv 一致

void show_identity() noexcept
{
    const auto &data = app_context().data;
    const auto sys = data.system_status.read();
    const auto store = data.store_status.read();
    cli::print_section("identity");
    cli::print_kv("sn", "%s", sys.sn.data());
    cli::print_kv("bt_name", "%s", sys.device_name.data());
    if (store.sn_error != ESP_OK) {
        cli::print_kv("sn_store", "%s (SN derived from BT MAC)", esp_err_to_name(store.sn_error));
    }
}

void show_link() noexcept
{
    const auto &data = app_context().data;
    const auto link = data.link_status.read();
    const auto store = data.store_status.read();
    cli::print_section("link");
    cli::print_kv("policy", "%s", policy_name(link.policy));
    // 白名单计数上限为 kMaxAlarms，转 unsigned 无损。
    cli::print_kv("whitelist", "%u / %u", static_cast<unsigned>(link.whitelist_count),
                  static_cast<unsigned>(kMaxAlarms));
    std::array<char, kMacStrBuf> mac_buf{};
    for (size_t idx = 0U; idx < link.whitelist_count; ++idx) {
        format_mac(link.whitelist[idx], mac_buf);
        cli::print_kv("  alarm", "%s", mac_buf.data());
    }
    if (store.whitelist_pending || (store.whitelist_error != ESP_OK)) {
        cli::print_kv("wl_sync", "%s, last error %s, failures %" PRIu32 ", retry %" PRIu32 " ms",
                      store.whitelist_pending ? "pending" : "idle", esp_err_to_name(store.whitelist_error),
                      store.failures, store.retry_ms);
    }
}

void show_group(const StoreGroup &group) noexcept
{
    // 类别名长度为常量字面量，远小于 int 上限。
    cli::out_printf("[%.*s] %.*s\n", static_cast<int>(group.name.size()), group.name.data(),
                    static_cast<int>(group.title.size()), group.title.data());
    if (group.keys.empty()) {
        cli::out_printf("  (feature disabled in this build)\n");
        return;
    }
    for (const fw::StoreKey &key : group.keys) {
        constexpr size_t kTextBufBytes = 24U;
        std::array<char, kTextBufBytes> text{};
        if (key.type == fw::StoreType::kBlob) {
            std::array<uint8_t, store::kMaxBlobKeyBytes> blob{};
            size_t len = 0U;
            const esp_err_t err = store::read_blob(key, blob, len);
            if (err == ESP_OK) {
                (void)std::snprintf(text.data(), text.size(), "blob(%uB)", static_cast<unsigned>(len));
            } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                (void)std::snprintf(text.data(), text.size(), "default");
            } else {
                (void)std::snprintf(text.data(), text.size(), "error");
            }
        } else {
            uint32_t value = 0U;
            const esp_err_t err = store::read_value(key, value);
            if (err == ESP_OK) {
                (void)std::snprintf(text.data(), text.size(), "%" PRIu32, value);
            } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                (void)std::snprintf(text.data(), text.size(), "default");
            } else {
                (void)std::snprintf(text.data(), text.size(), "error");
            }
        }
        cli::out_printf("  %-*.*s %-10s  %.*s\n", kNameWidth, static_cast<int>(key.key.size()), key.key.data(),
                        text.data(), static_cast<int>(key.help.size()), key.help.data());
    }
}

void show_groups(std::string_view prefix) noexcept
{
    for (const StoreGroup &group : store_catalog()) {
        if (group.name.substr(0U, prefix.size()) == prefix) {
            show_group(group);
        }
    }
}

void show_build() noexcept
{
    cli::print_section("build");
    cli::print_kv("console", "UART%d %d baud", CONFIG_RTK_CONSOLE_UART_NUM, CONFIG_RTK_CONSOLE_UART_BAUD);
    cli::print_kv("trace", "%s", FW_TRACE_ENABLED ? "compiled in" : "compiled out");
#if RTK_GNSS_ENABLED
    cli::print_kv("gnss_uart", "UART%d RX GPIO%d TX GPIO%d, %d baud", CONFIG_RTK_GNSS_UART_NUM, CONFIG_RTK_GNSS_RX_GPIO,
                  CONFIG_RTK_GNSS_TX_GPIO, CONFIG_RTK_GNSS_DEFAULT_BAUD);
    cli::print_kv("gnss_power", "GPIO%d active %s", CONFIG_RTK_GNSS_POWER_GPIO,
                  (CONFIG_RTK_GNSS_POWER_ACTIVE_HIGH != 0) ? "high" : "low");
    cli::print_kv("gnss_port", "COM%d", CONFIG_RTK_GNSS_PORT);
#else
    cli::print_kv("gnss", "disabled");
#endif
}

struct Section {
    std::string_view name;
    void (*show)() noexcept;
};

void show_gnss() noexcept
{
    show_groups("gnss");
}

void show_debug() noexcept
{
    show_groups("debug");
}

constexpr std::array<Section, 5U> kSections{{
    {"identity", show_identity},
    {"link", show_link},
    {"gnss", show_gnss},
    {"debug", show_debug},
    {"build", show_build},
}};

int cmd_config(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 1) || (argc > static_cast<int>(kGroupArgs)) || (argv == nullptr)) {
        return cli::fail("usage: config [identity|link|gnss|debug|build]");
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
     "[identity|link|gnss|debug|build]", cmd_config, nullptr, false},
}};

} // namespace

std::span<const cli::Cmd> config_cmds() noexcept
{
    return kConfigCmds;
}

} // namespace app
