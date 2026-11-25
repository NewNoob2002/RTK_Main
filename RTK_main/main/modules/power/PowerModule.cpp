// 电源按键与关机管理模块实现（kLink 执行器）。
// init 仅启动任务调用，可等待首组 ADC 样本；poll 仅 kLink 调用；requestShutdown 任意任务安全。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/power/PowerModule.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "app/PowerRequests.hpp"
#include "def/GnssSubDef.hpp"
#include "board/BatteryAdc.hpp"
#include "board/BoardProfile.hpp"
#include "board/BoardStartup.hpp"
#include "board/Charger.hpp"
#include "fw/Atomic.hpp"
#include "fw/Log.hpp"
#include "fw/Trace.hpp"
#include "modules/storage/StoreWriteGate.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

constexpr uint32_t kQuiesceMinWaitMs = 30U;     // ms，给 trace_out 一次唤醒窗口
constexpr uint32_t kQuiesceMaxWaitMs = 150U;    // ms，日志排空等待上限
constexpr uint32_t kWifiQuiesceWaitMs = 5500U;  // ms，覆盖 HTTP socket 默认 5 s 超时
constexpr uint32_t kRestartWaitMs = 150U;       // ms，异步日志和 UART 排空上限
constexpr uint32_t kOledFinalFlushMaxMs = 30U;  // ms，USB 关机终帧 I2C 刷出等待上限（OL2）
constexpr uint32_t kChargerPollPeriodMs = 200U; // ms，运行期 SGM41513 I2C 采样周期
constexpr uint32_t kBatteryPeriodMs = 1000U;    // ms，组间最短等待时间
constexpr uint32_t kBootBatterySpacingMs = 5U;  // ms，旧 P2 首组组内采样间隔
constexpr uint32_t kBatteryFreshMs = 2000U;     // ms，超期旧读数不用于低电关机

constinit std::atomic<TaskHandle_t> s_restart_task_handle{nullptr};
static_assert(fw::kAtomicUsable<TaskHandle_t>, "电源重启唤醒任务句柄原子须无锁");

/**
 * @brief 配置单个输入 GPIO（无内部上下拉，关中断）
 * @param gpio: 引脚编号，小于 0 时跳过
 * @retval ESP_OK 配置成功或引脚未配置
 * @retval 其他 SDK GPIO 错误
 * @note 仅启动任务或 kLink 恢复路径调用，非 ISR。
 */
[[nodiscard]] esp_err_t configPlainInput(int gpio) noexcept
{
    if (gpio < 0) {
        return ESP_OK;
    }
    const gpio_config_t config{.pin_bit_mask = 1ULL << static_cast<unsigned>(gpio),
                               .mode = GPIO_MODE_INPUT,
                               .pull_up_en = GPIO_PULLUP_DISABLE,
                               .pull_down_en = GPIO_PULLDOWN_DISABLE,
                               .intr_type = GPIO_INTR_DISABLE};
    return gpio_config(&config);
}

/**
 * @brief 读取已配置输入脚的有效逻辑状态
 * @param gpio: 引脚编号，小于 0 时返回 false
 * @param activeLow: true 表示低电平有效
 * @retval true 引脚处于有效态
 * @retval false 引脚无效或处于非有效态
 * @note 任意任务可调用，不阻塞，非 ISR。
 */
[[nodiscard]] bool readPinActive(int gpio, bool activeLow) noexcept
{
    if (gpio < 0) {
        return false;
    }
    // 引脚来自板级静态表，枚举转换安全。
    const int level = gpio_get_level(static_cast<gpio_num_t>(gpio));
    return activeLow ? (level == 0) : (level != 0);
}

/**
 * @brief 立即检查控制台 UART 是否已排空
 * @retval true 已排空或没有 UART 发送驱动
 * @retval false 发送仍占用 UART
 * @note 仅 kLink 调用，不等待，非 ISR。
 */
[[nodiscard]] bool consoleUartDrained() noexcept
{
#if defined(CONFIG_RTK_CONSOLE_UART_NUM)
    // Kconfig 限定控制台 UART 号为 0..2，枚举转换安全。
    constexpr auto kPort = static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM);
    return uart_wait_tx_done(kPort, 0U) != ESP_ERR_TIMEOUT;
#else
    return true;
#endif
}

/**
 * @brief 检查关门后投递的队尾命令
 * @param executor: 目标执行器
 * @param kind: 目标模块
 * @param verb: 队尾命令动词
 * @param[in,out] ticket: 该队尾命令的凭据
 * @retval true 队尾命令已执行，或对应执行器不存在
 * @retval false 等待队列容量或命令完成
 * @note 仅 kLink 调用，不阻塞，非 ISR。
 */
