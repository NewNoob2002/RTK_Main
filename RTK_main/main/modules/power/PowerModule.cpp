// 电源按键与关机管理模块实现（kLink 执行器）。
// init 仅启动任务调用，可等待首组 ADC 样本；poll 仅 kLink 调用；requestShutdown 任意任务安全。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/power/PowerModule.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string_view>

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
#include "app/RadioMode.hpp"
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

constexpr uint32_t kQuiesceMinWaitMs = 30U;      // ms，给 trace_out 一次唤醒窗口
constexpr uint32_t kQuiesceMaxWaitMs = 150U;     // ms，日志排空等待上限
constexpr uint32_t kNetQuiesceMaxWaitMs = 2000U; // ms，4G 优雅关机总上限（ADR-0021 §2.9）
constexpr uint32_t kWifiQuiesceWaitMs = 5500U;   // ms，覆盖 HTTP socket 默认 5 s 超时
constexpr uint32_t kRestartWaitMs = 150U;        // ms，异步日志和 UART 排空上限
constexpr uint32_t kOledFinalFlushMaxMs = 30U;   // ms，USB 关机终帧 I2C 刷出等待上限（OL2）
constexpr uint32_t kChargerPollPeriodMs = 200U;  // ms，运行期 SGM41513 I2C 采样周期
constexpr uint32_t kBatteryPeriodMs = 1000U;     // ms，组间最短等待时间
constexpr uint32_t kBootBatterySpacingMs = 5U;   // ms，旧 P2 首组组内采样间隔
constexpr uint32_t kBatteryFreshMs = 2000U;      // ms，超期旧读数不用于低电关机

constinit std::atomic<TaskHandle_t> s_restart_task_handle{nullptr};
static_assert(fw::kAtomicUsable<TaskHandle_t>, "电源重启唤醒任务句柄原子须无锁");

// 低 8 位 = 模拟百分比，高 24 位 = 会话代号；两者同一原子字发布，读者不会看到撕裂组合。
constexpr uint32_t kSimPctMask = 0xFFU;
constexpr uint32_t kSimSessionShift = 8U;
constinit std::atomic<uint32_t> s_sim_battery_word{board::kBatteryPctUnknown};
static_assert(fw::kAtomicUsable<uint32_t>, "模拟电量原子须无锁");

constinit std::array<power::ShutdownParticipant, power::kMaxExtraShutdownParticipants>
    s_extra_participants{};
constinit size_t s_extra_participant_count = 0U;

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
           (lhs.shutdownDoneCount == rhs.shutdownDoneCount) &&
           (lhs.shutdownTotalCount == rhs.shutdownTotalCount) &&
           (lhs.shortPresses == rhs.shortPresses) && (lhs.longPresses == rhs.longPresses) &&
           (lhs.lastShutdownFault == rhs.lastShutdownFault) &&
           (lhs.batterySimSession == rhs.batterySimSession);
}

} // namespace

namespace power {

bool requestShutdown(board::ShutdownReason reason) noexcept
{
    const auto &app = appContext();
    return app.data.powerStatus.read().active &&
           s_power_requests.requestShutdown(app.bootMode, reason);
}

fw::Status registerShutdownParticipant(const ShutdownParticipant &entry) noexcept
{
    // kCutPower 由内部 OLED 双终帧有界流程独占：外部参与者不得延长断电窗口。
    // 名字含控制字符（CR/LF 等）会在日志中消失或换行，造成与内置名无法区分，一律拒绝。
    constexpr unsigned char kFirstPrintable = 0x20U; // 低于此值为 C0 控制字符
    constexpr unsigned char kDelChar = 0x7FU;
    const auto hasControlChar = [](std::string_view name) noexcept {
        return std::any_of(name.begin(), name.end(), [](char c) noexcept {
            const auto ch = static_cast<unsigned char>(c);
            return (ch < kFirstPrintable) || (ch == kDelChar);
        });
    };
    if ((entry.name == nullptr) || (*entry.name == '\0') || (entry.isDone == nullptr) ||
        ((entry.stage != board::ShutdownStage::kSave) &&
         (entry.stage != board::ShutdownStage::kQuiesce)) ||
        (std::strlen(entry.name) > kMaxShutdownParticipantNameLen) ||
        hasControlChar(std::string_view{entry.name}) ||
        PowerModule::isBuiltinParticipantName(entry.name)) {
        return fw::Status::kInvalidArg;
    }
    for (size_t i = 0U; i < s_extra_participant_count; ++i) {
        if ((s_extra_participants[i].name != nullptr) &&
            (std::strcmp(s_extra_participants[i].name, entry.name) == 0)) {
            return fw::Status::kBusy;
        }
    }
    if (s_extra_participant_count >= kMaxExtraShutdownParticipants) {
        return fw::Status::kNoSpace;
    }
    s_extra_participants[s_extra_participant_count] = entry;
    ++s_extra_participant_count;
    return fw::Status::kOk;
}

void clearExtraShutdownParticipants() noexcept
{
    s_extra_participants = {};
    s_extra_participant_count = 0U;
}

std::span<const ShutdownParticipant> extraShutdownParticipants() noexcept
{
    return {s_extra_participants.data(), s_extra_participant_count};
}

void setSimulatedBatteryPct(uint8_t pct) noexcept
{
    constexpr uint8_t kMaxBatteryPct = 100U;
    const uint32_t encoded = (pct <= kMaxBatteryPct) ? pct : board::kBatteryPctUnknown;
    uint32_t cur = s_sim_battery_word.load(std::memory_order_relaxed);
    uint32_t next = 0U;
    do {
        if ((encoded == board::kBatteryPctUnknown) &&
            ((cur & kSimPctMask) == board::kBatteryPctUnknown)) {
            // 模拟本已关闭：重复清除保持幂等，不开启新会话，避免重置真实低电计时。
            return;
        }
        // 其余调用（启用、改值、退出、重注入）均递增会话代号（24 位回绕），使「清除后立即重新注入」也可被消费者观察到。
        const uint32_t session = ((cur >> kSimSessionShift) + 1U) & 0xFFFFFFU;
        next = (session << kSimSessionShift) | encoded;
    } while (!s_sim_battery_word.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed));
}

