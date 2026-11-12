// 固件入口与组合根：装配静态模块表、执行器与控制台命令表，初始化 GPIO26 电源保持并按顺序启动链路/发送任务。
// 只在 app_main 启动任务中运行，启动期一次性完成；之后本文件不再执行。
#include "app_context.hpp"

#include <array>
#include <span>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cli/console.hpp"

namespace {
constexpr const char *kTag = "app";
constinit app::AppContext s_app{};

template <typename T> constexpr auto kModuleOps = fw::make_ops<T>();
// 编号由 id() 保证无窄化；对象与操作表同为静态存储。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, readability-identifier-naming)
constinit const fw::Module kModules[]{
// X-macro 生成异构对象的注册项，constexpr 函数无法替代清单展开。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MODULE_DEF(type, member, name, kind, executor, period)                                                         \
    {name,          app::id(app::ModuleKind::kind), app::id(app::ExecKind::executor), period,                          \
     &s_app.member, &kModuleOps<app::type>},
#include "module_list.inc"
#undef MODULE_DEF
};
// 链路绑 core1，BT worker/控制台在 core0；每轮至少让出 1 tick 给 IDLE1。
constexpr rtos::ExecutorConfig kLinkConfig{
    .name = "link",
    .id = app::id(app::ExecKind::kLink),
    .priority = 10,
    .core = 1,
    .idle_wait_ms = 20,
    .round_budget_ms = 20,
    .yield_ticks = 1,
    .watchdog = true,
    .wait = [](void * /*ctx*/, uint32_t max_ms) noexcept { s_app.bt_link.wait(max_ms); },
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

bool enable_power_hold() noexcept
{
    // 保留基线 GPIO26 高有效的电源保持逻辑；本任务不执行硬件动作。
    const esp_err_t level_err = gpio_set_level(GPIO_NUM_26, 1U);
    if (level_err != ESP_OK) {
        ESP_LOGE(kTag, "power hold level failed: %s", esp_err_to_name(level_err));
        return false;
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
        return false;
    }
    return true;
}

void start_link() noexcept
{
    /* 所有模块完成 init 后才允许启动 TX 和链路任务。 */
    const auto initialized = s_app.link_executor.init(kLinkConfig, kModules, rtos::view_of(s_app.link_storage));
    if (initialized != fw::Status::kOk) {
        ESP_LOGE(kTag, "executor init failed: %s", to_string(initialized));
        return;
    }
    if (!s_app.data.system_status.read().bt_ready) {
        return;
    }
    const auto tx_started = s_app.bt_link.start_tx();
    if (tx_started != fw::Status::kOk) {
        ESP_LOGE(kTag, "tx task start failed (%s); replies fall back to sync write", to_string(tx_started));
    }
    const auto started = s_app.link_executor.start();
    if (started != fw::Status::kOk) {
        ESP_LOGE(kTag, "link task start failed; console only");
    }
}

void start_console() noexcept
{
    constexpr const char *kConsolePrompt = "console> ";
    const std::array<std::span<const cli::Cmd>, 4> cmd_tables{{
        app::system_cmds(),
        app::link_cmds(),
        app::storage_cmds(),
        app::rtcm_cmds(),
    }};
    /* 按原顺序注册命令，任一失败即停止控制台初始化。 */
    for (const auto table : cmd_tables) {
        const esp_err_t console_err = cli::add(table);
        if (console_err != ESP_OK) {
            ESP_LOGE(kTag, "console add failed: %s", esp_err_to_name(console_err));
            return;
        }
    }
    const esp_err_t console_err = cli::start(kConsolePrompt);
    if (console_err != ESP_OK) {
        ESP_LOGE(kTag, "console start failed: %s", esp_err_to_name(console_err));
    }
}
} // namespace

namespace app {
AppContext &app_context() noexcept
{
    return s_app;
}
} // namespace app

extern "C" void app_main(void)
{
    if (!enable_power_hold()) {
        return;
    }
    /* 输出启动原因并绑定数据主题的共享锁。 */
    // 固件版本字符串短于 int 上限，日志精度转换安全。
    ESP_LOGI(kTag, "RTK_main %.*s boot", static_cast<int>(app::kAppVersion.size()), app::kAppVersion.data());
    ESP_LOGI(kTag, "reset reason: %s", reset_reason_name(esp_reset_reason()));
    s_app.data.init();
    /* NVS/SN 必须先于 BT 初始化，执行器再次 init 存储模块时为幂等。 */
    const auto loaded = s_app.storage.init();
    if (!fw::is_ok(loaded)) {
        ESP_LOGE(kTag, "storage module init failed");
    }
    s_app.system.prepare();
    /* 先启动链路，再启动始终可用的控制台。 */
    start_link();
    start_console();
}