[[nodiscard]] bool reachedSaveFence(rtos::Executor *executor, ModuleKind kind, uint8_t verb,
                                    rtos::Ticket &ticket) noexcept
{
    if (executor == nullptr) {
        return true;
    }
    if (ticket.seq == 0U) {
        const fw::Command cmd{.target = id(kind), .verb = verb};
        const auto posted = executor->post(cmd, rtos::Caller::kInternal, ticket);
        return posted == fw::Result::kNotFound;
    }
    fw::CommandResult result{};
    const auto state = executor->poll(ticket, result);
    if (state == rtos::PollState::kDone) {
        // GGA 冲刷失败仍代表前序命令已出队，实际待写状态另由 kIo 发布。
        ticket = {};
        return true;
    }
    if (state != rtos::PollState::kPending) {
        ticket = {};
    }
    return false;
}

/**
 * @brief 比较两份电源快照是否完全一致（避免无变化时重复加锁写 Slot）
 * @param lhs: 快照 A
 * @param rhs: 快照 B
 * @retval true 字段全部相等
 * @retval false 存在字段变化
 * @note 纯比较，不阻塞，非 ISR。
 */
[[nodiscard]] bool snapshotEqual(const PowerSnapshot &lhs, const PowerSnapshot &rhs) noexcept
{
    return (lhs.active == rhs.active) && (lhs.keyPressed == rhs.keyPressed) &&
           (lhs.u3On == rhs.u3On) && (lhs.usbPresent == rhs.usbPresent) &&
           (lhs.charging == rhs.charging) && (lhs.usbSource == rhs.usbSource) &&
           (lhs.batteryAdcReady == rhs.batteryAdcReady) &&
           (lhs.chargerPresent == rhs.chargerPresent) && (lhs.chargerOk == rhs.chargerOk) &&
           (lhs.chargerReg08 == rhs.chargerReg08) && (lhs.batteryMv == rhs.batteryMv) &&
           (lhs.batteryPct == rhs.batteryPct) && (lhs.bootMode == rhs.bootMode) &&
           (lhs.powerOn == rhs.powerOn) && (lhs.shutdownStage == rhs.shutdownStage) &&
           (lhs.lastShutdown == rhs.lastShutdown) &&
           (lhs.shutdownDoneMask == rhs.shutdownDoneMask) &&
           (lhs.shutdownDegradedMask == rhs.shutdownDegradedMask) &&
           (lhs.shortPresses == rhs.shortPresses) && (lhs.longPresses == rhs.longPresses);
}

} // namespace

namespace power {

bool requestShutdown(board::ShutdownReason reason) noexcept
{
    const auto &app = appContext();
    return app.data.powerStatus.read().active &&
           s_power_requests.requestShutdown(app.bootMode, reason);
}

} // namespace power

bool PowerModule::configureInputPins() noexcept
{
    const auto &profile = board::current();
    const auto configure = [](int pin, bool required) noexcept {
        if (pin < 0) {
            return false;
        }
        const esp_err_t err = configPlainInput(pin);
        if (err == ESP_OK) {
            return true;
        }
        if (required) {
            FW_LOGE(fw::TraceCat::kSys, "power input GPIO%d config failed: %s", pin,
                    esp_err_to_name(err));
        } else {
            FW_LOGW(fw::TraceCat::kSys, "power input GPIO%d unavailable: %s", pin,
                    esp_err_to_name(err));
        }
        return false;
    };
    if (!configure(profile.powerKey.keyDetectGpio, true) ||
        !configure(profile.powerHold.senseGpio, true)) {
        return false;
    }
    usbGpioAvailable_ = configure(profile.powerKey.usbDetectGpio, false);
    statGpioAvailable_ = configure(profile.powerKey.chgStatGpio, false);
    return true;
}

fw::Result PowerModule::init() noexcept
{
    if (context_ == nullptr) {
        return fw::Result::kInvalidState;
    }
    keyDriveHigh_ = false;
    forcePowerOffFired_ = false;
    restartPending_ = false;
    restartStartMs_ = 0U;
    restartLogReady_ = false;
    oledWaitDeadlineMs_ = 0U;
    resetShutdownProgress();
    shortPresses_ = 0U;
    longPresses_ = 0U;
    shutdownSm_ = board::ShutdownSm{};
    const auto &profile = board::current();
    if (profile.powerHold.kind != board::HoldKind::kKeyEmulate) {
        active_ = false;
        publishSnapshot(PinLevels{}, true);
        return fw::Result::kOk;
    }
    if (!configureInputPins()) {
        active_ = false;
        return fw::Result::kIoError;
    }
    keySm_.reset(kPowerOffPressMs);
    ctx().data.uiEvents.updateKeyHold(false, 0U, keySm_.longPressMs(), false);
    chargerSampled_ = false;
    chargerRetry_ = false;
    lastChargerStatus_ = board::ChargerInputStatus{};
    active_ = true;
    initBattery(profile.batteryAdcGpio);
    const PinLevels pins = samplePins(rtos::nowMs(), true);
    publishSnapshot(pins, true);
    // 在线与否由启动探测决定；GPIO27 可用时无需额外读 REG08。
    const char *const charger = board::charger::isReady() ? "ok" : "absent";
    if (batteryPct_ == board::kBatteryPctUnknown) {
        FW_LOGI(fw::TraceCat::kSys, "charger=%s usb=%u src=%s bat=n/a n/a", charger,
                pins.usbPresent ? 1U : 0U, board::usbSourceName(pins.usbSource));
    } else {
        FW_LOGI(fw::TraceCat::kSys, "charger=%s usb=%u src=%s bat=%" PRIu32 "mV %u%%", charger,
                pins.usbPresent ? 1U : 0U, board::usbSourceName(pins.usbSource), batteryMv_,
                static_cast<unsigned>(batteryPct_));
    }
    return fw::Result::kOk;
}

