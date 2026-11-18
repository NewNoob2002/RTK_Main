// 持久化目录实现（ADR-0010 §2.10）：每个类别一个条件块，功能关闭时登记空表而不是改表的形状。
// 只含常量表与纯查询函数，无运行期状态；可在任何任务读取，ISR 安全。

#include "modules/storage/StoreCatalog.hpp"

#include <array>
#include <cstdint>
#include <string_view>

#include "AppConfig.hpp"
#include "fw/Trace.hpp" // FW_TRACE_ENABLED

#if FW_TRACE_ENABLED
#include "modules/debug/DebugKeys.hpp"
#endif
#if RTK_GNSS_ENABLED
#include "gnss/Types.hpp"
#include "modules/gnss/GnssKeys.hpp"
#endif

namespace app {

namespace {

#if RTK_GNSS_ENABLED
constexpr std::span<const fw::StoreKey> kGnssConfig{gnss_store::kConfigKeys};
constexpr std::span<const fw::StoreKey> kGnssBase{gnss_store::kBaseKeys};
constexpr std::span<const fw::StoreKey> kGnssCache{gnss_store::kCacheKeys};

/**
 * @brief  gnss.base 的枚举数值显示名（mode / base_type）
 * @param  key: 键名
 * @param  value: NVS 读出的数值
 * @retval 可读枚举名；其他键或越界值返回 nullptr
 */
const char *gnssBaseValueName(std::string_view key, uint32_t value) noexcept
{
    if (key == "mode") {
        if (value == static_cast<uint32_t>(WorkModeKind::kRover)) {
            return "rover";
        }
        if (value == static_cast<uint32_t>(WorkModeKind::kBase)) {
            return "base";
        }
        if (value == static_cast<uint32_t>(WorkModeKind::kSmartBase)) {
            return "smartbase";
        }
    }
    if (key == "base_type") {
        if (value == static_cast<uint32_t>(BaseType::kSurvey)) {
            return "survey";
        }
        if (value == static_cast<uint32_t>(BaseType::kFixed)) {
            return "fixed";
        }
    }
    return nullptr;
}
constexpr StoreValueName kGnssBaseNames = &gnssBaseValueName;

/**
 * @brief  gnss.cache 的数值显示名：family/model 存的是 gnss::Family/Model 枚举值
 * @param  key: 键名
 * @param  value: NVS 读出的数值
 * @retval 家族/型号短名；其他键或越界值返回 nullptr
 */
const char *gnssCacheValueName(std::string_view key, uint32_t value) noexcept
{
    if (value > UINT8_MAX) {
        return nullptr;
    }
    const auto narrow = static_cast<uint8_t>(value);
    if (key == "family") {
        const auto family = gnss::familyFromU8(narrow);
        return family.has_value() ? gnss::familyName(*family) : nullptr;
    }
    if (key == "model") {
        const auto model = gnss::modelFromU8(narrow);
        return model.has_value() ? gnss::modelName(*model) : nullptr;
    }
    return nullptr;
}
constexpr StoreValueName kGnssCacheNames = &gnssCacheValueName;
#else
constexpr std::span<const fw::StoreKey> kGnssConfig{};
constexpr std::span<const fw::StoreKey> kGnssBase{};
constexpr std::span<const fw::StoreKey> kGnssCache{};
constexpr StoreValueName kGnssBaseNames = nullptr;
constexpr StoreValueName kGnssCacheNames = nullptr;
#endif

#if FW_TRACE_ENABLED
constexpr std::span<const fw::StoreKey> kDebug{debug::kKeys};
#else
constexpr std::span<const fw::StoreKey> kDebug{};
#endif

constexpr std::array<StoreGroup, 4U> kCatalog{{
    {"gnss.config", "GNSS user settings", kGnssConfig, nullptr},
    {"gnss.base", "GNSS work mode & base settings", kGnssBase, kGnssBaseNames},
    {"gnss.cache", "last detected GNSS receiver (probe hint)", kGnssCache, kGnssCacheNames},
    {"debug", "trace settings", kDebug, nullptr},
}};

} // namespace

std::span<const StoreGroup> storeCatalog() noexcept
{
    return kCatalog;
}

} // namespace app
