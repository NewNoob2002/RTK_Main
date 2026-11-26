// ADR-0022 EEPROM 字段解码与规范化。
// 纯计算，线程安全，不分配堆，也不访问 NVS。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "PhoneCompatService.hpp"
#include "data/GnssOutput.hpp"

namespace app::store {

inline constexpr size_t kV27EepromBytes = 2280U;
inline constexpr size_t kV31EepromBytes = 2560U;

struct LegacyFields {
    std::array<char, core::kPhoneSnBytes + 1U> sn{};
    core::PhonePn pn{};
    core::SatTrackConfig satellite = core::SatTrackConfig::defaults();
    data::GnssOutConfig output = data::GnssOutConfig::defaults();
    core::PhoneJsonServerConfig json{};
    core::PhoneThingsboardConfig thingsboard{};

    // 合并到同一个键的字段一起写入或跳过，但统计仍按源字段。
    uint8_t satelliteCount = 0U;
    uint8_t outputCount = 0U;
    uint8_t jsonCount = 0U;
    uint8_t thingsboardCount = 0U;
    uint8_t skipped = 0U;
    uint8_t invalid = 0U;
};

/** @brief 判断旧 EEPROM blob 长度。 @param len: blob 字节数。 @retval 是否为 V2.7 或 V3.1。 */
[[nodiscard]] constexpr bool legacyEepromLength(size_t len) noexcept
{
    return len == kV27EepromBytes || len == kV31EepromBytes;
}

/**
 * @brief 解码旧 EEPROM，并用现有配置编码校验可迁移字段。
 * @param blob: 完整 EEPROM blob。
 * @param rateHz: 当前导航频率，须可整除 1000 ms 且大于 0。
 * @retval 规范字段与源字段统计；输入长度或导航频率非法时为空。
 * @note 纯计算，无共享状态，不阻塞。
 */
[[nodiscard]] std::optional<LegacyFields> decodeLegacyEeprom(std::span<const uint8_t> blob,
                                                             uint8_t rateHz) noexcept;

} // namespace app::store