PowerModule::PinLevels PowerModule::samplePins(uint32_t nowMs, bool forceChargerRead) noexcept
{
    const auto &profile = board::current();
    const auto gpioUsb = usbGpioAvailable_
                             ? std::optional{readPinActive(profile.powerKey.usbDetectGpio,
                                                           profile.powerKey.usbActiveLow)}
                             : std::nullopt;
    const auto stat = statGpioAvailable_
                          ? std::optional{readPinActive(profile.powerKey.chgStatGpio,
                                                        profile.powerKey.chgActiveLow)}
                          : std::nullopt;
    if (!gpioUsb.has_value() && board::charger::isReady() &&
        (forceChargerRead || (shutdownSm_.stage() != board::ShutdownStage::kQuiesce &&
                              (chargerRetry_ || !chargerSampled_ ||
                               nowMs - lastChargerPollMs_ >= kChargerPollPeriodMs)))) {
        const auto status = board::charger::refreshInputStatus(lastChargerStatus_);
        chargerRetry_ = status == fw::Status::kBusy;
        if (!chargerRetry_) {
            lastChargerPollMs_ = nowMs;
            chargerSampled_ = true;
        }
    }
    const board::UsbDetection usb = board::decideUsbPresent(gpioUsb, lastChargerStatus_, stat);
    const bool fresh =
        chargerSampled_ && !chargerRetry_ && nowMs - lastChargerPollMs_ < kChargerPollPeriodMs;
    return PinLevels{
        .keyPressed = readPinActive(profile.powerKey.keyDetectGpio, profile.powerKey.keyActiveLow),
        .u3On = readPinActive(profile.powerHold.senseGpio, false),
        .usbPresent = usb.present,
        // STAT 高也可能是充满；REG08 缓存遇到争用时不能用于断电决策。
        .usbKnown = gpioUsb.has_value() || (fresh && lastChargerStatus_.ok) ||
                    (usb.source == board::UsbSource::kStat && usb.present),
        .charging = stat.value_or(false),
        .usbSource = usb.source,
        .chargerOk = lastChargerStatus_.ok,
        .chargerReg08 = lastChargerStatus_.rawReg08,
    };
}

void PowerModule::initBattery(int gpio) noexcept
{
    batterySampleCount_ = 0U;
    lastBatterySampleMs_ = 0U;
    batteryMv_ = 0U;
    batteryPct_ = board::kBatteryPctUnknown;
    lowBattery_ = {};
    const auto status = board::initBatteryAdc(gpio);
    batteryAdcReady_ = fw::isOk(status);
    if (!batteryAdcReady_) {
        if (status != fw::Status::kNotFound) {
            FW_LOGW(fw::TraceCat::kSys, "<battery> ADC init failed (%s)", fw::toString(status));
        }
        return;
    }
    for (size_t i = 0U; i < kBatterySamples; ++i) {
        const auto sample = takeBatterySample(rtos::nowMs());
        if (!fw::isOk(sample)) {
            FW_LOGW(fw::TraceCat::kSys, "<battery> first sample failed (%s)", fw::toString(sample));
            break;
        }
        if (i + 1U < kBatterySamples) {
            vTaskDelay(pdMS_TO_TICKS(kBootBatterySpacingMs));
        }
    }
    lastBatteryBatchMs_ = rtos::nowMs();
}

fw::Status PowerModule::takeBatterySample(uint32_t nowMs) noexcept
{
    const auto status = board::readBatteryAdcMv(batterySamples_[batterySampleCount_]);
    if (!fw::isOk(status)) {
        batterySampleCount_ = 0U;
        batteryMv_ = 0U;
        batteryPct_ = board::kBatteryPctUnknown;
        return status;
    }
    lastBatterySampleMs_ = nowMs;
    if (++batterySampleCount_ == kBatterySamples) {
        // 中值抑制短时负载尖峰，采样间隔避免在同一尖峰内完成整组转换。
        std::sort(batterySamples_.begin(), batterySamples_.end());
        batteryMv_ =
            (batterySamples_[kBatterySamples / 2U - 1U] + batterySamples_[kBatterySamples / 2U]) /
            2U;
        batteryPct_ = board::batteryPercent(batteryMv_);
        batterySampleCount_ = 0U;
    }
    return fw::Status::kOk;
}

