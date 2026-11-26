// ESP32 板识别与电源保持（ADR-0015）。
// 仅启动任务调用，非 ISR。ADC oneshot 与校准的 SDK 内部堆开销仅存在于各启动步骤。
#include "board/BoardStartup.hpp"

#include <cinttypes>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"
#include "sdkconfig.h"

#include "board/BoardProfile.hpp"
#include "board/Charger.hpp"

namespace app::board {
namespace {

constexpr adc_atten_t kAttenuation = ADC_ATTEN_DB_12;
constexpr adc_bitwidth_t kBitWidth = ADC_BITWIDTH_12;
constexpr uint32_t kNominalVrefMv = 1100U;
constexpr int kIdSamples = 8;
constexpr uint32_t kHoldPollMs = 10U;

class BootAdc final {
public:
    constexpr BootAdc() noexcept = default;
    BootAdc(const BootAdc &) = delete;
    BootAdc &operator=(const BootAdc &) = delete;

    /** @brief 释放属于本启动步骤的校准及 ADC 句柄；启动任务，非 ISR。 */
    ~BootAdc() noexcept
    {
        if (cali_ != nullptr) {
            const esp_err_t err = adc_cali_delete_scheme_line_fitting(cali_);
            if (err != ESP_OK) {
                FW_LOGE(fw::TraceCat::kSys, "adc calibration release failed: %s",
                        esp_err_to_name(err));
            }
        }
        if (unit_ != nullptr) {
            const esp_err_t err = adc_oneshot_del_unit(unit_);
            if (err != ESP_OK) {
                FW_LOGE(fw::TraceCat::kSys, "adc unit release failed: %s", esp_err_to_name(err));
            }
        }
    }

    /**
     * @brief 配置一次性 ADC 通道并装配 line fitting 校准
     * @param unitId: SDK ADC 单元编号
     * @param channel: 该单元内的通道号
     * @retval ESP_OK 可用（缺 eFuse 时用标称 Vref 校准）
     * @retval 其他 SDK 配置错误，已获取的句柄仍由析构释放
     * @note 仅启动任务调用一次，非 ISR。
     */
    [[nodiscard]] esp_err_t init(adc_unit_t unitId, adc_channel_t channel) noexcept
    {
        // oneshot 将零当作选择 SDK 默认时钟源的哨兵值。
        const adc_oneshot_unit_init_cfg_t unitConfig{.unit_id = unitId,
                                                     .clk_src =
                                                         static_cast<adc_oneshot_clk_src_t>(0),
                                                     .ulp_mode = ADC_ULP_MODE_DISABLE};
        esp_err_t err = adc_oneshot_new_unit(&unitConfig, &unit_);
        if (err != ESP_OK) {
            return err;
        }
        const adc_oneshot_chan_cfg_t channelConfig{.atten = kAttenuation, .bitwidth = kBitWidth};
        err = adc_oneshot_config_channel(unit_, channel, &channelConfig);
        if (err != ESP_OK) {
            return err;
        }
        channel_ = channel;
        adc_cali_line_fitting_efuse_val_t source{};
        err = adc_cali_scheme_line_fitting_check_efuse(&source);
        if (err != ESP_OK) {
            return err;
        }
        // 无 eFuse 数据时仍用 line fitting + 标称 Vref：12 dB 档非线性，理想线性换算在 1.65 V 处偏高约 20%，会落出 ID 窗口。
        if (source == ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF) {
            FW_LOGW(fw::TraceCat::kSys, "adc%d calibration eFuse missing, using nominal Vref",
                    static_cast<int>(unitId) + 1);
        }
        const adc_cali_line_fitting_config_t caliConfig{.unit_id = unitId,
                                                        .atten = kAttenuation,
                                                        .bitwidth = kBitWidth,
                                                        .default_vref = kNominalVrefMv};
        return adc_cali_create_scheme_line_fitting(&caliConfig, &cali_);
    }

