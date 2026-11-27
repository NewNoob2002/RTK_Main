// OLED 告警批次与优先级，供 DataCenter、告警生产者和 UI 共用。
// 纯值与纯函数，无 IDF 依赖，不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace app::oled {

// 枚举顺序即显示优先级，由低至高；非围栏告警无需携带名称。
enum class AlarmKind : uint8_t {
    kNone = 0U,
    kNetwork,
    kLowBattery,
    kPrecisionLost,
    kFenceExit,
    kFenceEnter,
};

struct AlarmSnapshot {
    static constexpr size_t kFenceNameBytes = 24U;
    std::array<char, kFenceNameBytes> fenceEnterName{};
    std::array<char, kFenceNameBytes> fenceExitName{};
    uint32_t sequence = 0U; // 新批次更新序号，允许同种事件重新显示。
    unsigned pending = 0U;
};

inline constexpr AlarmSnapshot kNoAlarm{};

// WP-ALARM 的蜂鸣器、LED 和马达应复用此节奏表。
inline constexpr uint32_t kAlarmFlashDurationMs = 30000U;
inline constexpr uint32_t kFenceAlarmPeriodMs = 250U;
inline constexpr uint32_t kOtherAlarmPeriodMs = 500U;

/**
 * @brief 查询告警提示的一次完整亮暗周期。
 * @param kind: 告警类型。
 * @retval 毫秒数。
 * @note 纯函数，可跨任务使用。
 */
[[nodiscard]] constexpr uint32_t alarmFlashPeriodMs(AlarmKind kind) noexcept
{
    return (kind == AlarmKind::kFenceEnter || kind == AlarmKind::kFenceExit) ? kFenceAlarmPeriodMs
                                                                             : kOtherAlarmPeriodMs;
}

/**
 * @brief 取告警类型对应的批次位。
 * @param kind: 告警类型。
 * @retval 可记录或确认的位，kNone 没有对应位。
 * @note 纯函数，可跨任务调用。
 */
[[nodiscard]] constexpr unsigned alarmBit(AlarmKind kind) noexcept
{
    return kind == AlarmKind::kNone ? 0U : 1U << static_cast<unsigned>(kind);
}

/**
 * @brief 从未确认的告警中选择最高优先级。
 * @param pending: 批次中尚未确认的位。
 * @retval 最高优先级类型，空批次返回 kNone。
 * @note 纯函数，可跨任务调用。
 */
[[nodiscard]] constexpr AlarmKind highestAlarm(unsigned pending) noexcept
{
    using enum AlarmKind;
    for (const auto kind : {kFenceEnter, kFenceExit, kPrecisionLost, kLowBattery, kNetwork}) {
        if ((pending & alarmBit(kind)) != 0U) {
            return kind;
        }
    }
    return kNone;
}

} // namespace app::oled
