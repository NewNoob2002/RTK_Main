// ADR-0015 配置表、窗口匹配与电源保持判定。
// 静态只读表；纯函数可从任意任务或 ISR 调用。
#include "board/BoardProfile.hpp"

#include <array>

namespace app::board {
namespace {

constexpr std::array kP2SafeOutputs{SafeOutput{13, true}, SafeOutput{15, false},
                                    SafeOutput{23, false}, SafeOutput{26, false},
                                    SafeOutput{4, false}};

// Feature 的底层值就是 8 位掩码；按位或的结果仍可无损存入 uint8_t。
constexpr uint8_t kP2Features =
    static_cast<uint8_t>(Feature::kCellular) | static_cast<uint8_t>(Feature::kOled) |
    static_cast<uint8_t>(Feature::kImu) | static_cast<uint8_t>(Feature::kCharger) |
    static_cast<uint8_t>(Feature::kBeeper) | static_cast<uint8_t>(Feature::kMotor) |
    static_cast<uint8_t>(Feature::kLeds);

constexpr std::array kProfiles{
    Profile{.id = BoardId::kP2V141,
            .name = "P2 v1.41",
            .product = "P2",
            .idMinMv = 1500U,
            .idMaxMv = 1800U,
            .gnss = {.uart = 2, .rxGpio = 16, .txGpio = 17, .receiverPort = 1U},
            .powerHold = {.kind = HoldKind::kKeyEmulate,
                          .keyGpio = 4,
                          .senseGpio = 25,
                          .senseOnMv = 1600U,
                          .holdTimeoutMs = 5000U},
            .safeOutputs = kP2SafeOutputs,
            .features = kP2Features},
    Profile{
        .id = BoardId::kE1DevKit,
        .name = "E1 DevKit",
        .product = "E1",
        .gnss = {.uart = 2, .rxGpio = 16, .txGpio = 17, .powerGpio = 25, .powerActiveHigh = true},
        .powerHold = {.kind = HoldKind::kLatchGpio, .latch = {26, true}}},
};

#if !defined(ESP_PLATFORM) && defined(CONFIG_RTK_GNSS_UART_NUM)
static_assert([] {
    for (const Profile &profile : kProfiles) {
        if (profile.gnss.uart != CONFIG_RTK_GNSS_UART_NUM) {
            return false;
        }
    }
    return true;
}());
#endif

} // namespace

std::span<const Profile> profiles() noexcept
{
    return kProfiles;
}

const Profile *matchById(uint32_t mv) noexcept
{
    for (const Profile &profile : kProfiles) {
        if (profile.idMinMv != 0U && mv >= profile.idMinMv && mv <= profile.idMaxMv) {
            return &profile;
        }
    }
    return nullptr;
}

const Profile *byId(BoardId id) noexcept
{
    for (const Profile &profile : kProfiles) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

HoldDecision checkHold(const PowerHold &hold, uint32_t mv, uint32_t elapsedMs,
                       uint8_t &streak) noexcept
{
    constexpr uint8_t kRequiredSamples = 3U;
    if (elapsedMs >= hold.holdTimeoutMs) {
        return HoldDecision::kTimeout;
    }
    if (mv < hold.senseOnMv) {
        streak = 0U;
    } else if (streak < kRequiredSamples) {
        ++streak;
    }
    return streak == kRequiredSamples ? HoldDecision::kHeld : HoldDecision::kContinue;
}

} // namespace app::board