    /**
     * @brief 读取一次通道原始值
     * @param[out] raw: 成功时为 12 位转换码，失败时由 SDK 决定
     * @retval ESP_OK 读数有效
     * @retval 其他 SDK 读取错误
     * @note 仅持有句柄的启动任务调用，非 ISR。
     */
    [[nodiscard]] esp_err_t read(int &raw) const noexcept
    {
        return adc_oneshot_read(unit_, channel_, &raw);
    }

    /**
     * @brief 将已读取的原始值换算为 mV
     * @param raw: 原始值或原始值均值
     * @param[out] mv: 成功时写入 mV，失败时保留原值
     * @retval ESP_OK 换算成功
     * @retval ESP_ERR_INVALID_STATE 未装配校准
     * @retval 其他 SDK 校准错误
     * @note 仅启动任务调用，非 ISR。
     */
    [[nodiscard]] esp_err_t toMv(int raw, uint32_t &mv) const noexcept
    {
        int value = 0;
        if (cali_ != nullptr) {
            const esp_err_t err = adc_cali_raw_to_voltage(cali_, raw, &value);
            if (err != ESP_OK) {
                return err;
            }
        } else {
            return ESP_ERR_INVALID_STATE;
        }
        // ADC 换算结果为非负毫伏，钳制到零后符号转换安全。
        mv = value > 0 ? static_cast<uint32_t>(value) : 0U;
        return ESP_OK;
    }

private:
    adc_oneshot_unit_handle_t unit_ = nullptr;
    adc_cali_handle_t cali_ = nullptr;
    adc_channel_t channel_{};
};

/**
 * @brief 读取 GPIO35 的八次平均 ID 电压，丢弃首样
 * @param[out] mv: 成功时写入 mV，失败时保留原值
 * @retval ESP_OK 读数有效
 * @retval 其他 SDK ADC 错误
 * @note 仅启动任务调用，非 ISR；返回时释放 ADC1。
 */
[[nodiscard]] esp_err_t readIdMv(uint32_t &mv) noexcept
{
    BootAdc adc{};
    esp_err_t err = adc.init(ADC_UNIT_1, ADC_CHANNEL_7);
    int raw = 0;
    if (err == ESP_OK) {
        err = adc.read(raw);
    }
    int sum = 0;
    for (int i = 0; i < kIdSamples && err == ESP_OK; ++i) {
        err = adc.read(raw);
        if (err == ESP_OK) {
            sum += raw;
        }
    }
    return err == ESP_OK ? adc.toMv((sum + kIdSamples / 2) / kIdSamples, mv) : err;
}

/**
 * @brief 先设输出锁存电平，再启用推挽输出以避免切换毛刺
 * @param output: ADR 已确认可输出的引脚及初始电平
 * @retval ESP_OK 电平及方向均配置完成
 * @retval 其他 SDK GPIO 错误
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] esp_err_t setOutput(const SafeOutput &output) noexcept
{
    // 调用方只传已登记且支持输出的 GPIO，枚举和位掩码转换安全。
    const auto pin = static_cast<gpio_num_t>(output.gpio);
    const esp_err_t err = gpio_set_level(pin, output.high ? 1U : 0U);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t config{.pin_bit_mask = 1ULL << static_cast<unsigned>(output.gpio),
                               .mode = GPIO_MODE_OUTPUT,
                               .pull_up_en = GPIO_PULLUP_DISABLE,
                               .pull_down_en = GPIO_PULLDOWN_DISABLE,
                               .intr_type = GPIO_INTR_DISABLE};
    return gpio_config(&config);
}

/**
 * @brief 将已识别板型的输出置为板级关闭电平
 * @param profile: 通过识别与 UART 检查的表项
 * @retval kOk 全部设置完成
 * @retval kIoError 任一 GPIO 设置失败
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] fw::Status setSafeOutputs(const Profile &profile) noexcept
{
    fw::Status status = fw::Status::kOk;
    for (const SafeOutput &output : profile.safeOutputs) {
        const esp_err_t err = setOutput(output);
        if (err != ESP_OK) {
            FW_LOGE(fw::TraceCat::kSys, "board safe GPIO%d failed: %s", output.gpio,
                    esp_err_to_name(err));
            status = fw::Status::kIoError;
        }
    }
    return status;
}

/**
 * @brief 按 10 ms 周期等待连续三个达标读数
 * @param adc: 已准备的 ADC2 通道
 * @param hold: 保持阈值与超时
 * @param startedMs: GPIO4 拉高前的单调时钟（ms）
 * @param[out] mv: 最后一次有效电压（mV）；无有效样本时保留原值
 * @retval ESP_OK 达标且未超时
 * @retval ESP_ERR_TIMEOUT 超时或 ADC 读取超时
 * @retval 其他 SDK ADC 错误
 * @note 仅启动任务调用，非 ISR，等待中让出 CPU。
 */
[[nodiscard]] esp_err_t waitForHold(const BootAdc &adc, const PowerHold &hold, uint32_t startedMs,
                                    uint32_t &mv) noexcept
{
    uint8_t streak = 0U;
    const TickType_t delay = pdMS_TO_TICKS(kHoldPollMs);
    while (true) {
        vTaskDelay(delay > 0U ? delay : 1U);
        if (rtos::nowMs() - startedMs >= hold.holdTimeoutMs) {
            return ESP_ERR_TIMEOUT;
        }
        int raw = 0;
        esp_err_t err = adc.read(raw);
        if (err == ESP_OK) {
            err = adc.toMv(raw, mv);
        }
        if (err != ESP_OK) {
            return err;
        }
        switch (checkHold(hold, mv, rtos::nowMs() - startedMs, streak)) {
        case HoldDecision::kHeld:
            return ESP_OK;
        case HoldDecision::kTimeout:
            return ESP_ERR_TIMEOUT;
        case HoldDecision::kContinue:
        default:
            break;
        }
    }
}

/**
 * @brief 模拟按键并检查电源保持，拉高后的所有路径都尝试释放按键
 * @param hold: 已识别 P2 板型的电源参数
 * @retval kOk 三次连续达标并释放按键
 * @retval kIoError 设置 GPIO、初始化 ADC 或校验读数失败
 * @retval kTimeout 保持未在时限内完成
 * @note 仅启动任务调用，非 ISR；ADC2 释放发生在按键释放后。
 */
[[nodiscard]] fw::Status emulateKey(const PowerHold &hold) noexcept
{
    BootAdc adc{};
    adc_unit_t unit{};
    adc_channel_t channel{};
    esp_err_t err = adc_oneshot_io_to_channel(hold.senseGpio, &unit, &channel);
    if (err == ESP_OK) {
        err = adc.init(unit, channel); // oneshot 配置将该 RTC pad 置为输入并关闭上下拉。
    }
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power sense adc init failed: %s", esp_err_to_name(err));
        return fw::Status::kIoError;
    }

