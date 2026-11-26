// 主机验证 RTC 射频记录校验、一次性消费及生产模块的启用集合。
// 单线程主机测试，非 ISR。
#include "app/RadioMode.hpp"

#include <array>
#include <cassert>
#include <string_view>

namespace {

using app::ModuleEnable;
using app::RadioMode;
using app::RadioReason;

// 验证包含 GNSS、trace 和 Wi-Fi 配置的完整清单，仅使用注册项名称和启用条件。
#define RTK_GNSS_ENABLED                                               1
#define FW_TRACE_ENABLED                                               1
#define CONFIG_RTK_WIFI_CONFIG_ENABLE                                  1
#define MODULE_DEF(type, member, name, kind, executor, period, enable) {name, ModuleEnable::enable},
struct ModuleEntry {
    std::string_view name;
    ModuleEnable enable;
};
constexpr auto kModules = std::to_array<ModuleEntry>({
#include "module_list.inc"
});
#undef MODULE_DEF
#undef CONFIG_RTK_WIFI_CONFIG_ENABLE
#undef FW_TRACE_ENABLED
#undef RTK_GNSS_ENABLED

/**
 * @brief 取出给定启动档与射频模式启用的模块名
 * @param bootMode: 启动运行档
 * @param mode: 待检查的射频模式
 * @retval 按生产顺序排列的模块名，末尾空项表示跳过的条目
 * @note 仅编译期调用。
 */
consteval auto enabledNames(app::board::BootMode bootMode, RadioMode mode) noexcept
{
    std::array<std::string_view, kModules.size()> names{};
    size_t used = 0U;
    for (const auto &module : kModules) {
        if (app::moduleEnabled(bootMode, mode, module.enable)) {
            names[used++] = module.name;
        }
    }
    return names;
}

static_assert(enabledNames(app::board::BootMode::kNormal, RadioMode::kBluetooth) ==
              std::array<std::string_view, 10U>{"bt_link", "rtcm", "storage", "system", "power",
                                                "oled", "gnss", "net", "debug"});
static_assert(enabledNames(app::board::BootMode::kNormal, RadioMode::kWifiConfig) ==
              std::array<std::string_view, 10U>{"storage", "system", "power", "oled", "web", "gnss",
                                                "net", "debug"});
static_assert(enabledNames(app::board::BootMode::kChargeStandby, RadioMode::kBluetooth) ==
              std::array<std::string_view, 10U>{"storage", "system", "power", "oled", "debug"});
static_assert(enabledNames(app::board::BootMode::kChargeStandby, RadioMode::kWifiConfig) ==
              std::array<std::string_view, 10U>{"storage", "system", "power", "oled", "debug"});
static_assert(!app::moduleEnabled(RadioMode::kBluetooth, ModuleEnable::kWifiOnly));
static_assert(app::moduleEnabled(RadioMode::kWifiConfig, ModuleEnable::kWifiOnly));
static_assert(!app::moduleEnabled(app::board::BootMode::kChargeStandby, RadioMode::kBluetooth,
                                  ModuleEnable::kNormalOnly));

/**
 * @brief 检查回退路径选择蓝牙且无请求原因
 * @param selection: 被检查的解码或消费结果
 * @note 单线程主机测试，非 ISR。
 */
void checkDefault(app::RadioSelection selection)
{
    assert(selection.mode == RadioMode::kBluetooth && !selection.reason.has_value());
}

} // namespace

/**
 * @brief 验证合法记录、位错误、非软件复位及一次性消费
 * @retval 0 所有检查通过
 * @note 单线程主机测试，非 ISR。
 */
int main()
{
    for (const auto mode : {RadioMode::kBluetooth, RadioMode::kWifiConfig}) {
        for (const auto reason : {RadioReason::kConsole, RadioReason::kPhone, RadioReason::kWebExit,
                                  RadioReason::kIdle, RadioReason::kUi}) {
            const auto decoded = app::decodeRadioRecord(app::encodeRadioRecord(mode, reason));
            assert(decoded.mode == mode && decoded.reason == reason);
        }
    }
    const uint32_t wifi = app::encodeRadioRecord(RadioMode::kWifiConfig, RadioReason::kConsole);
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        checkDefault(app::decodeRadioRecord(wifi ^ (uint32_t{1U} << bit)));
    }
    volatile uint32_t rtc = wifi;
    checkDefault(app::consumeRadioRecord(rtc, false)); // 上电、掉电或故障复位。
    assert(rtc == 0U);
    rtc = wifi ^ 1U;
    checkDefault(app::consumeRadioRecord(rtc, true));
    assert(rtc == 0U);
    rtc = wifi;
    const auto request = app::consumeRadioRecord(rtc, true);
    assert(request.mode == RadioMode::kWifiConfig && request.reason == RadioReason::kConsole);
    assert(rtc == 0U);
    checkDefault(app::consumeRadioRecord(rtc, true));
    return 0;
}