void PowerModule::pollBattery(uint32_t nowMs, const PinLevels &pins) noexcept
{
    if (!batteryAdcReady_) {
        return;
    }
    // 在本轮采样前检查间隔，避免将轮询中断跨过的时间计入连续低电。
    const bool fresh = nowMs - lastBatterySampleMs_ < kBatteryFreshMs;
    if (nowMs - lastBatteryBatchMs_ >= kBatteryPeriodMs) {
        const bool wasValid = batteryPct_ != board::kBatteryPctUnknown;
        const auto status = takeBatterySample(nowMs);
        if (batterySampleCount_ == 0U) {
            lastBatteryBatchMs_ = nowMs;
        }
        if (!fw::isOk(status) && wasValid) {
            FW_LOGW(fw::TraceCat::kSys, "<battery> ADC sample lost (%s)", fw::toString(status));
        }
    }
    const bool onBattery = pins.usbKnown && !pins.usbPresent;
    if (ctx().bootMode != board::BootMode::kNormal ||
        !power::s_power_requests.canScheduleRestart()) {
        lowBattery_ = {};
        return;
    }
    if (lowBattery_.isDue(nowMs, onBattery && fresh, batteryPct_) &&
        power::requestShutdown(board::ShutdownReason::kLowBattery)) {
        FW_LOGW(fw::TraceCat::kSys, "<battery> low battery shutdown at %" PRIu32 " mV", batteryMv_);
        lowBattery_ = {};
    }
}

void PowerModule::setKeyDrive(bool high) noexcept
{
    const int keyGpio = board::current().powerHold.keyGpio;
    if (keyGpio < 0) {
        return;
    }
    if (keyDriveHigh_ == high) {
        return;
    }
    // GPIO4 已由 B1 安全输出表配置为输出，枚举转换安全。
    const esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(keyGpio), high ? 1U : 0U);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power key drive GPIO%d=%u failed: %s", keyGpio, high ? 1U : 0U,
                esp_err_to_name(err));
        return;
    }
    keyDriveHigh_ = high;
}

void PowerModule::handleChargeStandbyPowerOn(uint32_t nowMs, bool longPress,
                                             bool senseHigh) noexcept
{
    switch (standbyWake_.confirmStandbyHold(nowMs, longPress, senseHigh,
                                            board::current().powerHold.holdTimeoutMs)) {
    case board::StandbyWakeAction::kPress:
        setKeyDrive(true);
        if (!keyDriveHigh_) {
            standbyWake_ = {};
        }
        break;
    case board::StandbyWakeAction::kHeld:
        setKeyDrive(false);
        if (!power::s_power_requests.claimRestart()) {
            break;
        }
        FW_LOGI(fw::TraceCat::kSys, "power hold ok (digital), restarting from charge standby");
        restartPending_ = true;
        restartStartMs_ = nowMs;
        restartLogReady_ = false;
        oledWaitDeadlineMs_ = 0U;
        break;
    case board::StandbyWakeAction::kTimeout:
        setKeyDrive(false);
        FW_LOGE(fw::TraceCat::kSys, "charge standby power hold timed out");
        break;
    case board::StandbyWakeAction::kNone:
    default:
        break;
    }
}

bool PowerModule::isLogDrained(uint32_t elapsed) const noexcept
{
    if (elapsed < kQuiesceMinWaitMs) {
        return false;
    }
#if FW_TRACE_ENABLED
    if (!context_->debug.logOutputIdle()) {
        return false;
    }
#endif
    return consoleUartDrained();
}

void PowerModule::pollSaveFences() noexcept
{
    if (!power::s_power_requests.settingsSubmitted()) {
        return;
    }
    if (!linkFenceReached_) {
        linkFenceReached_ = reachedSaveFence(ctx().linkExecutor, ModuleKind::kStorage,
                                             StorageModule::kSaveFence, linkFence_);
    }
    if (!ioFenceReached_) {
        // 在已获准的 kIo 设置命令之后冲刷，即使该命令是在关门时才入队。
        ioFenceReached_ =
            reachedSaveFence(ctx().ioExecutor, ModuleKind::kGnss,
                             static_cast<uint8_t>(GnssSubVerb::kFlushGgaGate), ioFence_);
    }
}

bool PowerModule::isSaveComplete() const noexcept
{
    if (!linkFenceReached_ || !ioFenceReached_ || context_->storage.hasPendingWrites()) {
        return false;
    }
    if (store::inFlightWrites() != 0U) {
        return false;
    }
    if ((context_->bootMode == board::BootMode::kNormal) &&
        (context_->radioMode == RadioMode::kBluetooth) && context_->bt_link.hasPendingSettings()) {
        return false;
    }
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    if ((context_->bootMode == board::BootMode::kNormal) &&
        (context_->radioMode == RadioMode::kWifiConfig) && context_->web.hasPendingSettings()) {
        return false;
    }
#endif
#if RTK_GNSS_ENABLED
    if ((context_->bootMode == board::BootMode::kNormal) &&
        context_->data.gnssPendingNvs.load(std::memory_order_acquire)) {
        return false;
    }
#endif
    return true;
}