    // GPIO4 已在识别步骤以低电平配置为输出，枚举转换安全。
    const auto key = static_cast<gpio_num_t>(hold.keyGpio);
    const uint32_t startedMs = rtos::nowMs();
    uint32_t mv = 0U;
    err = gpio_set_level(key, 1U);
    if (err == ESP_OK) {
        err = waitForHold(adc, hold, startedMs, mv);
    }
    const esp_err_t releaseErr = gpio_set_level(key, 0U);
    if (releaseErr != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power hold release failed: %s", esp_err_to_name(releaseErr));
        return fw::Status::kIoError;
    }
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power hold failed: %s sense=%" PRIu32 " mV",
                esp_err_to_name(err), mv);
        return err == ESP_ERR_TIMEOUT ? fw::Status::kTimeout : fw::Status::kIoError;
    }
    FW_LOGI(fw::TraceCat::kSys, "power hold ok sense=%" PRIu32 " mV after %" PRIu32 " ms", mv,
            rtos::nowMs() - startedMs);
    return fw::Status::kOk;
}

/**
 * @brief 配置启动 USB 检测所用的无内部上下拉输入脚
 * @param gpio: 板级引脚编号，小于 0 时跳过
 * @retval 配置成功时返回引脚编号，否则返回 -1
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] int configureBootInput(int gpio) noexcept
{
    if (gpio < 0) {
        return -1;
    }
    // 引脚来自板级静态表，掩码转换安全。
    const gpio_config_t config{.pin_bit_mask = 1ULL << static_cast<unsigned>(gpio),
                               .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_DISABLE,
                               .pull_down_en = GPIO_PULLDOWN_DISABLE,
                               .intr_type = GPIO_INTR_DISABLE};
    return gpio_config(&config) == ESP_OK ? gpio : -1;
}

/**
 * @brief 读取已配置的启动检测输入脚
 * @param gpio: 配置成功的引脚编号，小于 0 表示不可用
 * @param activeLow: true 表示低电平有效
 * @retval 引脚有效态；不可用时返回 nullopt
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] std::optional<bool> readInputPin(int gpio, bool activeLow) noexcept
{
    if (gpio < 0) {
        return std::nullopt;
    }
    // 非负编号来自已配置的板级输入脚，枚举转换安全。
    const int level = gpio_get_level(static_cast<gpio_num_t>(gpio));
    return activeLow ? (level == 0) : (level != 0);
}

/**
 * @brief 拉 GPIO4 之前用 ADC2 单次采样 GPIO25（U3 OUT）电压
 * @param senseGpio: 检测引脚编号
 * @param[out] mv: 成功时写入电压（mV）
 * @retval ESP_OK 采样成功
 * @retval 其他 SDK ADC 错误
 * @note 仅启动任务在蓝牙/Wi-Fi 启动前调用，返回时释放 ADC2，非 ISR。
 */
