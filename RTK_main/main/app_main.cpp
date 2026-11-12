// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "app_config.hpp"
#include "data/app_data.hpp"
#include "modules/bt_link/bt_link_module.hpp"
#include "modules/rtcm/rtcm_module.hpp"
#include "modules/storage/storage_module.hpp"
#include "modules/system/system_module.hpp"

#include <array>
#include <span>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "rtk/console/console.hpp"

namespace {
constexpr const char *kTag = "rtk";
constexpr size_t kLinkStackBytes = 6144;
constinit rtk::os::Executor s_link_executor{};
constinit rtk::os::ExecutorStorage<kLinkStackBytes, 4> s_link_storage{};
// 编号为 uint8_t 底层枚举，由 id() 保证无窄化；所有实例与表均为静态存储。
// 任务卡明确规定 constinit kModules[]；静态数组由 span 带长度传入执行器。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, readability-identifier-naming)
constinit const ::app::Module kModules[]{
    {"bt_link", rtk::app::id(rtk::app::ModuleKind::kBtLink), rtk::app::id(rtk::app::ExecKind::kLink), 0,
     &rtk::app::s_bt_link_module, &rtk::app::kBtLinkOps},
    {"rtcm", rtk::app::id(rtk::app::ModuleKind::kRtcm), rtk::app::id(rtk::app::ExecKind::kLink), 0,
     &rtk::app::s_rtcm_module, &rtk::app::kRtcmOps},
    {"storage", rtk::app::id(rtk::app::ModuleKind::kStorage), rtk::app::id(rtk::app::ExecKind::kLink), 0,
     &rtk::app::s_storage_module, &rtk::app::kStorageOps},
    {"system", rtk::app::id(rtk::app::ModuleKind::kSystem), rtk::app::id(rtk::app::ExecKind::kLink), 0,
     &rtk::app::s_system_module, &rtk::app::kSystemOps},
};
// 链路绑 core1，BT worker/控制台在 core0；每轮至少让出 1 tick 给 IDLE1。
constexpr rtk::os::ExecutorConfig kLinkConfig{
    .name = "rtk_link",
    .id = rtk::app::id(rtk::app::ExecKind::kLink),
    .priority = 10,
    .core = 1,
    .idle_wait_ms = 20,
    .round_budget_ms = 20,
    .yield_ticks = 1,
    .watchdog = true,
    .wait = rtk::app::bt_link_wait,
    .wait_ctx = nullptr,
};

const char *reset_reason_name(esp_reset_reason_t reason) noexcept
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external-pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-wdt";
    case ESP_RST_TASK_WDT:
        return "task-wdt";
    case ESP_RST_WDT:
        return "other-wdt";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power-glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu-lockup";
    default:
        return "unknown";
    }
}
} // namespace

extern "C" void app_main(void)
{
    // 保留基线电源保持初始化（原 app_main.cpp，GPIO26 高有效）；本任务不执行硬件动作。
    const esp_err_t level_err = gpio_set_level(GPIO_NUM_26, 1U);
    if (level_err != ESP_OK) {
        ESP_LOGE(kTag, "power hold level failed: %s", esp_err_to_name(level_err));
        return;
    }
    gpio_config_t power_hold{};
    power_hold.pin_bit_mask = 1ULL << GPIO_NUM_26;
    power_hold.mode = GPIO_MODE_OUTPUT;
    power_hold.pull_up_en = GPIO_PULLUP_DISABLE;
    power_hold.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_hold.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t power_err = gpio_config(&power_hold);
    if (power_err != ESP_OK) {
        ESP_LOGE(kTag, "power hold init failed: %s", esp_err_to_name(power_err));
        return;
    }
    // 固件版本字符串短于 int 上限，日志精度转换安全。
    ESP_LOGI(kTag, "RTK_main %.*s boot", static_cast<int>(rtk::app::kAppVersion.size()), rtk::app::kAppVersion.data());
    ESP_LOGI(kTag, "reset reason: %s", reset_reason_name(esp_reset_reason()));
    rtk::app::data::init();
    // NVS/SN 必须先于 BT 初始化；Executor.init 再次调用时为幂等，不重复访问后端。
    const auto loaded = rtk::app::storage_init(&rtk::app::s_storage_module);
    if (!::app::is_ok(loaded)) {
        ESP_LOGE(kTag, "storage module init failed");
    }
    rtk::app::system_prepare(s_link_executor);
    const auto initialized = s_link_executor.init(kLinkConfig, kModules, rtk::os::view_of(s_link_storage));
    if (initialized != rtk::Status::kOk) {
        ESP_LOGE(kTag, "executor init failed: %s", rtk::to_string(initialized));
    } else if (rtk::app::data::kSystemStatus.read().bt_ready) {
        const auto tx_started = rtk::app::bt_link_start_tx();
        if (tx_started != rtk::Status::kOk) {
            ESP_LOGE(kTag, "tx task start failed (%s); replies fall back to sync write", rtk::to_string(tx_started));
        }
        const auto started = s_link_executor.start();
        if (started != rtk::Status::kOk) {
            ESP_LOGE(kTag, "link task start failed; console only");
        }
    }
    constexpr const char *kConsolePrompt = "rtk> ";
    const std::array<std::span<const rtk::console::Cmd>, 4> cmd_tables{{
        rtk::app::system_cmds(),
        rtk::app::link_cmds(),
        rtk::app::storage_cmds(),
        rtk::app::rtcm_cmds(),
    }};
    esp_err_t console_err = ESP_OK;
    for (const auto table : cmd_tables) {
        console_err = rtk::console::add(table);
        if (console_err != ESP_OK) {
            ESP_LOGE(kTag, "console add failed: %s", esp_err_to_name(console_err));
            break;
        }
    }
    if (console_err == ESP_OK) {
        console_err = rtk::console::start(kConsolePrompt);
        if (console_err != ESP_OK) {
            ESP_LOGE(kTag, "console start failed: %s", esp_err_to_name(console_err));
        }
    }
}