void PowerModule::logSaveTimeoutItems() const noexcept
{
    constexpr size_t kPendingSummaryBytes = 96U; // B，待落盘项名摘要缓冲
    std::array<char, kPendingSummaryBytes> storageItems{};
    context_->storage.formatPendingSummary(storageItems);
    const uint32_t inflight = store::inFlightWrites();
    const bool btPending = (context_->bootMode == board::BootMode::kNormal) &&
                           (context_->radioMode == RadioMode::kBluetooth) &&
                           context_->bt_link.hasPendingSettings();
    bool webPending = false;
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    webPending = (context_->bootMode == board::BootMode::kNormal) &&
                 (context_->radioMode == RadioMode::kWifiConfig) &&
                 context_->web.hasPendingSettings();
#endif
    bool gnssPending = false;
#if RTK_GNSS_ENABLED
    gnssPending = (context_->bootMode == board::BootMode::kNormal) &&
                  context_->data.gnssPendingNvs.load(std::memory_order_acquire);
#endif
    FW_LOGW(fw::TraceCat::kSys,
            "power save timeout after %" PRIu32 " ms; pending storage=%s nvs_inflight=%" PRIu32
            " bt=%u web=%u gnss=%u settings_queue=%u",
            board::kShutdownSaveTimeoutMs, storageItems.data(), inflight, btPending ? 1U : 0U,
            webPending ? 1U : 0U, gnssPending ? 1U : 0U,
            linkFenceReached_ && ioFenceReached_ ? 0U : 1U);
}

void PowerModule::enterQuiesce(uint32_t nowMs) noexcept
{
    if (linkFence_.seq != 0U) {
        ctx().linkExecutor->cancel(linkFence_);
        linkFence_ = {};
    }
    if (ioFence_.seq != 0U) {
        ctx().ioExecutor->cancel(ioFence_);
        ioFence_ = {};
    }
    store::pausePowerSave();
    ctx().storage.finishPowerSave();
    quiesceStartMs_ = nowMs;
    if (ctx().bootMode == board::BootMode::kNormal) {
        if (ctx().radioMode == RadioMode::kBluetooth) {
            ctx().bt_link.setPowerPaused(true);
        }
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
        if (ctx().radioMode == RadioMode::kWifiConfig) {
            ctx().web.setPowerPaused(true);
        }
#endif
    }
    const fw::Status safeStatus = board::setSafeOutputLevels(board::current());
    if (fw::isOk(safeStatus)) {
        peripheralsSafeDone_ = true;
    } else {
        peripheralsSafeFailed_ = true;
        FW_LOGW(fw::TraceCat::kSys, "power quiesce safe outputs failed (%s)",
                fw::toString(safeStatus));
    }
    keyDriveHigh_ = false;
    FW_LOGI(fw::TraceCat::kSys, "device powered down");
}

bool PowerModule::isQuiesceDone(uint32_t nowMs) const noexcept
{
    const uint32_t elapsed = nowMs - quiesceStartMs_;
    if (context_->radioMode == RadioMode::kBluetooth && !context_->bt_link.powerPauseDone() &&
        elapsed < kQuiesceMaxWaitMs) {
        return false;
    }
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    if (context_->radioMode == RadioMode::kWifiConfig && !context_->web.powerPauseDone() &&
        elapsed < kWifiQuiesceWaitMs) {
        return false;
    }
#endif
    return elapsed >= kQuiesceMaxWaitMs || isLogDrained(elapsed);
}

void PowerModule::logQuiesceTimeout() noexcept
{
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    if (ctx().radioMode == RadioMode::kWifiConfig && !ctx().web.powerPauseDone()) {
        FW_LOGW(fw::TraceCat::kSys, "power SoftAP quiesce timed out");
    }
#endif
    if (ctx().radioMode == RadioMode::kBluetooth && !ctx().bt_link.powerPauseDone()) {
        FW_LOGW(fw::TraceCat::kSys, "power BT disconnect timed out");
    }
}

void PowerModule::handleShutdownStageEntry(uint32_t nowMs, const PinLevels &pins,
                                           const board::ShutdownOutput &out) noexcept
{
    stageEnteredMs_ = nowMs;
    stageOnScreenMs_.reset();
    fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(PowerTrace::kShutdownStage), 0U,
              static_cast<uint32_t>(out.stage), static_cast<uint32_t>(shutdownSm_.reason()));
    switch (out.stage) {
    case board::ShutdownStage::kAnnounce:
        FW_LOGI(fw::TraceCat::kSys, "power off reason=%s usb=%u",
                board::shutdownReasonName(shutdownSm_.reason()), pins.usbPresent ? 1U : 0U);
        linkFenceReached_ = false;
        ioFenceReached_ = false;
        resetShutdownProgress();
        ctx().data.uiEvents.resetShutdownFinalFrame();
        break;
    case board::ShutdownStage::kSave:
        store::startPowerSave(nowMs);
        ctx().storage.expeditePendingWrites();
        break;
    case board::ShutdownStage::kQuiesce:
        if (out.saveTimedOut) {
            saveTimedOut_ = true;
            logSaveTimeoutItems();
        } else {
            saveFencesDone_ = true;
            saveFlushedDone_ = true;
        }
        enterQuiesce(nowMs);
        break;
    case board::ShutdownStage::kCutPower:
        cutStartMs_ = nowMs;
        break;
    case board::ShutdownStage::kIdle:
    default:
        break;
    }
}