SimBatterySession simulatedBatterySession() noexcept
{
    constexpr uint8_t kMaxBatteryPct = 100U;
    const uint32_t word = s_sim_battery_word.load(std::memory_order_acquire);
    const auto raw = static_cast<uint8_t>(word & kSimPctMask);
    return SimBatterySession{(raw <= kMaxBatteryPct) ? raw : board::kBatteryPctUnknown,
                             word >> kSimSessionShift};
}

uint8_t simulatedBatteryPct() noexcept
{
    return simulatedBatterySession().pct;
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
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
    simSession_ = power::simulatedBatterySession().session;
    simActive_ = false;
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
        // 无充电 IC 的板（F9P 板）STAT GPIO2 悬空可读成有效电平；无 USB 输入时不可能充电。
        .charging = stat.value_or(false) && usb.present,
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

bool PowerModule::pollBatteryAdc(uint32_t nowMs, bool &fresh) noexcept
{
    if (!batteryAdcReady_) {
        return false;
    }
    // 在本轮采样前检查间隔，避免将轮询中断跨过的时间计入连续低电。
    fresh = nowMs - lastBatterySampleMs_ < kBatteryFreshMs;
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
    return true;
}

void PowerModule::pollBattery(uint32_t nowMs, const PinLevels &pins) noexcept
{
    const auto sim = power::simulatedBatterySession();
    if (sim.session != simSession_) {
        // 模拟会话变化（含「清除后立即重新注入」）：作废已累计的低电计时，避免两轮注入被合并。
        simSession_ = sim.session;
        lowBattery_ = {};
        if (simActive_) {
            // 模拟读数不得带入真实 ADC 路径：丢弃并让真实采样重新开始。
            simActive_ = false;
            batteryPct_ = board::kBatteryPctUnknown;
            batteryMv_ = 0U;
            batterySampleCount_ = 0U;
            lastBatteryBatchMs_ = nowMs - kBatteryPeriodMs;
            lastBatterySampleMs_ = nowMs - kBatteryFreshMs;
        }
    }
    bool fresh = false;
    if (sim.pct != board::kBatteryPctUnknown) {
        simActive_ = true;
        batteryPct_ = sim.pct;
        batteryMv_ = 0U;
        lastBatterySampleMs_ = nowMs;
        fresh = true;
    } else if (!pollBatteryAdc(nowMs, fresh)) {
        return;
    }
    const bool onBattery = pins.usbKnown && !pins.usbPresent;
    if (ctx().bootMode != board::BootMode::kNormal ||
        !power::s_power_requests.canScheduleRestart()) {
        lowBattery_ = {};
        return;
    }
    constexpr uint32_t kHoldMs =
#if defined(CONFIG_RTK_LOW_BATTERY_HOLD_MS)
        static_cast<uint32_t>(CONFIG_RTK_LOW_BATTERY_HOLD_MS);
#else
        board::kLowBatteryHoldMs;
#endif
    if (lowBattery_.isDue(nowMs, onBattery && fresh, batteryPct_, kHoldMs) &&
        power::requestShutdown(board::ShutdownReason::kLowBattery)) {
        FW_LOGW(fw::TraceCat::kSys, "<battery> low battery shutdown at %" PRIu32 " mV (%u%%)",
                batteryMv_, static_cast<unsigned>(batteryPct_));
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

struct PowerModule::ParticipantOps {
    /**
     * @brief 轮询 kLink（storage）保存围栏是否到达
     * @param userCtx: PowerModule 实例指针
     * @retval true 围栏已到达
     * @retval false 等待设置提交或围栏执行
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool storageFenceDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        return static_cast<PowerModule *>(userCtx)->pollSaveFence(true);
    }

    /**
     * @brief 轮询 kIo（GNSS）冲刷围栏是否到达
     * @param userCtx: PowerModule 实例指针
     * @retval true 围栏已到达
     * @retval false 等待设置提交或围栏执行
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool gnssFenceDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        return static_cast<PowerModule *>(userCtx)->pollSaveFence(false);
    }

    /**
     * @brief 开启断电保存窗口并加速延迟持久化队列落盘
     * @param stage: 当前阶段（kSave）
     * @param userCtx: PowerModule 实例指针
     * @note 仅 kLink 调用，非 ISR。
     */
    static void storageFlushBegin(board::ShutdownStage stage, uint32_t /*deadlineMs*/,
                                  void *userCtx) noexcept
    {
        static_cast<void>(stage);
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        auto *self = static_cast<PowerModule *>(userCtx);
        store::startPowerSave(self->shutdownNowMs_);
        self->ctx().storage.expeditePendingWrites();
    }

    /**
     * @brief 检查延迟存储队列与在途 NVS 写入是否全部清空
     * @param userCtx: PowerModule 实例指针
     * @retval true 无待写项且无在途 NVS 写入
     * @retval false 仍有数据正在落盘
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool storageFlushDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        const auto *self = static_cast<const PowerModule *>(userCtx);
        return !self->context_->storage.hasPendingWrites() && (store::inFlightWrites() == 0U);
    }

    /**
     * @brief 检查蓝牙链路是否存在未提交设置
     * @param userCtx: PowerModule 实例指针
     * @retval true 无待提交设置或当前不在蓝牙常规模式
     * @retval false 仍有待提交蓝牙设置
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool btSettingsDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        const auto *self = static_cast<const PowerModule *>(userCtx);
        return (self->context_->bootMode != board::BootMode::kNormal) ||
               (self->context_->radioMode != RadioMode::kBluetooth) ||
               !self->context_->bt_link.hasPendingSettings();
    }

    /**
     * @brief 检查配网 Web 模块是否存在未提交设置
     * @param userCtx: PowerModule 实例指针
     * @retval true 无待提交设置或配网未启用/非配网模式
     * @retval false 仍有待提交 Web 设置
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool webSettingsDone(void *userCtx) noexcept
    {
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        const auto *self = static_cast<const PowerModule *>(userCtx);
        return (self->context_->bootMode != board::BootMode::kNormal) ||
               (self->context_->radioMode != RadioMode::kWifiConfig) ||
               !self->context_->web.hasPendingSettings();
#else
        static_cast<void>(userCtx);
        return true;
#endif
    }

    /**
     * @brief 检查 GNSS 模块是否仍有挂起 NVS 保存请求
     * @param userCtx: PowerModule 实例指针
     * @retval true 无挂起 NVS 保存或 GNSS 未启用
     * @retval false 仍有 GNSS NVS 保存待完成
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool gnssNvsDone(void *userCtx) noexcept
    {
#if RTK_GNSS_ENABLED
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        const auto *self = static_cast<const PowerModule *>(userCtx);
        return (self->context_->bootMode != board::BootMode::kNormal) ||
               !self->context_->data.gnssPendingNvs.load(std::memory_order_acquire);
#else
        static_cast<void>(userCtx);
        return true;
#endif
    }

    /**
     * @brief 请求当前启用的无线业务（BT/Wi-Fi）进入关机暂停态
     * @param stage: 当前阶段（kQuiesce）
     * @param userCtx: PowerModule 实例指针
     * @note 仅 kLink 调用，非 ISR。
     */
    static void radioPauseBegin(board::ShutdownStage stage, uint32_t /*deadlineMs*/,
                                void *userCtx) noexcept
    {
        static_cast<void>(stage);
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        auto *self = static_cast<PowerModule *>(userCtx);
        if (self->ctx().bootMode != board::BootMode::kNormal) {
            return;
        }
        if (self->ctx().radioMode == RadioMode::kBluetooth) {
            self->ctx().bt_link.setPowerPaused(true);
        }
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
        if (self->ctx().radioMode == RadioMode::kWifiConfig) {
            self->ctx().web.setPowerPaused(true);
        }
#endif
    }

    /**
     * @brief 检查无线业务是否已完成关机静默
     * @param userCtx: PowerModule 实例指针
     * @retval true 蓝牙/Wi-Fi 已暂停或无需静默
     * @retval false 仍在等待无线断开/停用
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool radioPauseDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        return static_cast<const PowerModule *>(userCtx)->isRadioQuiesced();
    }

    /**
     * @brief 将板级外设输出恢复到安全电平并记录结果
     * @param stage: 当前阶段（kQuiesce）
     * @param userCtx: PowerModule 实例指针
     * @note 仅 kLink 调用，非 ISR。
     */
    static void safeGpioBegin(board::ShutdownStage stage, uint32_t /*deadlineMs*/,
                              void *userCtx) noexcept
    {
        static_cast<void>(stage);
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        auto *self = static_cast<PowerModule *>(userCtx);
        const fw::Status safeStatus = board::setSafeOutputLevels(board::current());
        self->safeGpioEvaluated_ = true;
        self->safeGpioOk_ = fw::isOk(safeStatus);
        if (!self->safeGpioOk_) {
            FW_LOGW(fw::TraceCat::kSys, "power quiesce safe outputs failed (%s)",
                    fw::toString(safeStatus));
        }
        self->keyDriveHigh_ = false;
    }

    /**
     * @brief 检查板级外设安全电平配置是否成功
     * @param userCtx: PowerModule 实例指针
     * @retval true 已成功置为安全电平
     * @retval false 未执行或硬件配置失败
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool safeGpioDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        return static_cast<const PowerModule *>(userCtx)->safeGpioOk_;
    }

    /**
     * @brief 判断参与者是否为内置 log_drain
     * @param p: 参与者表项
     * @retval true 是 log_drain（其 begin/截止延后到其它 kQuiesce 参与者收尾之后）
     * @retval false 其它参与者
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool isLogDrain(const power::ShutdownParticipant &p) noexcept
    {
        return p.isDone == &logDrainDone;
    }

    /**
     * @brief 打印关机完成日志以启动日志排空等待
     * @param stage: 当前阶段（kQuiesce）
     * @param userCtx: 未使用
     * @note 仅 kLink 调用，非 ISR；由 pollStageParticipants 在其它 kQuiesce 参与者收尾后调用，
     *       使 "device powered down" 晚于 4G 断电等收尾日志。
     */
    static void logDrainBegin(board::ShutdownStage stage, uint32_t /*deadlineMs*/,
                              void *userCtx) noexcept
    {
        static_cast<void>(stage);
        static_cast<void>(userCtx);
        FW_LOGI(fw::TraceCat::kSys, "device powered down");
    }

    /**
     * @brief 检查异步日志输出任务与控制台 UART 是否已排空
     * @param userCtx: PowerModule 实例指针
     * @retval true 日志与 UART 均已排空
     * @retval false 仍有日志等待输出
     * @note 仅 kLink 调用，非 ISR；排空计时自 logDrainStartMs_ 起算（未开始前不会被调用）。
     */
    static bool logDrainDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        const auto *self = static_cast<const PowerModule *>(userCtx);
        return self->isLogDrained(self->shutdownNowMs_ - self->logDrainStartMs_);
    }

    /**
     * @brief 复位电池路径 OLED 终帧停留状态
     * @param stage: 当前阶段（kCutPower）
     * @param userCtx: PowerModule 实例指针
     * @note 仅 kLink 调用，非 ISR。
     */
    static void oledFinalBatBegin(board::ShutdownStage stage, uint32_t /*deadlineMs*/,
                                  void *userCtx) noexcept
    {
        static_cast<void>(stage);
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        static_cast<PowerModule *>(userCtx)->batteryFinalFrame_ = {};
    }

    /**
     * @brief 检查电池路径 OLED 终帧是否刷出并停留满足时长
     * @param userCtx: PowerModule 实例指针
     * @retval true 可放行断电（或当前处于 USB 重启路径）
     * @retval false 仍在等待电池终帧刷出或停留
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool oledFinalBatDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        auto *self = static_cast<PowerModule *>(userCtx);
        if (self->restartPending_) {
            return true;
        }
        return self->finalFrameReady(self->shutdownNowMs_,
                                     self->cutStartMs_ + board::kShutdownDisplayGraceMs, false);
    }

    /**
     * @brief 复位 USB 路径 OLED 终帧停留状态
     * @param stage: 当前阶段（kCutPower）
     * @param userCtx: PowerModule 实例指针
     * @note 仅 kLink 调用，非 ISR。
     */
    static void oledFinalUsbBegin(board::ShutdownStage stage, uint32_t /*deadlineMs*/,
                                  void *userCtx) noexcept
    {
        static_cast<void>(stage);
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        static_cast<PowerModule *>(userCtx)->usbFinalFrame_ = {};
    }

    /**
     * @brief 检查 USB 重启路径 OLED 终帧是否刷出并停留满足时长
     * @param userCtx: PowerModule 实例指针
     * @retval true 可放行重启（或尚未进入重启路径）
     * @retval false 仍在等待 USB RESTARTING 终帧刷出或停留
     * @note 仅 kLink 调用，非 ISR。
     */
    static bool oledFinalUsbDone(void *userCtx) noexcept
    {
        // 内置参与者表仅由 PowerModule 传入 this，恢复指针安全。
        auto *self = static_cast<PowerModule *>(userCtx);
        if (!self->restartPending_) {
            return true;
        }
        return self->finalFrameReady(self->shutdownNowMs_, self->oledWaitDeadlineMs_, true);
    }

    static constexpr std::array<power::ShutdownParticipant, 11U> kBuiltin{{
        {"storage_fence", nullptr, &storageFenceDone, nullptr, board::ShutdownStage::kSave,
         power::ShutdownMilestone::kSaveFence},
        {"gnss_fence", nullptr, &gnssFenceDone, nullptr, board::ShutdownStage::kSave,
         power::ShutdownMilestone::kSaveFence},
        {"storage_flush", &storageFlushBegin, &storageFlushDone, nullptr,
         board::ShutdownStage::kSave, power::ShutdownMilestone::kSaveFlush},
        {"bt_settings", nullptr, &btSettingsDone, nullptr, board::ShutdownStage::kSave,
         power::ShutdownMilestone::kSaveFlush},
        {"web_settings", nullptr, &webSettingsDone, nullptr, board::ShutdownStage::kSave,
         power::ShutdownMilestone::kSaveFlush},
        {"gnss_nvs", nullptr, &gnssNvsDone, nullptr, board::ShutdownStage::kSave,
         power::ShutdownMilestone::kSaveFlush},
        {"radio_pause", &radioPauseBegin, &radioPauseDone, nullptr, board::ShutdownStage::kQuiesce,
         power::ShutdownMilestone::kRadioQuiesce},
        {"safe_gpio", &safeGpioBegin, &safeGpioDone, nullptr, board::ShutdownStage::kQuiesce,
         power::ShutdownMilestone::kSafeGpio},
        {"log_drain", &logDrainBegin, &logDrainDone, nullptr, board::ShutdownStage::kQuiesce,
         power::ShutdownMilestone::kNone},
        {"oled_final_bat", &oledFinalBatBegin, &oledFinalBatDone, nullptr,
         board::ShutdownStage::kCutPower, power::ShutdownMilestone::kOledFinal},
        {"oled_final_usb", &oledFinalUsbBegin, &oledFinalUsbDone, nullptr,
         board::ShutdownStage::kCutPower, power::ShutdownMilestone::kOledFinal},
    }};
    static_assert(kBuiltin.size() + power::kMaxExtraShutdownParticipants <= 16U,
                  "关机参与者总数须适配 16 位状态掩码");
};

bool PowerModule::isBuiltinParticipantName(const char *name) noexcept
{
    if (name == nullptr) {
        return false;
    }
    return std::any_of(ParticipantOps::kBuiltin.begin(), ParticipantOps::kBuiltin.end(),
                       [name](const power::ShutdownParticipant &p) noexcept {
                           return std::strcmp(p.name, name) == 0;
                       });
}

bool PowerModule::pollSaveFence(bool link) noexcept
{
    if (!power::s_power_requests.settingsSubmitted()) {
        return false;
    }
    if (link) {
        if (!linkFenceReached_) {
            linkFenceReached_ = reachedSaveFence(ctx().linkExecutor, ModuleKind::kStorage,
                                                 StorageModule::kSaveFence, linkFence_);
        }
        return linkFenceReached_;
    }
    if (!ioFenceReached_) {
        // 在已获准的 kIo 设置命令之后冲刷，即使该命令是在关门时才入队。
        ioFenceReached_ =
            reachedSaveFence(ctx().ioExecutor, ModuleKind::kGnss,
                             static_cast<uint8_t>(GnssSubVerb::kFlushGgaGate), ioFence_);
    }
    return ioFenceReached_;
}

uint32_t PowerModule::quiesceLimitMs(const power::ShutdownParticipant &p,
                                     bool builtin) const noexcept
{
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    if ((p.milestone == power::ShutdownMilestone::kRadioQuiesce) &&
        (context_->radioMode == RadioMode::kWifiConfig)) {
        return kWifiQuiesceWaitMs;
    }
#endif
    if (builtin && (p.milestone == power::ShutdownMilestone::kSafeGpio) && safeGpioEvaluated_) {
        // 仅内置 safe_gpio 是同步执行：失败时不再额外空等；外部参与者即使声明同里程碑也按通用上限等待。
        return 0U;
    }
    if (p.milestone == power::ShutdownMilestone::kNetPower) {
        return kNetQuiesceMaxWaitMs;
    }
    return kQuiesceMaxWaitMs;
}

void PowerModule::beginStageParticipants(board::ShutdownStage stage) noexcept
{
    size_t idx = 0U;
    const auto invoke = [this, stage, &idx](const power::ShutdownParticipant &p, void *cbCtx,
                                            bool builtin) noexcept {
        if (p.stage == stage) {
            // log_drain 的 begin（打印 "device powered down"）延后到 pollStageParticipants：
            // 须等其它 kQuiesce 参与者（如 4G 优雅关机）收尾后再启动，避免过早起算 150 ms 上限。
            if ((p.begin != nullptr) && !(builtin && ParticipantOps::isLogDrain(p))) {
                // kQuiesce 的绝对截止时刻 = PW1 起点 + 该参与者上限；参与者与 PW1 共用同一时刻（0=未指定）。
                const uint32_t deadline = (stage == board::ShutdownStage::kQuiesce)
                                              ? (quiesceStartMs_ + quiesceLimitMs(p, builtin))
                                              : 0U;
                p.begin(stage, deadline, cbCtx);
            }
            // safe_gpio 失败位只绑定内置表项；外部同里程碑参与者靠自己的 isDone/超时判定。
            if (builtin && (p.milestone == power::ShutdownMilestone::kSafeGpio) &&
                safeGpioEvaluated_ && !safeGpioOk_) {
                // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
                participantTimedOutMask_ =
                    static_cast<uint16_t>(participantTimedOutMask_ | (1U << idx));
            }
        }
        ++idx;
    };
    for (const auto &p : ParticipantOps::kBuiltin) {
        invoke(p, this, true);
    }
    for (const auto &p : power::extraShutdownParticipants()) {
        invoke(p, p.userCtx, false);
    }
}

/**
 * @brief 轮询内置 log_drain：在 kNetPower 参与者收尾后才开始，并自开始时刻起算排空上限
 * @param p: log_drain 参与者表项
 * @param idx: 其在完成位掩码中的序号
 * @param longSeen: kQuiesce 是否存在 kNetPower 参与者
 * @param longPending: kNetPower 参与者是否仍在等待（未完成且未到限）
 * @param stage: 当前阶段（kQuiesce）
 * @param nowMs: 当前时刻（ms）
 * @retval true log_drain 已完成或已到排空上限（不再阻塞静默阶段）
 * @retval false 尚未开始或仍在排空
 * @note 仅 kLink 调用，非 ISR；无 kNetPower 参与者时起点取 quiesceStartMs_，与旧行为一致。
 */
bool PowerModule::pollLogDrain(const power::ShutdownParticipant &p, size_t idx, bool longSeen,
                               bool longPending, board::ShutdownStage stage,
                               uint32_t nowMs) noexcept
{
    // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
    const auto bit = static_cast<uint16_t>(1U << idx);
    if (!logDrainStarted_ && !longPending) {
        logDrainStarted_ = true;
        logDrainStartMs_ = longSeen ? nowMs : quiesceStartMs_;
        if (p.begin != nullptr) {
            p.begin(stage, logDrainStartMs_ + quiesceLimitMs(p, true), this);
        }
    }
    if (!logDrainStarted_) {
        // 未开始前不视为超时（recordStageTimeouts 只在 allSatisfied 后调用），但阻塞放行。
        participantDoneMask_ = static_cast<uint16_t>(participantDoneMask_ & ~bit);
        return false;
    }
    if (p.isDone(this)) {
        participantDoneMask_ = static_cast<uint16_t>(participantDoneMask_ | bit);
        return true;
    }
    participantDoneMask_ = static_cast<uint16_t>(participantDoneMask_ & ~bit);
    return (nowMs - logDrainStartMs_) >= quiesceLimitMs(p, true);
}

/**
 * @brief 判定单个非 log_drain 参与者并更新完成位与汇总状态
 * @param p: 参与者表项（其 stage 已等于当前阶段）
 * @param cbCtx: 传给 isDone/onTimeout 的上下文
 * @param builtin: 是否内置参与者
 * @param stage: 当前阶段
 * @param quiesceElapsed: 自 kQuiesce 起点的耗时（ms）
 * @param idx: 该参与者在完成位掩码中的序号
 * @param st: 轮询汇总状态（allSatisfied 与 kNetPower 跟踪）
 * @note 仅 kLink 调用，非 ISR；超时兜底回调在此到点即调用。
 */
void PowerModule::pollOneParticipant(const power::ShutdownParticipant &p, void *cbCtx, bool builtin,
                                     board::ShutdownStage stage, uint32_t quiesceElapsed,
                                     size_t idx, PollState &st) noexcept
{
    // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
    const auto bit = static_cast<uint16_t>(1U << idx);
    const bool isLong = (stage == board::ShutdownStage::kQuiesce) &&
                        (p.milestone == power::ShutdownMilestone::kNetPower);
    st.longSeen = st.longSeen || isLong;
    if ((p.isDone == nullptr) || p.isDone(cbCtx)) {
        participantDoneMask_ = static_cast<uint16_t>(participantDoneMask_ | bit);
        return;
    }
    participantDoneMask_ = static_cast<uint16_t>(participantDoneMask_ & ~bit);
    if (stage != board::ShutdownStage::kQuiesce) {
        st.allSatisfied = false;
    } else if (quiesceElapsed < quiesceLimitMs(p, builtin)) {
        st.allSatisfied = false;
        st.longPending = st.longPending || isLong;
    } else if (!builtin && (p.onTimeout != nullptr)) {
        // 到点未完成：立即兜底（不等 OLED 可见时间/其它参与者）；失败则每轮重试到 isDone。
        p.onTimeout(cbCtx);
    }
}

bool PowerModule::pollStageParticipants(board::ShutdownStage stage, uint32_t nowMs) noexcept
{
    shutdownNowMs_ = nowMs;
    const uint32_t quiesceElapsed = nowMs - quiesceStartMs_;
    PollState st{};
    size_t idx = 0U;
    // log_drain 延后处理：记录其表项与位序号，待其它参与者判定完再决定是否开始。
    // 4G 里程碑（kNetPower，上限 2000 ms，完成时才打断电日志）之外的参与者与 log_drain 同窗口，
    // 不必串行等待，避免无 4G 时静默阶段被拉长。
    const power::ShutdownParticipant *logDrain = nullptr;
    size_t logDrainIdx = 0U;
    const auto check = [&](const power::ShutdownParticipant &p, void *cbCtx,
                           bool builtin) noexcept {
        if (p.stage == stage) {
            if ((stage == board::ShutdownStage::kQuiesce) && builtin &&
                ParticipantOps::isLogDrain(p)) {
                logDrain = &p;
                logDrainIdx = idx;
            } else {
                pollOneParticipant(p, cbCtx, builtin, stage, quiesceElapsed, idx, st);
            }
        }
        ++idx;
    };
    for (const auto &p : ParticipantOps::kBuiltin) {
        check(p, this, true);
    }
    for (const auto &p : power::extraShutdownParticipants()) {
        check(p, p.userCtx, false);
    }
    if (logDrain != nullptr) {
        const bool satisfied =
            pollLogDrain(*logDrain, logDrainIdx, st.longSeen, st.longPending, stage, nowMs);
        st.allSatisfied = st.allSatisfied && satisfied;
    }
    return st.allSatisfied;
}

void PowerModule::retryQuiesceFallbacks() noexcept
{
    // kQuiesce 兜底（如 4G 硬切 PWREN）首次失败后 PW1 已进入 kCutPower：每轮继续重试直到参与者 isDone。
    // 独立于 OLED 终帧门控（无 OLED/USB 在位时 cutReady 不会轮询 kCutPower 参与者）；总窗口不延长（r4 R5）。
    for (const auto &p : power::extraShutdownParticipants()) {
        if ((p.stage == board::ShutdownStage::kQuiesce) && (p.onTimeout != nullptr) &&
            (p.isDone != nullptr) && !p.isDone(p.userCtx)) {
            p.onTimeout(p.userCtx);
        }
    }
}

void PowerModule::recordStageTimeouts(board::ShutdownStage stage) noexcept
{
    size_t idx = 0U;
    const char *stageFault = nullptr;
    const auto mark = [this, stage, &idx,
                       &stageFault](const power::ShutdownParticipant &p) noexcept {
        if (p.stage == stage) {
            // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
            const auto bit = static_cast<uint16_t>(1U << idx);
            if ((participantDoneMask_ & bit) == 0U) {
                participantTimedOutMask_ = static_cast<uint16_t>(participantTimedOutMask_ | bit);
            }
            if (stageFault == nullptr && (participantTimedOutMask_ & bit) != 0U) {
                stageFault = p.name;
            }
        }
        ++idx;
    };
    for (const auto &p : ParticipantOps::kBuiltin) {
        mark(p);
    }
    for (const auto &p : power::extraShutdownParticipants()) {
        const auto bit = static_cast<uint16_t>(1U << (idx));
        const bool timedOut = (p.stage == stage) && ((participantDoneMask_ & bit) == 0U);
        mark(p);
        // kQuiesce 的兜底已在 pollStageParticipants 到点时执行（并按轮重试），此处不重复调用。
        if (timedOut && (p.onTimeout != nullptr) && (stage != board::ShutdownStage::kQuiesce)) {
            p.onTimeout(p.userCtx); // 兜底动作（如 4G 硬切 PWREN）：有界、不阻塞
        }
    }
    if (stageFault != nullptr) {
        lastShutdownFault_ = stageFault;
    }
}

void PowerModule::logStageTimeout(board::ShutdownStage stage) const noexcept
{
    if (stage == board::ShutdownStage::kSave) {
        constexpr size_t kPendingSummaryBytes = 96U; // B，待落盘项名摘要缓冲
        std::array<char, kPendingSummaryBytes> storageItems{};
        context_->storage.formatPendingSummary(storageItems);
        const uint32_t inflight = store::inFlightWrites();
        FW_LOGW(fw::TraceCat::kSys,
                "power save timeout after %" PRIu32 " ms; storage=%s nvs_inflight=%" PRIu32,
                board::kShutdownSaveTimeoutMs, storageItems.data(), inflight);
    }
    size_t idx = 0U;
    const char *const stageText = stage == board::ShutdownStage::kSave ? "save" : "quiesce";
    const auto report = [this, stage, stageText,
                         &idx](const power::ShutdownParticipant &p) noexcept {
        if (p.stage == stage) {
            // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
            const auto bit = static_cast<uint16_t>(1U << idx);
            if ((participantTimedOutMask_ & bit) != 0U) {
                // 每个超时参与者独占一行并完整打印名字（名字 ≤ 32 字符，不会被日志行截断）。
                FW_LOGW(fw::TraceCat::kSys, "power %s timeout pending=%s", stageText, p.name);
            }
        }
        ++idx;
    };
    for (const auto &p : ParticipantOps::kBuiltin) {
        report(p);
    }
    for (const auto &p : power::extraShutdownParticipants()) {
        report(p);
    }
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
    logDrainStartMs_ = nowMs;
    logDrainStarted_ = false;
    shutdownNowMs_ = nowMs;
    beginStageParticipants(board::ShutdownStage::kQuiesce);
    static_cast<void>(pollStageParticipants(board::ShutdownStage::kQuiesce, nowMs));
}

void PowerModule::handleShutdownStageEntry(uint32_t nowMs, const PinLevels &pins,
                                           const board::ShutdownOutput &out) noexcept
{
    stageEnteredMs_ = nowMs;
    stageOnScreenMs_.reset();
    shutdownNowMs_ = nowMs;
    fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(PowerTrace::kShutdownStage), 0U,
              static_cast<uint32_t>(out.stage), static_cast<uint32_t>(shutdownSm_.reason()));
    switch (out.stage) {
    case board::ShutdownStage::kAnnounce:
        FW_LOGI(fw::TraceCat::kSys, "power off reason=%s usb=%u",
                board::shutdownReasonName(shutdownSm_.reason()), pins.usbPresent ? 1U : 0U);
        linkFenceReached_ = false;
        ioFenceReached_ = false;
        resetShutdownProgress();
        lastShutdownFault_ = nullptr;
        ctx().data.uiEvents.resetShutdownFinalFrame();
        break;
    case board::ShutdownStage::kSave:
        beginStageParticipants(board::ShutdownStage::kSave);
        break;
    case board::ShutdownStage::kQuiesce:
        if (out.saveTimedOut) {
            saveStageTimedOut_ = true;
            recordStageTimeouts(board::ShutdownStage::kSave);
            logStageTimeout(board::ShutdownStage::kSave);
        }
        enterQuiesce(nowMs);
        break;
    case board::ShutdownStage::kCutPower:
        cutStartMs_ = nowMs;
        beginStageParticipants(board::ShutdownStage::kCutPower);
        break;
    case board::ShutdownStage::kIdle:
    default:
        break;
    }
}

void PowerModule::handleShutdownCutPower(uint32_t nowMs, const board::ShutdownOutput &out) noexcept
{
    if (out.cutTimedOut) {
        if (lastShutdownFault_ == nullptr) {
            lastShutdownFault_ = "cut_power";
        }
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
        usbFinalFrame_ = {};
    }
}

void PowerModule::stepShutdown(uint32_t nowMs, PinLevels &pins) noexcept
{
    if (!shutdownSm_.active() && !shutdownSm_.request(power::s_power_requests.shutdownReason())) {
        return;
    }

    shutdownNowMs_ = nowMs;
    const auto stage = shutdownSm_.stage();
    if (stage == board::ShutdownStage::kCutPower) {
        retryQuiesceFallbacks();
    }
    const bool saveDone = (stage == board::ShutdownStage::kSave) &&
                          pollStageParticipants(board::ShutdownStage::kSave, nowMs);
    const bool stageVisible = shutdownStageVisible(nowMs, stage);
    const bool quiesceReady = (stage == board::ShutdownStage::kQuiesce) &&
                              pollStageParticipants(board::ShutdownStage::kQuiesce, nowMs);
    const bool quiesceDone =
        (stage == board::ShutdownStage::kQuiesce) && stageVisible && quiesceReady;
    if (quiesceDone) {
        recordStageTimeouts(board::ShutdownStage::kQuiesce);
        logStageTimeout(board::ShutdownStage::kQuiesce);
        pins = samplePins(nowMs, true);
        // 刷新 REG08 与 UART 排空均在 GPIO4 首沿开始计时之前。
        nowMs = rtos::nowMs();
        shutdownNowMs_ = nowMs;
    }
    const auto usb =
        pins.u3On || pins.usbKnown ? std::optional<bool>{pins.usbPresent} : std::nullopt;
    const bool cutReady =
        !ctx().data.uiEvents.oledActive() ||
        ((stage == board::ShutdownStage::kCutPower) &&
         (pins.usbPresent ? stageVisible
                          : pollStageParticipants(board::ShutdownStage::kCutPower, nowMs)));
    const auto out =
        shutdownSm_.step(nowMs, saveDone, quiesceDone, pins.u3On, usb, stageVisible, cutReady);
    if (out.stageEntered) {
        handleShutdownStageEntry(nowMs, pins, out);
    }
    handleShutdownCutPower(nowMs, out);
}

void PowerModule::resetShutdownProgress() noexcept
{
    batteryFinalFrame_ = {};
    usbFinalFrame_ = {};
    participantDoneMask_ = 0U;
    participantTimedOutMask_ = 0U;
    safeGpioOk_ = false;
    safeGpioEvaluated_ = false;
    saveStageTimedOut_ = false;
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

PowerModule::ParticipantCount PowerModule::computeShutdownCounts() const noexcept
{
    if (shutdownSm_.stage() == board::ShutdownStage::kIdle) {
        return {};
    }
    ParticipantCount counts{};
    size_t idx = 0U;
    const auto tally = [this, &counts, &idx](const power::ShutdownParticipant &p) noexcept {
        if ((p.stage == board::ShutdownStage::kSave) ||
            (p.stage == board::ShutdownStage::kQuiesce)) {
            ++counts.total;
            // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
            const auto bit = static_cast<uint16_t>(1U << idx);
            if ((participantDoneMask_ & bit) != 0U) {
                ++counts.done;
            }
        }
        ++idx;
    };
    for (const auto &p : ParticipantOps::kBuiltin) {
        tally(p);
    }
    for (const auto &p : power::extraShutdownParticipants()) {
        tally(p);
    }
    return counts;
}

bool PowerModule::allParticipantsDone(board::ShutdownStage stage,
                                      power::ShutdownMilestone m) const noexcept
{
    bool found = false;
    bool allDone = true;
    size_t idx = 0U;
    const auto inspect = [this, stage, m, &found, &allDone,
                          &idx](const power::ShutdownParticipant &p) noexcept {
        if ((p.stage == stage) && ((m == power::ShutdownMilestone::kNone) || (p.milestone == m))) {
            found = true;
            // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
            const bool done = (participantDoneMask_ & static_cast<uint16_t>(1U << idx)) != 0U;
            allDone = allDone && done;
        }
        ++idx;
    };
    for (const auto &p : ParticipantOps::kBuiltin) {
        inspect(p);
    }
    for (const auto &p : power::extraShutdownParticipants()) {
        inspect(p);
    }
    return found && allDone;
}

uint8_t PowerModule::computeShutdownDoneMask(const PinLevels &pins) const noexcept
{
    const auto stage = shutdownSm_.stage();
    if (stage == board::ShutdownStage::kIdle) {
        return 0U;
    }
    uint8_t mask = board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced);
    if (allParticipantsDone(board::ShutdownStage::kSave, power::ShutdownMilestone::kSaveFence)) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFences);
    }
    if (allParticipantsDone(board::ShutdownStage::kSave, power::ShutdownMilestone::kNone)) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed);
    }
    if (allParticipantsDone(board::ShutdownStage::kQuiesce, power::ShutdownMilestone::kSafeGpio)) {
        mask |= board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe);
    }
    if (allParticipantsDone(board::ShutdownStage::kQuiesce,
                            power::ShutdownMilestone::kRadioQuiesce)) {
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
    uint8_t mask = saveStageTimedOut_
                       ? board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout)
                       : 0U;
    size_t idx = 0U;
    const auto inspect = [this, &mask, &idx](const power::ShutdownParticipant &p) noexcept {
        // 静态断言已限制参与者总数 <= 16，位掩码窄化安全。
        const bool failed = (participantTimedOutMask_ & static_cast<uint16_t>(1U << idx)) != 0U;
        if (failed) {
            if (p.stage == board::ShutdownStage::kSave) {
                mask |= board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout);
            } else if (p.milestone == power::ShutdownMilestone::kSafeGpio) {
                mask |= board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed);
            } else if (p.milestone == power::ShutdownMilestone::kRadioQuiesce) {
                mask |= board::shutdownDegradedBit(board::ShutdownDegradedBit::kRadioTimeout);
            }
        }
        ++idx;
    };
    for (const auto &p : ParticipantOps::kBuiltin) {
        inspect(p);
    }
    for (const auto &p : power::extraShutdownParticipants()) {
        inspect(p);
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
    const ParticipantCount counts = computeShutdownCounts();
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
        .shutdownDoneCount = counts.done,
        .shutdownTotalCount = counts.total,
        .shortPresses = shortPresses_,
        .longPresses = longPresses_,
        .lastShutdownFault = lastShutdownFault_,
        .batterySimSession = simSession_,
    };
    if (!force && (next.usbPresent != lastSnapshot_.usbPresent ||
                   next.usbSource != lastSnapshot_.usbSource)) {
        FW_LOGI(fw::TraceCat::kSys, "<power> usb=%u src=%s", next.usbPresent ? 1U : 0U,
                board::usbSourceName(next.usbSource));
    }
    const bool shutdownProgressChanged =
        !force && ((next.shutdownStage != lastSnapshot_.shutdownStage) ||
                   (next.shutdownDoneMask != lastSnapshot_.shutdownDoneMask) ||
                   (next.shutdownDegradedMask != lastSnapshot_.shutdownDegradedMask) ||
                   (next.shutdownDoneCount != lastSnapshot_.shutdownDoneCount) ||
                   (next.shutdownTotalCount != lastSnapshot_.shutdownTotalCount));
    const bool powerSupplyChanged = !force && ((next.usbPresent != lastSnapshot_.usbPresent) ||
                                               (next.charging != lastSnapshot_.charging));
    if (force || !snapshotEqual(next, lastSnapshot_)) {
        lastSnapshot_ = next;
        ctx().data.powerStatus.write(next);
        // 必须在 powerStatus 写入完成后再唤醒 UI，避免多核/抢占下 OLED 读到旧关机/供电状态（ROL1-03）。
        if (shutdownProgressChanged || powerSupplyChanged) {
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
    FinalFrameState &state = usb ? usbFinalFrame_ : batteryFinalFrame_;
    const auto &events = ctx().data.uiEvents;
    if (state.ready || !events.oledActive()) {
        return true;
    }
    if (!state.holdDeadlineMs && events.shutdownFinalFrameFlushed(usb)) {
        // 首次观察到终帧确认后才取单调时间：入参 nowMs 可能早于确认事件，直接使用会缩短 300 ms 停留。
        nowMs = rtos::nowMs();
        state.holdDeadlineMs = nowMs + board::kShutdownFinalVisibleMs;
    }
    const uint32_t deadline = state.holdDeadlineMs.value_or(flushDeadlineMs);
    // 两个等待窗口都远小于半个 uint32_t 周期，差值符号可用于回绕截止判断。
    state.ready = static_cast<int32_t>(nowMs - deadline) >= 0;
    if (state.ready && !state.holdDeadlineMs) {
        FW_LOGW(fw::TraceCat::kSys, "shutdown OLED final frame timed out");
    }
    return state.ready;
}

bool PowerModule::pollRestart(uint32_t nowMs) noexcept
{
    shutdownNowMs_ = nowMs;
    const uint32_t elapsed = nowMs - restartStartMs_;
    if (!restartLogReady_) {
        if (elapsed < kRestartWaitMs && !isLogDrained(elapsed)) {
            return false;
        }
        restartLogReady_ = true;
        oledWaitDeadlineMs_ = nowMs + kOledFinalFlushMaxMs;
    }
    const RebootBanner restartBanner =
        shutdownSm_.active() ? RebootBanner::kKeepFrame : RebootBanner::kStarting;
    const auto doRestart = [restartBanner]() noexcept {
#if defined(ESP_PLATFORM)
        restartWithOledQuiesce(restartBanner);
#else
        static_cast<void>(restartBanner);
        esp_restart();
#endif
    };
    // U3 已关断后沿用有界日志等待、终帧确认与 300 ms 停留；30 s 回退只属于 U3 未关断阶段（ADR-0015 §2.4），
    // 不得在这里提前结束已确认的终帧停留。
    if (!shutdownSm_.active() || pollStageParticipants(board::ShutdownStage::kCutPower, nowMs)) {
        doRestart();
    } else if (!usbFinalFrame_.holdDeadlineMs) {
        if (const TaskHandle_t self = xTaskGetCurrentTaskHandle(); self != nullptr) {
            waitOledFinalFrame(self);
            if (pollStageParticipants(board::ShutdownStage::kCutPower, rtos::nowMs())) {
                doRestart();
            }
        }
    }
    return false;
}

void PowerModule::handleKeyPoll(uint32_t nowMs, board::KeyEvent event, bool keyPressed) noexcept
{
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
        return;
    }
    if ((ctx().bootMode != board::BootMode::kNormal) || forcePowerOffFired_ || !keyPressed) {
        return;
    }
    const uint32_t holdMs = nowMs - keySm_.pressStartMs();
    if (holdMs < board::kForcePowerOffHoldMs) {
        return;
    }
    forcePowerOffFired_ = true;
    if (power::requestShutdown(board::ShutdownReason::kKey)) {
        FW_LOGW(fw::TraceCat::kSys, "force power-off fallback after %" PRIu32 " ms key hold",
                holdMs);
    }
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
    handleKeyPoll(nowMs, event, pins.keyPressed);

    pollBattery(nowMs, pins);
    if (ctx().bootMode == board::BootMode::kChargeStandby) {
        handleChargeStandbyPowerOn(rtos::nowMs(), event == board::KeyEvent::kLongHold, pins.u3On);
    } else {
        stepShutdown(rtos::nowMs(), pins);
    }
    const bool standbyLocked =
        standbyWake_.active() ||
        ((ctx().bootMode == board::BootMode::kChargeStandby) && restartPending_);
    ctx().data.uiEvents.updateKeyHold(keySm_.holding(), keySm_.pressStartMs(), keySm_.longPressMs(),
                                      standbyLocked);
    publishSnapshot(pins, false);
    return false;
}

} // namespace app
