// P2 电源按键消抖、启动分支与关机序列纯状态机实现（B2）。
// 无 IDF/FreeRTOS 依赖，可在主机与目标固件共用；不阻塞、不分配堆。
#include "board/PowerLogic.hpp"

#include <array>

namespace app::board {

const char *bootModeName(BootMode mode) noexcept
{
    switch (mode) {
    case BootMode::kNormal:
        return "normal";
    case BootMode::kChargeStandby:
        return "charge_standby";
    default:
        return "unknown";
    }
}

const char *powerOnReasonName(PowerOnReason reason) noexcept
{
    switch (reason) {
    case PowerOnReason::kKey:
        return "key";
    case PowerOnReason::kCharge:
        return "charge";
    case PowerOnReason::kLatch:
        return "latch";
    case PowerOnReason::kUnknown:
    default:
        return "unknown";
    }
}

const char *shutdownReasonName(ShutdownReason reason) noexcept
{
    switch (reason) {
    case ShutdownReason::kKey:
        return "key";
    case ShutdownReason::kConsole:
        return "console";
    case ShutdownReason::kLowBattery:
        return "low_battery";
    case ShutdownReason::kPhone:
        return "phone";
    case ShutdownReason::kNone:
    default:
        return "none";
    }
}

const char *shutdownStageName(ShutdownStage stage) noexcept
{
    switch (stage) {
    case ShutdownStage::kIdle:
        return "idle";
    case ShutdownStage::kAnnounce:
        return "announce";
    case ShutdownStage::kSave:
        return "save";
    case ShutdownStage::kQuiesce:
        return "quiesce";
    case ShutdownStage::kCutPower:
        return "cut_power";
    default:
        return "unknown";
    }
}

const char *usbSourceName(UsbSource source) noexcept
{
    switch (source) {
    case UsbSource::kGpio:
        return "gpio";
    case UsbSource::kI2c:
        return "i2c";
    case UsbSource::kStat:
        return "stat";
    case UsbSource::kNone:
    default:
        return "none";
    }
}

ChargerInputStatus decodeSgmReg08(uint8_t rawReg08) noexcept
{
    return ChargerInputStatus{
        .ok = true,
        .powerGood = (rawReg08 & kSgmReg08PgBit) != 0U,
        .vbusStat = static_cast<uint8_t>((rawReg08 >> kSgmReg08VbusShift) & kSgmReg08VbusMask),
        .chrgStat = static_cast<uint8_t>((rawReg08 >> kSgmReg08ChrgShift) & kSgmReg08ChrgMask),
        .rawReg08 = rawReg08,
    };
}

void updateChargerSample(ChargerInputStatus &last, fw::Status status, uint8_t raw) noexcept
{
    if (status != fw::Status::kBusy) {
        last = fw::isOk(status) ? decodeSgmReg08(raw) : ChargerInputStatus{};
    }
}

UsbDetection decideUsbPresent(std::optional<bool> gpioPresent, const ChargerInputStatus &i2cStatus,
                              std::optional<bool> statActive) noexcept
{
    if (gpioPresent.has_value()) {
        return {*gpioPresent, UsbSource::kGpio};
    }
    if (i2cStatus.ok) {
        return UsbDetection{
            .present = i2cStatus.powerGood || (i2cStatus.vbusStat != 0U),
            .source = UsbSource::kI2c,
        };
    }
    if (statActive.has_value()) {
        return UsbDetection{
            .present = *statActive,
            .source = UsbSource::kStat,
        };
    }
    return UsbDetection{};
}

uint8_t batteryPercent(uint32_t mv) noexcept
{
    // P2_V3.1_IDF/main/user_src/user_battery.c:135-146，每个节点相差 10%。
    constexpr std::array<uint32_t, 11> kVoltageMv{3340U, 3440U, 3540U, 3600U, 3630U, 3670U,
                                                  3750U, 3860U, 3940U, 4050U, 4150U};
    constexpr uint32_t kPercentStep = 10U;
    constexpr uint8_t kFullBatteryPct = 100U;
    if (mv <= kVoltageMv.front()) {
        return 0U;
    }
    for (size_t i = 1U; i < kVoltageMv.size(); ++i) {
        if (mv < kVoltageMv[i]) {
            const uint32_t low = kVoltageMv[i - 1U];
            // i-1 在 0..9，插值结果为 0..99，可安全窄化。
            return static_cast<uint8_t>((i - 1U) * kPercentStep +
                                        (mv - low) * kPercentStep / (kVoltageMv[i] - low));
        }
    }
    return kFullBatteryPct;
}

bool LowBatteryTimer::isDue(uint32_t nowMs, bool onBattery, uint8_t pct) noexcept
{
    constexpr uint8_t kLowBatteryPct = 10U;
    constexpr uint32_t kLowBatteryHoldMs = 30000U; // ms
    if (!onBattery || pct >= kLowBatteryPct) {
        sinceMs_.reset();
        return false;
    }
    if (!sinceMs_.has_value()) {
        sinceMs_ = nowMs;
    }
    return nowMs - *sinceMs_ >= kLowBatteryHoldMs;
}

PowerOnAction decidePowerOn(bool senseHigh, bool usbPresent) noexcept
{
    if (senseHigh) {
        return PowerOnAction::kHoldConfirm;
    }
    return usbPresent ? PowerOnAction::kChargeStandby : PowerOnAction::kHoldRecoverWarn;
}

void PowerKeySm::reset(uint32_t longPressMs) noexcept
{
    longPressMs_ = longPressMs < kKeyDebounceMs ? kKeyDebounceMs : longPressMs;
    initialized_ = false;
    armed_ = false;
    stablePressed_ = false;
    candidatePressed_ = false;
    pressing_ = false;
    longFired_ = false;
    candidateSinceMs_ = 0U;
    pressStartMs_ = 0U;
    lastHoldMs_ = 0U;
}

KeyEvent PowerKeySm::poll(uint32_t nowMs, bool rawPressed) noexcept
{
    if (!initialized_) {
        initialized_ = true;
        candidatePressed_ = rawPressed;
        candidateSinceMs_ = nowMs;
        // 启动时若按键仍被用户按住，须先看到一次稳定松开再开始计数。
        return KeyEvent::kNone;
    }

    if (rawPressed != candidatePressed_) {
        candidatePressed_ = rawPressed;
        candidateSinceMs_ = nowMs;
    } else if ((!armed_ || candidatePressed_ != stablePressed_) &&
               ((nowMs - candidateSinceMs_) >= kKeyDebounceMs)) {
        stablePressed_ = candidatePressed_;
        if (!stablePressed_) {
            if (!armed_) {
                armed_ = true;
                return KeyEvent::kNone;
            }
            if (pressing_) {
                const uint32_t heldMs = candidateSinceMs_ - pressStartMs_;
                const bool wasLong = longFired_;
                pressing_ = false;
                longFired_ = false;
                lastHoldMs_ = heldMs;
                if (!wasLong && (heldMs < kKeyShortMaxMs)) {
                    return KeyEvent::kShort;
                }
            }
            return KeyEvent::kNone;
        }
        if (armed_) {
            pressing_ = true;
            longFired_ = false;
            // 以按下沿作为计时起点，使 2500 ms 阈值与物理按住时长一致。
            pressStartMs_ = candidateSinceMs_;
        }
    }

    if (candidatePressed_ && pressing_ && !longFired_ &&
        ((nowMs - pressStartMs_) >= longPressMs_)) {
        longFired_ = true;
        lastHoldMs_ = nowMs - pressStartMs_;
        return KeyEvent::kLongHold;
    }
    return KeyEvent::kNone;
}

StandbyWakeAction StandbyWakeSm::confirmStandbyHold(uint32_t nowMs, bool longPress, bool senseHigh,
                                                    uint32_t timeoutMs) noexcept
{
    if (!active_) {
        if (!longPress) {
            return StandbyWakeAction::kNone;
        }
        active_ = true;
        startedMs_ = nowMs;
        streak_ = 0U;
        return StandbyWakeAction::kPress;
    }
    if (nowMs - startedMs_ >= timeoutMs) {
        active_ = false;
        return StandbyWakeAction::kTimeout;
    }
    // 在实际驱动 GPIO4 之后才采样，连续三轮读高才确认。
    streak_ = senseHigh ? streak_ + 1U : 0U;
    constexpr unsigned kHoldSamples = 3U;
    if (streak_ == kHoldSamples) {
        active_ = false;
        return StandbyWakeAction::kHeld;
    }
    return StandbyWakeAction::kNone;
}

bool ShutdownSm::request(ShutdownReason reason) noexcept
{
    if ((stage_ != ShutdownStage::kIdle) || (reason == ShutdownReason::kNone)) {
        return false;
    }
    stage_ = ShutdownStage::kAnnounce;
    reason_ = reason;
    lastReason_ = reason;
    stageEntered_ = false;
    cutFirstHigh_ = false;
    cutHigh_ = false;
    stageStartMs_ = 0U;
    cutToggleStartMs_ = 0U;
    return true;
}

ShutdownOutput ShutdownSm::step(uint32_t nowMs, bool saveDone, bool quiesceDone, bool senseHigh,
                                std::optional<bool> usbPresent, bool stageVisible,
                                bool cutReady) noexcept
{
    switch (stage_) {
    case ShutdownStage::kIdle:
        return ShutdownOutput{};
    case ShutdownStage::kAnnounce:
        if (!stageEntered_) {
            stageEntered_ = true;
            stageStartMs_ = nowMs;
            return ShutdownOutput{.stage = ShutdownStage::kAnnounce, .stageEntered = true};
        }
        if (!stageVisible) {
            return ShutdownOutput{.stage = ShutdownStage::kAnnounce};
        }
        stage_ = ShutdownStage::kSave;
        stageEntered_ = true;
        stageStartMs_ = nowMs;
        return ShutdownOutput{.stage = ShutdownStage::kSave, .stageEntered = true};
    case ShutdownStage::kSave: {
        const bool timedOut = !saveDone && ((nowMs - stageStartMs_) >= kShutdownSaveTimeoutMs);
        if (!stageVisible || (!saveDone && !timedOut)) {
            return ShutdownOutput{.stage = ShutdownStage::kSave};
        }
        stage_ = ShutdownStage::kQuiesce;
        stageEntered_ = true;
        stageStartMs_ = nowMs;
        return ShutdownOutput{
            .stage = ShutdownStage::kQuiesce,
            .stageEntered = true,
            .saveTimedOut = timedOut,
        };
    }
    case ShutdownStage::kQuiesce: {
        if (!stageVisible || !quiesceDone) {
            return ShutdownOutput{.stage = ShutdownStage::kQuiesce};
        }
        stage_ = ShutdownStage::kCutPower;
        stageEntered_ = true;
        stageStartMs_ = nowMs;
        cutToggleStartMs_ = nowMs;
        cutFirstHigh_ = true;
        cutHigh_ = true;
        cutUsbPresent_ = usbPresent.value_or(false);
        auto out = stepCutPower(nowMs, senseHigh, usbPresent, cutReady);
        out.stageEntered = true;
        return out;
    }
    case ShutdownStage::kCutPower:
        return stepCutPower(nowMs, senseHigh, usbPresent, cutReady);
    default:
        stage_ = ShutdownStage::kIdle;
        reason_ = ShutdownReason::kNone;
        return ShutdownOutput{};
    }
}

ShutdownOutput ShutdownSm::stepCutPower(uint32_t nowMs, bool senseHigh,
                                        std::optional<bool> usbPresent, bool cutReady) noexcept
{
    if ((nowMs - stageStartMs_) >= kShutdownCutTimeoutMs) {
        stage_ = ShutdownStage::kIdle;
        reason_ = ShutdownReason::kNone;
        stageEntered_ = false;
        cutHigh_ = false;
        return ShutdownOutput{.stage = ShutdownStage::kIdle, .cutTimedOut = true};
    }

    if (!cutReady || !usbPresent.has_value()) {
        cutFirstHigh_ = true;
        cutToggleStartMs_ = nowMs;
        cutHigh_ = false;
        return ShutdownOutput{.stage = ShutdownStage::kCutPower};
    }
    if (*usbPresent != cutUsbPresent_) {
        cutUsbPresent_ = *usbPresent;
        // 离开 USB 持续按压协议后，从实际进入本协议的时刻重新计首段。
        cutFirstHigh_ = true;
        cutToggleStartMs_ = nowMs;
    }

    if (*usbPresent) {
        // 有 USB 时持续拉高 GPIO4 直到 GPIO25 读到低（U3 关断），随后释放并请求重启进充电待机。
        if (!senseHigh) {
            cutHigh_ = false;
            return ShutdownOutput{.stage = ShutdownStage::kCutPower, .requestRestart = true};
        }
        cutHigh_ = true;
        return ShutdownOutput{.stage = ShutdownStage::kCutPower, .driveKeyHigh = true};
    }

    // 无 USB：首段高 2 s，随后低/高各 3.1 s 交替，低电平期采样 GPIO25。
    if (cutFirstHigh_) {
        if (!cutHigh_) {
            // 从未知样本或有 USB 低电平恢复为无 USB 时，自恢复时刻起算完整首段 2 s 高电平。
            cutHigh_ = true;
            cutToggleStartMs_ = nowMs;
        } else if ((nowMs - cutToggleStartMs_) >= kShutdownCutInitHighMs) {
            cutFirstHigh_ = false;
            cutHigh_ = false;
            cutToggleStartMs_ = nowMs;
        }
    } else if ((nowMs - cutToggleStartMs_) >= kShutdownCutToggleMs) {
        cutHigh_ = !cutHigh_;
        cutToggleStartMs_ = nowMs;
    }
    return ShutdownOutput{.stage = ShutdownStage::kCutPower, .driveKeyHigh = cutHigh_};
}

} // namespace app::board
