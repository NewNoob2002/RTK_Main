// 一次性射频切换记录的编解码与 ESP-IDF 软件重启适配。
// RTC 记录仅在启动任务或串行化的重启入口访问，非 ISR，不分配堆。
#include "app/RadioMode.hpp"

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_system.h"
#include "sdkconfig.h"
#endif

namespace app {
namespace {

constexpr uint32_t kMagic = 0x524BU;
constexpr unsigned kMagicShift = 16U;
constexpr unsigned kModeShift = 12U;
constexpr unsigned kReasonShift = 8U;
constexpr uint32_t kNibbleMask = 0xFU;
constexpr uint32_t kByteMask = 0xFFU;

#if defined(ESP_PLATFORM)
// RTC_NOINIT 在软件重启时由 IDF 保留。volatile 访问保证清除和写入实际到达保留区。
RTC_NOINIT_ATTR constinit volatile uint32_t s_radio_record{};
#endif

/**
 * @brief 计算记录高 24 位的 CRC-8/SMBUS（多项式 0x07，初值 0）
 * @param record: magic、mode 和 reason 位于高 24 位
 * @retval CRC 字节
 * @note 纯计算，线程和 ISR 安全。
 */
constexpr uint32_t recordCrc(uint32_t record) noexcept
{
    constexpr uint32_t kPolynomial = 0x07U;
    constexpr uint32_t kTopBit = 0x80U;
    uint32_t crc = 0U;
    constexpr unsigned kTopByteShift = 24U;
    for (unsigned shift = kTopByteShift; shift > 0U; shift -= 8U) {
        crc ^= (record >> shift) & kByteMask;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t feedback = (crc & kTopBit) != 0U ? kPolynomial : 0U;
            crc = ((crc << 1U) ^ feedback) & kByteMask;
        }
    }
    return crc;
}

} // namespace

const char *radioReasonName(std::optional<RadioReason> reason) noexcept
{
    if (!reason.has_value()) {
        return "default";
    }
    switch (*reason) {
    case RadioReason::kConsole:
        return "console";
    case RadioReason::kPhone:
        return "phone";
    case RadioReason::kWebExit:
        return "web-exit";
    case RadioReason::kIdle:
        return "idle";
    case RadioReason::kUi:
        return "ui";
    default:
        return "unknown";
    }
}

uint32_t encodeRadioRecord(RadioMode mode, RadioReason reason) noexcept
{
    // 枚举底层类型为 uint8_t，扩展到 uint32_t 后按界内字段编码。
    const auto modeValue = static_cast<uint32_t>(mode);
    const auto reasonValue = static_cast<uint32_t>(reason);
    if (modeValue > static_cast<uint32_t>(RadioMode::kWifiConfig) ||
        reasonValue > static_cast<uint32_t>(RadioReason::kUi)) {
        return 0U;
    }
    const uint32_t payload =
        (kMagic << kMagicShift) | (modeValue << kModeShift) | (reasonValue << kReasonShift);
    return payload | recordCrc(payload);
}

RadioSelection decodeRadioRecord(uint32_t record) noexcept
{
    const uint32_t mode = (record >> kModeShift) & kNibbleMask;
    const uint32_t reason = (record >> kReasonShift) & kNibbleMask;
    if ((record >> kMagicShift) != kMagic || (record & kByteMask) != recordCrc(record) ||
        mode > static_cast<uint32_t>(RadioMode::kWifiConfig) ||
        reason > static_cast<uint32_t>(RadioReason::kUi)) {
        return {};
    }
    // 上面的范围检查保证转换到两个 uint8_t 枚举安全。
    return {static_cast<RadioMode>(mode), static_cast<RadioReason>(reason)};
}

RadioSelection consumeRadioRecord(volatile uint32_t &record, bool softwareReset) noexcept
{
    const uint32_t saved = record;
    record = 0U;
    return softwareReset ? decodeRadioRecord(saved) : RadioSelection{};
}

#if defined(ESP_PLATFORM)
void requestRadioMode([[maybe_unused]] RadioMode mode, [[maybe_unused]] RadioReason reason) noexcept
{
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE)
    s_radio_record = encodeRadioRecord(mode, reason);
#endif
    esp_restart();
}

RadioSelection consumeRadioRequest() noexcept
{
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE)
    return consumeRadioRecord(s_radio_record, esp_reset_reason() == ESP_RST_SW);
#else
    s_radio_record = 0U;
    return {};
#endif
}
#endif

} // namespace app