void PowerModule::handleShutdownCutPower(uint32_t nowMs, const board::ShutdownOutput &out) noexcept
{
    if (out.cutTimedOut) {
        setKeyDrive(false);
        FW_LOGE(fw::TraceCat::kSys, "power off timeout after 30 s, aborting");
        fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(PowerTrace::kShutdownAbort), 0U,
                  static_cast<uint32_t>(shutdownSm_.lastReason()), nowMs - cutStartMs_);
        if (ctx().radioMode == RadioMode::kBluetooth) {
            ctx().bt_link.setPowerPaused(false);
        }
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
        if (ctx().radioMode == RadioMode::kWifiConfig) {
            ctx().web.setPowerPaused(false);
        }
#endif
        store::resumePowerSave();
        resetShutdownProgress();
        restartPending_ = false;
        restartLogReady_ = false;
        ctx().data.uiEvents.resetShutdownFinalFrame();
        power::s_power_requests.releaseAction();
        return;
    }
    if (out.stage != board::ShutdownStage::kCutPower) {
        return;
    }
    setKeyDrive(out.driveKeyHigh);
    if (out.requestRestart) {
        setKeyDrive(false);
        FW_LOGI(fw::TraceCat::kSys, "power off u3 down with usb, restarting");
        restartPending_ = true;
        restartStartMs_ = nowMs;
        restartLogReady_ = false;
        oledWaitDeadlineMs_ = 0U;
        finalHoldDeadlineMs_.reset();
        finalFrameReady_ = false;
    }
}

void PowerModule::stepShutdown(uint32_t nowMs, PinLevels &pins) noexcept
{
    if (!shutdownSm_.active() && !shutdownSm_.request(power::s_power_requests.shutdownReason())) {
        return;
    }

    const auto stage = shutdownSm_.stage();
    if (stage == board::ShutdownStage::kSave) {
        pollSaveFences();
        if (linkFenceReached_ && ioFenceReached_) {
            saveFencesDone_ = true;
        }
    }
    const bool saveDone = stage == board::ShutdownStage::kSave && isSaveComplete();
    if (saveDone) {
        saveFlushedDone_ = true;
    }
    if ((stage == board::ShutdownStage::kQuiesce) && isRadioQuiesced()) {
        radioQuiescedDone_ = true;
    }
    const bool stageVisible = shutdownStageVisible(nowMs, stage);
    const bool quiesceDone =
        stage == board::ShutdownStage::kQuiesce && stageVisible && isQuiesceDone(nowMs);
    if (quiesceDone) {
        if (isRadioQuiesced()) {
            radioQuiescedDone_ = true;
        } else {
            radioTimedOut_ = true;
        }
        logQuiesceTimeout();
        pins = samplePins(nowMs, true);
        // 刷新 REG08 与 UART 排空均在 GPIO4 首沿开始计时之前。
        nowMs = rtos::nowMs();
    }
    const auto usb =
        pins.u3On || pins.usbKnown ? std::optional<bool>{pins.usbPresent} : std::nullopt;
    const bool cutReady =
        !ctx().data.uiEvents.oledActive() ||
        (stage == board::ShutdownStage::kCutPower &&
         (pins.usbPresent
              ? stageVisible
              : finalFrameReady(nowMs, cutStartMs_ + board::kShutdownDisplayGraceMs, false)));
    const auto out =
        shutdownSm_.step(nowMs, saveDone, quiesceDone, pins.u3On, usb, stageVisible, cutReady);
    if (out.stageEntered) {
        handleShutdownStageEntry(nowMs, pins, out);
    }
    handleShutdownCutPower(nowMs, out);
}

void PowerModule::resetShutdownProgress() noexcept
{
    finalHoldDeadlineMs_.reset();
    finalFrameReady_ = false;
    saveFencesDone_ = false;
    saveFlushedDone_ = false;
    saveTimedOut_ = false;
    peripheralsSafeDone_ = false;
    peripheralsSafeFailed_ = false;
    radioQuiescedDone_ = false;
    radioTimedOut_ = false;
}

bool PowerModule::isRadioQuiesced() const noexcept
{
    if (context_->bootMode != board::BootMode::kNormal) {
        return true;
    }
    if (context_->radioMode == RadioMode::kBluetooth) {
        return context_->bt_link.powerPauseDone();
    }
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    if (context_->radioMode == RadioMode::kWifiConfig) {
        return context_->web.powerPauseDone();
    }
#endif
    return true;
}