[[nodiscard]] esp_err_t readSenseOnce(int senseGpio, uint32_t &mv) noexcept
{
    BootAdc adc{};
    adc_unit_t unit{};
    adc_channel_t channel{};
    esp_err_t err = adc_oneshot_io_to_channel(senseGpio, &unit, &channel);
    if (err == ESP_OK) {
        err = adc.init(unit, channel);
    }
    int raw = 0;
    if (err == ESP_OK) {
        err = adc.read(raw);
    }
    if (err == ESP_OK) {
        err = adc.toMv(raw, mv);
    }
    return err;
}

/**
 * @brief 启动早期探测 SGM41513，在线时写入并校验充电参数
 * @param pins: 板级充电芯片配置
 * @note 仅启动任务调用；失败降级，I2C 调用可能阻塞，非 ISR。
 */
void initChargerAtBoot(const ChargerPins &pins) noexcept
{
    if (!fw::isOk(charger::init(pins))) {
        return;
    }
    const auto status = charger::applyChargeParams(pins);
    if (!fw::isOk(status) && status != fw::Status::kCorrupt) {
        FW_LOGW(fw::TraceCat::kSys, "charger: charge parameters apply failed (%s)",
                fw::toString(status));
    }
}

/**
 * @brief 启动时按 GPIO27、在线 REG08、STAT 的顺序读取 USB 在位
 * @param key: 已配置的检测引脚与有效电平，不可用引脚的编号小于 0
 * @retval 首个可用来源的 USB 在位样本
 * @note 仅启动任务调用，I2C 传输最多等待 10 ms，非 ISR。
 */
[[nodiscard]] UsbDetection readBootUsb(const PowerKey &key) noexcept
{
    const auto gpio = readInputPin(key.usbDetectGpio, key.usbActiveLow);
    ChargerInputStatus i2c{};
    if (!gpio.has_value() && charger::isReady() && !fw::isOk(charger::refreshInputStatus(i2c))) {
        i2c = {};
    }
    const auto stat = !gpio.has_value() && !i2c.ok ? readInputPin(key.chgStatGpio, key.chgActiveLow)
                                                   : std::nullopt;
    return decideUsbPresent(gpio, i2c, stat);
}

