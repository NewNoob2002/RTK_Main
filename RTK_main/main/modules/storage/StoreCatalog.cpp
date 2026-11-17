// 持久化目录实现（ADR-0010 §2.10）：每个类别一个条件块，功能关闭时登记空表而不是改表的形状。

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
constexpr std::span<const fw::StoreKey> kGnssCache{gnss_store::kCacheKeys};

/**
 * @brief  gnss.cache 的数值显示名：family/model 存的是 gnss::Family/Model 枚举值
 * @param  key: 键名
 * @param  value: NVS 读出的数值
 * @retval 家族/型号短名；其他键或越界值返回 nullptr
 */
const char *gnss_cache_value_name(std::string_view key, uint32_t value) noexcept
{
    if (value > UINT8_MAX) {
        return nullptr;
    }
    const auto narrow = static_cast<uint8_t>(value);
    if (key == "family") {
        const auto family = gnss::family_from_u8(narrow);
        return family.has_value() ? gnss::family_name(*family) : nullptr;
    }
    if (key == "model") {
        const auto model = gnss::model_from_u8(narrow);
        return model.has_value() ? gnss::model_name(*model) : nullptr;
    }
    return nullptr;
}
constexpr StoreValueName kGnssCacheNames = &gnss_cache_value_name;
#else
constexpr std::span<const fw::StoreKey> kGnssConfig{};
constexpr std::span<const fw::StoreKey> kGnssCache{};
constexpr StoreValueName kGnssCacheNames = nullptr;
#endif

#if FW_TRACE_ENABLED
constexpr std::span<const fw::StoreKey> kDebug{debug::kKeys};
#else
constexpr std::span<const fw::StoreKey> kDebug{};
#endif

constexpr std::array<StoreGroup, 3U> kCatalog{{
    {"gnss.config", "GNSS user settings", kGnssConfig, nullptr},
    {"gnss.cache", "last detected GNSS receiver (probe hint)", kGnssCache, kGnssCacheNames},
    {"debug", "trace settings", kDebug, nullptr},
}};

} // namespace

std::span<const StoreGroup> store_catalog() noexcept
{
    return kCatalog;
}

} // namespace app