uint8_t PowerModule::computeShutdownDoneMask(const PinLevels &pins) const noexcept
{
    const auto stage = shutdownSm_.stage();
    if (stage == board::ShutdownStage::kIdle) {
        return 0U;
    }
    uint8_t mask = board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced);
    if (saveFencesDone_) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFences);
    }
    if (saveFlushedDone_) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed);
    }
    if (peripheralsSafeDone_) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe);
    }
    if (radioQuiescedDone_) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced);
    }
    if (stage == board::ShutdownStage::kCutPower) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower);
        if (restartPending_) {
            mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady);
        } else if (!pins.usbPresent && (pins.usbKnown || pins.u3On)) {
            mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kBatteryOffReady);
        }
    }
    return mask;
}

uint8_t PowerModule::computeShutdownDegradedMask() const noexcept
{
    if (shutdownSm_.stage() == board::ShutdownStage::kIdle) {
        return 0U;
    }
    uint8_t mask = 0U;
    if (saveTimedOut_) {
        mask |= board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout);
    }
    if (peripheralsSafeFailed_) {
        mask |= board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed);
    }
    if (radioTimedOut_) {
        mask |= board::shutdownDegradedBit(board::ShutdownDegradedBit::kRadioTimeout);
    }
    return mask;
}

void PowerModule::wakeRestartWait(void * /*ctx*/) noexcept
{
    if (const TaskHandle_t handle = s_restart_task_handle.load(std::memory_order_acquire);
        handle != nullptr) {
        static_cast<void>(xTaskNotifyGive(handle));
    }
}

void PowerModule::waitOledFinalFrame(TaskHandle_t self) noexcept
{
    s_restart_task_handle.store(self, std::memory_order_release);
    ctx().data.uiEvents.setRestartWakeHook(&PowerModule::wakeRestartWait, nullptr);
    while (!ctx().data.uiEvents.shutdownFinalFrameFlushed()) {
        const uint32_t curMs = rtos::nowMs();
        if (static_cast<int32_t>(curMs - oledWaitDeadlineMs_) >= 0) {
            break;
        }
        // 读时钟和进入相对等待之间可能被抢占。只等一 tick，恢复后重新核对绝对截止。
        static_cast<void>(ulTaskNotifyTake(pdTRUE, 1U));
    }
    ctx().data.uiEvents.setRestartWakeHook(nullptr, nullptr);
    s_restart_task_handle.store(nullptr, std::memory_order_release);
}
void PowerModule::publishSnapshot(const PinLevels &pins, bool force) noexcept
{
    const PowerSnapshot next{
        .active = active_,
        .keyPressed = pins.keyPressed,
        .u3On = pins.u3On,
        .usbPresent = pins.usbPresent,
        .charging = pins.charging,
        .usbSource = pins.usbSource,
        .batteryAdcReady = batteryAdcReady_,
        .chargerPresent = board::charger::isReady(),
        .chargerOk = pins.chargerOk,
        .chargerReg08 = pins.chargerReg08,
        .batteryPct = batteryPct_,
        .batteryMv = batteryMv_,
        .bootMode = ctx().bootMode,
        .powerOn = ctx().powerOnReason,
        .shutdownStage = shutdownSm_.stage(),
        .lastShutdown = shutdownSm_.lastReason(),
        .shutdownDoneMask = computeShutdownDoneMask(pins),
        .shutdownDegradedMask = computeShutdownDegradedMask(),
        .shortPresses = shortPresses_,
        .longPresses = longPresses_,
    };
    if (!force && (next.usbPresent != lastSnapshot_.usbPresent ||
                   next.usbSource != lastSnapshot_.usbSource)) {
        FW_LOGI(fw::TraceCat::kSys, "<power> usb=%u src=%s", next.usbPresent ? 1U : 0U,
                board::usbSourceName(next.usbSource));
    }
    const bool shutdownProgressChanged =
        !force && ((next.shutdownStage != lastSnapshot_.shutdownStage) ||
                   (next.shutdownDoneMask != lastSnapshot_.shutdownDoneMask) ||
                   (next.shutdownDegradedMask != lastSnapshot_.shutdownDegradedMask));
    if (force || !snapshotEqual(next, lastSnapshot_)) {
        lastSnapshot_ = next;
        ctx().data.powerStatus.write(next);
        // 必须在 powerStatus 写入完成后再唤醒 UI，避免多核/抢占下 OLED 读到旧关机阶段（ROL1-03）。
        if (shutdownProgressChanged) {
            ctx().data.uiEvents.notifyWake();
        }
    }
}