/**
 * @brief 按启动分支决策执行 P2 电源保持或进入充电待机
 * @param hold: 板级电源保持配置
 * @param action: 启动分支决策结果
 * @param initialMv: 拉高按键前测得的 GPIO25 电压（mV）
 * @param usb: 按检测来源优先级取得的 USB 在位结论
 * @param[out] bootMode: 输出运行档
 * @param[out] reason: 输出开机原因
 * @retval kOk 保持完成或进入充电待机
 * @retval 其他 保持失败错误码
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] fw::Status executePowerOnAction(const PowerHold &hold, PowerOnAction action,
                                              uint32_t initialMv, const UsbDetection &usb,
                                              BootMode &bootMode, PowerOnReason &reason) noexcept
{
    switch (action) {
    case PowerOnAction::kChargeStandby:
        bootMode = BootMode::kChargeStandby;
        reason = PowerOnReason::kCharge;
        FW_LOGI(fw::TraceCat::kSys, "power_on=charge sense=%" PRIu32 " mV usb=1 src=%s", initialMv,
                usbSourceName(usb.source));
        return fw::Status::kOk;
    case PowerOnAction::kHoldRecoverWarn:
        reason = PowerOnReason::kKey;
        FW_LOGW(fw::TraceCat::kSys,
                "power_on=key sense=%" PRIu32 " mV low without usb (src=%s), forcing hold",
                initialMv, usbSourceName(usb.source));
        return emulateKey(hold);
    case PowerOnAction::kHoldConfirm:
    default:
        reason = PowerOnReason::kKey;
        FW_LOGI(fw::TraceCat::kSys, "power_on=key sense=%" PRIu32 " mV usb=%u src=%s", initialMv,
                usb.present ? 1U : 0U, usbSourceName(usb.source));
        return emulateKey(hold);
    }
}

} // namespace

fw::Status applySafeOutputs(const Profile &profile) noexcept
{
    return setSafeOutputs(profile);
}

fw::Status setSafeOutputLevels(const Profile &profile) noexcept
{
    fw::Status status = fw::Status::kOk;
    for (const SafeOutput &output : profile.safeOutputs) {
        // 4G PWREN 由 NetModule 的 PW1 参与者在优雅关机（QPOWD）完成后自行拉低；这里若提前置低
        // 会在 AT+QPOWD 写入 flash 期间硬掉电（ADR-0021 §2.9）。整板断电由 kCutPower 保证。
        if (hasFeature(profile, Feature::kCellular) && output.gpio == profile.cellular.pwrenGpio) {
            continue;
        }
        // 引脚已在启动识别阶段经 setSafeOutputs 配为输出，仅刷新电平避免重复 gpio_config 告警。
        const esp_err_t err =
            gpio_set_level(static_cast<gpio_num_t>(output.gpio), output.high ? 1U : 0U);
        if (err != ESP_OK) {
            FW_LOGE(fw::TraceCat::kSys, "board safe level GPIO%d failed: %s", output.gpio,
                    esp_err_to_name(err));
            status = fw::Status::kIoError;
        }
    }
    return status;
}

fw::Status identify(const Profile *&selected) noexcept
{
    selected = nullptr;
    uint32_t mv = 0U;
    const esp_err_t err = readIdMv(mv);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "board id adc failed: %s", esp_err_to_name(err));
        return fw::Status::kIoError;
    }
#if defined(CONFIG_RTK_BOARD_FORCE_E1_DEVKIT)
    const Profile *profile = byId(BoardId::kE1DevKit);
#elif defined(CONFIG_RTK_BOARD_FORCE_P2_V141)
    const Profile *profile = byId(BoardId::kP2V141);
#else
    const Profile *profile = matchById(mv);
#endif
    if (profile == nullptr) {
        FW_LOGE(fw::TraceCat::kSys, "board unknown id=%" PRIu32 " mV", mv);
        return fw::Status::kNotFound;
    }
#if defined(CONFIG_RTK_GNSS_UART_NUM)
    if (profile->gnss.uart != CONFIG_RTK_GNSS_UART_NUM) {
        FW_LOGE(fw::TraceCat::kSys, "board %s requires UART%d, GNSS configured for UART%d",
                profile->name, profile->gnss.uart, CONFIG_RTK_GNSS_UART_NUM);
        return fw::Status::kInvalidArg;
    }
#endif
#if defined(CONFIG_RTK_BOARD_FORCE_E1_DEVKIT) || defined(CONFIG_RTK_BOARD_FORCE_P2_V141)
    FW_LOGI(fw::TraceCat::kSys, "board %s id=%" PRIu32 " mV (forced)", profile->name, mv);
#else
    FW_LOGI(fw::TraceCat::kSys, "board %s id=%" PRIu32 " mV", profile->name, mv);
#endif
    const fw::Status status = setSafeOutputs(*profile);
    if (fw::isOk(status)) {
        selected = profile;
    }
    return status;
}

fw::Status enablePowerHold(const PowerHold &hold) noexcept
{
    switch (hold.kind) {
    case HoldKind::kNone:
        return fw::Status::kOk;
    case HoldKind::kLatchGpio: {
        const esp_err_t err = setOutput(hold.latch);
        if (err != ESP_OK) {
            FW_LOGE(fw::TraceCat::kSys, "power hold GPIO%d failed: %s", hold.latch.gpio,
                    esp_err_to_name(err));
            return fw::Status::kIoError;
        }
        return fw::Status::kOk;
    }
    case HoldKind::kKeyEmulate:
        return emulateKey(hold);
    default:
        return fw::Status::kInvalidArg;
    }
}

fw::Status startupPower(const PowerHold &hold, const PowerKey &key, const ChargerPins &chargerPins,
                        BootMode &bootMode, PowerOnReason &reason) noexcept
{
    bootMode = BootMode::kNormal;
    if (hold.kind != HoldKind::kKeyEmulate) {
        reason = hold.kind == HoldKind::kLatchGpio ? PowerOnReason::kLatch : PowerOnReason::kKey;
        return enablePowerHold(hold);
    }

    initChargerAtBoot(chargerPins);

    uint32_t initialMv = 0U;
    esp_err_t err = readSenseOnce(hold.senseGpio, initialMv);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power initial sense read failed: %s", esp_err_to_name(err));
        return fw::Status::kIoError;
    }
    PowerKey usbPins = key;
    usbPins.usbDetectGpio = configureBootInput(key.usbDetectGpio);
    usbPins.chgStatGpio = configureBootInput(key.chgStatGpio);
    const UsbDetection firstUsb = readBootUsb(usbPins);
    const bool senseHigh = initialMv >= hold.senseOnMv;
    const UsbDetection usb = waitForBootUsb(
        senseHigh, firstUsb, rtos::nowMs,
        [](uint32_t delayMs) noexcept {
            const TickType_t ticks = pdMS_TO_TICKS(delayMs);
            vTaskDelay(ticks == 0U ? 1U : ticks);
        },
        [&]() noexcept { return readBootUsb(usbPins); });
    if (!senseHigh && !firstUsb.present) {
        // 等待充电输入识别期间，物理按键可能已将 U3 打开。
        err = readSenseOnce(hold.senseGpio, initialMv);
        if (err != ESP_OK) {
            FW_LOGE(fw::TraceCat::kSys, "power sense reread failed: %s", esp_err_to_name(err));
            return fw::Status::kIoError;
        }
    }
    return executePowerOnAction(hold, decidePowerOn(initialMv >= hold.senseOnMv, usb.present),
                                initialMv, usb, bootMode, reason);
}

} // namespace app::board
