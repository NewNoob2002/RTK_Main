// OLED 蜂窝状态值、信号迟滞与状态栏图标判定（OL9）。
// 纯逻辑，由 OLED 任务或主机测试串行调用，不分配堆，非 ISR。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "cellular/Types.hpp"

namespace app::oled {

struct NetView {
    cellular::Phase phase = cellular::Phase::kOff;
    cellular::SimStatus sim = cellular::SimStatus::kUnknown;
    cellular::RegStatus reg = cellular::RegStatus::kUnknown;
    cellular::RecoveryLevel recoveryLevel = cellular::RecoveryLevel::kNone;
    uint16_t recoveryCount = 0U;
    uint8_t csq = cellular::kCsqUnknown;
    bool supported = false;
    bool enabled = false;
    bool pdpUp = false;
    bool ntripConnected = false;
    bool mqttConnected = false;
    std::array<char, cellular::kImeiBytes> imei{};
    std::array<char, cellular::kIccidBytes> maskedIccid{};
};

inline constexpr uint8_t kSignalBarCount = 4U;
inline constexpr uint32_t kSignalRiseMs = 10000U;
inline constexpr uint32_t kSignalSearchFrameMs = 250U;

/**
 * @brief 将有效 CSQ 量化为 1–4 格。
 * @param csq: 模组信号值，未知值为 99。
 * @retval 信号格数，未知为 0。
 * @note 纯函数，可跨任务调用。
 */
[[nodiscard]] constexpr uint8_t csqBars(uint8_t csq) noexcept
{
    constexpr uint8_t kTwoBars = 10U;
    constexpr uint8_t kThreeBars = 15U;
    constexpr uint8_t kFourBars = 20U;
    constexpr uint8_t kMaxCsq = 31U;
    if (csq > kMaxCsq) {
        return 0U;
    }
    // 首个未达到的门槛决定格数，合法结果为 1–4。
    const std::array thresholds{kTwoBars, kThreeBars, kFourBars};
    return static_cast<uint8_t>(1 +
                                (std::ranges::upper_bound(thresholds, csq) - thresholds.begin()));
}

struct SignalBars {
    uint32_t sinceMs = 0U;
    uint8_t shown = 0U;
    uint8_t candidate = 0U;
};

/**
 * @brief 推进信号格数，升格候选连续保持十秒才显示，降格即时生效。
 * @param previous: 上一次迟滞状态。
 * @param target: 本次映射候选格数。
 * @param nowMs: 单调毫秒，允许回绕。
 * @retval 更新后的迟滞状态。
 * @note 纯函数，可跨任务调用。
 */
[[nodiscard]] constexpr SignalBars updateSignalBars(SignalBars previous, uint8_t target,
                                                    uint32_t nowMs) noexcept
{
    if (target <= previous.shown) {
        return {.shown = target, .candidate = target};
    }
    if (target != previous.candidate) {
        return {.sinceMs = nowMs, .shown = previous.shown, .candidate = target};
    }
    if ((nowMs - previous.sinceMs) >= kSignalRiseMs) {
        previous.shown = target;
    }
    return previous;
}

enum class CellularIcon : uint8_t { kHidden, kSearching, kSimError, kFault, kSignal, kPdp };

/**
 * @brief 选择信号图标样式，SIM 故障优先于上次保留的注册或承载信息。
 * @param net: 蜂窝视图。
 * @retval 本帧图标样式。
 * @note 纯函数，可跨任务调用。
 */
[[nodiscard]] constexpr CellularIcon cellularIcon(const NetView &net) noexcept
{
    using enum cellular::SimStatus;
    if (!net.supported || !net.enabled || net.phase == cellular::Phase::kOff) {
        return CellularIcon::kHidden;
    }
    if (net.sim == kAbsent || net.sim == kPinRequired || net.sim == kError) {
        return CellularIcon::kSimError;
    }
    if (net.phase == cellular::Phase::kPowerFault) {
        return CellularIcon::kFault;
    }
    if (net.pdpUp) {
        return CellularIcon::kPdp;
    }
    const bool registered =
        net.reg == cellular::RegStatus::kHome || net.reg == cellular::RegStatus::kRoaming;
    return registered ? CellularIcon::kSignal : CellularIcon::kSearching;
}

} // namespace app::oled