bool PowerModule::shutdownStageVisible(uint32_t nowMs, board::ShutdownStage stage) noexcept
{
    const auto &events = ctx().data.uiEvents;
    if (!events.oledActive()) {
        return true;
    }
    if (!stageOnScreenMs_ && events.shutdownStageFlushed(stage)) {
        stageOnScreenMs_ = nowMs;
    }
    if (stageOnScreenMs_) {
        return nowMs - *stageOnScreenMs_ >= board::kMinStageVisibleMs;
    }
    return nowMs - stageEnteredMs_ >= board::kShutdownDisplayGraceMs + board::kMinStageVisibleMs;
}

bool PowerModule::finalFrameReady(uint32_t nowMs, uint32_t flushDeadlineMs, bool usb) noexcept
{
    const auto &events = ctx().data.uiEvents;
    if (finalFrameReady_ || !events.oledActive()) {
        return true;
    }
    if (!finalHoldDeadlineMs_ && events.shutdownFinalFrameFlushed(usb)) {
        finalHoldDeadlineMs_ = nowMs + board::kShutdownFinalVisibleMs;
    }
    const uint32_t deadline = finalHoldDeadlineMs_.value_or(flushDeadlineMs);
    // 两个等待窗口都远小于半个 uint32_t 周期，差值符号可用于回绕截止判断。
    finalFrameReady_ = static_cast<int32_t>(nowMs - deadline) >= 0;
    if (finalFrameReady_ && !finalHoldDeadlineMs_) {
        FW_LOGW(fw::TraceCat::kSys, "shutdown OLED final frame timed out");
    }
    return finalFrameReady_;
}

bool PowerModule::pollRestart(uint32_t nowMs) noexcept
{
    const uint32_t elapsed = nowMs - restartStartMs_;
    if (!restartLogReady_) {
        if (elapsed < kRestartWaitMs && !isLogDrained(elapsed)) {
            return false;
        }
        restartLogReady_ = true;
        oledWaitDeadlineMs_ = nowMs + kOledFinalFlushMaxMs;
    }
    if (!shutdownSm_.active() || finalFrameReady(nowMs, oledWaitDeadlineMs_, true)) {
        esp_restart();
    } else if (!finalHoldDeadlineMs_) {
        if (const TaskHandle_t self = xTaskGetCurrentTaskHandle(); self != nullptr) {
            waitOledFinalFrame(self);
            if (finalFrameReady(rtos::nowMs(), oledWaitDeadlineMs_, true)) {
                esp_restart();
            }
        }
    }
    return false;
}

bool PowerModule::poll(uint32_t nowMs) noexcept
{
    if (!active_) {
        return false;
    }
    if (restartPending_) {
        return pollRestart(nowMs);
    }
    const bool cutSenseFell = shutdownSm_.stage() == board::ShutdownStage::kCutPower &&
                              lastSnapshot_.u3On &&
                              !readPinActive(board::current().powerHold.senseGpio, false);
    PinLevels pins = samplePins(nowMs, cutSenseFell);
    const auto event = keySm_.poll(nowMs, pins.keyPressed);
    switch (event) {
    case board::KeyEvent::kShort:
        ++shortPresses_;
        fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(PowerTrace::kKeyShort), 0U,
                  shortPresses_, keySm_.lastHoldMs());
        ctx().data.uiEvents.notifyShortKey(nowMs);
        break;
    case board::KeyEvent::kLongHold:
        ++longPresses_;
        fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(PowerTrace::kKeyLong), 0U, longPresses_,
                  static_cast<uint32_t>(ctx().bootMode));
        if (ctx().bootMode == board::BootMode::kNormal) {
            if (ctx().data.uiEvents.oledActive()) {
                ctx().data.uiEvents.notifyLongKey(nowMs);
            } else {
                static_cast<void>(power::requestShutdown(board::ShutdownReason::kKey));
            }
        }
        break;
    case board::KeyEvent::kNone:
    default:
        break;
    }
    if (!keySm_.armed() || !keySm_.pressed()) {
        forcePowerOffFired_ = false;
    } else if ((ctx().bootMode == board::BootMode::kNormal) && !forcePowerOffFired_ &&
               pins.keyPressed &&
               ((nowMs - keySm_.pressStartMs()) >= board::kForcePowerOffHoldMs)) {
        forcePowerOffFired_ = true;
        if (power::requestShutdown(board::ShutdownReason::kKey)) {
            FW_LOGW(fw::TraceCat::kSys, "force power-off fallback after %" PRIu32 " ms key hold",
                    nowMs - keySm_.pressStartMs());
        }
    }

    pollBattery(nowMs, pins);
    if (ctx().bootMode == board::BootMode::kChargeStandby) {
        handleChargeStandbyPowerOn(rtos::nowMs(), event == board::KeyEvent::kLongHold, pins.u3On);
    } else {
        stepShutdown(rtos::nowMs(), pins);
    }
    ctx().data.uiEvents.updateKeyHold(keySm_.holding(), keySm_.pressStartMs(), keySm_.longPressMs(),
                                      standbyWake_.active());
    publishSnapshot(pins, false);
    return false;
}

} // namespace app
