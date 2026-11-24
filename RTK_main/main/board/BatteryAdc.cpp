// 采样 P2 电池电压并按原理图还原 VBAT。
// 启动任务初始化并取首样，随后由 kLink 独占，非 ISR。
// 堆档位：启动期一次性；SDK 创建一个 ADC1 单元和一个校准句柄，失败时回滚。
#include "board/BatteryAdc.hpp"

#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"

namespace app::board {
namespace {

constexpr adc_atten_t kAttenuation = ADC_ATTEN_DB_12;
constexpr adc_bitwidth_t kBitWidth = ADC_BITWIDTH_12;
constexpr uint32_t kNominalVrefMv = 1100U; // mV，缺 eFuse 校准时使用
constexpr uint32_t kBatteryDivider = 2U;   // p2v1.41.pdf 第 5 页 R217/R218，均 100 kΩ

struct BatteryAdcState {
    adc_oneshot_unit_handle_t adc = nullptr;
    adc_cali_handle_t calibration = nullptr;
    adc_channel_t channel{};
};

constinit BatteryAdcState s_battery_adc{};

} // namespace

fw::Status initBatteryAdc(int gpio) noexcept
{
    if (gpio < 0) {
        return fw::Status::kNotFound;
    }
    if (s_battery_adc.calibration != nullptr) {
        return fw::Status::kOk;
    }
    adc_unit_t unit{};
    adc_channel_t channel{};
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit, &channel);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    if (unit != ADC_UNIT_1) {
        return fw::Status::kInvalidArg;
    }
    const adc_oneshot_unit_init_cfg_t unitConfig{
        .unit_id = unit, .clk_src = ADC_RTC_CLK_SRC_DEFAULT, .ulp_mode = ADC_ULP_MODE_DISABLE};
    adc_oneshot_unit_handle_t adc = nullptr;
    err = adc_oneshot_new_unit(&unitConfig, &adc);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    const adc_oneshot_chan_cfg_t config{.atten = kAttenuation, .bitwidth = kBitWidth};
    err = adc_oneshot_config_channel(adc, channel, &config);
    adc_cali_handle_t calibration = nullptr;
    if (err == ESP_OK) {
        const adc_cali_line_fitting_config_t caliConfig{.unit_id = unit,
                                                        .atten = kAttenuation,
                                                        .bitwidth = kBitWidth,
                                                        .default_vref = kNominalVrefMv};
        err = adc_cali_create_scheme_line_fitting(&caliConfig, &calibration);
    }
    if (err != ESP_OK) {
        const auto cleanup = adc_oneshot_del_unit(adc);
        if (cleanup != ESP_OK) {
            FW_LOGW(fw::TraceCat::kSys, "<battery> ADC cleanup failed: %s",
                    esp_err_to_name(cleanup));
        }
        return rtos::fromEspErr(err);
    }
    s_battery_adc = {adc, calibration, channel};
    return fw::Status::kOk;
}

fw::Status readBatteryAdcMv(uint32_t &mv) noexcept
{
    if (s_battery_adc.calibration == nullptr) {
        return fw::Status::kInvalidState;
    }
    int raw = 0;
    auto err = adc_oneshot_read(s_battery_adc.adc, s_battery_adc.channel, &raw);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    int pinMv = 0;
    err = adc_cali_raw_to_voltage(s_battery_adc.calibration, raw, &pinMv);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    // 校准值最多为 ADC 量程，钳制负数后可安全转换并乘以 2。
    mv = pinMv > 0 ? static_cast<uint32_t>(pinMv) * kBatteryDivider : 0U;
    return fw::Status::kOk;
}

} // namespace app::board
