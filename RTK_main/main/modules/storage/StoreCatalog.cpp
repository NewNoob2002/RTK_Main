// 持久化目录实现（ADR-0010 §2.10）：每个类别一个条件块，功能关闭时登记空表而不是改表的形状。

#include "modules/storage/StoreCatalog.hpp"

#include <array>

#include "AppConfig.hpp"
#include "fw/Trace.hpp" // FW_TRACE_ENABLED

#if FW_TRACE_ENABLED
#include "modules/debug/DebugKeys.hpp"
#endif
#if RTK_GNSS_ENABLED
#include "modules/gnss/GnssKeys.hpp"
#endif

namespace app {

namespace {

#if RTK_GNSS_ENABLED
constexpr std::span<const fw::StoreKey> kGnssConfig{gnss_store::kConfigKeys};
constexpr std::span<const fw::StoreKey> kGnssCache{gnss_store::kCacheKeys};
#else
constexpr std::span<const fw::StoreKey> kGnssConfig{};
constexpr std::span<const fw::StoreKey> kGnssCache{};
#endif

#if FW_TRACE_ENABLED
constexpr std::span<const fw::StoreKey> kDebug{debug::kKeys};
#else
constexpr std::span<const fw::StoreKey> kDebug{};
#endif

constexpr std::array<StoreGroup, 3U> kCatalog{{
    {"gnss.config", "GNSS user settings", kGnssConfig},
    {"gnss.cache", "last detected GNSS receiver (probe hint)", kGnssCache},
    {"debug", "trace settings", kDebug},
}};

} // namespace

std::span<const StoreGroup> store_catalog() noexcept
{
    return kCatalog;
}

} // namespace app
