// 方言注册与型号元数据：按构建开关生成 enabled_dialects()（ADR-0010 §3）。
// 线程安全：是（返回静态常量表，编译期生成，无运行期状态）。
// 堆：不使用。构建开关：IDF 下 CONFIG_RTK_GNSS_DIALECT_<D>，host 下 RTK_GNSS_DIALECT_<D>（CMake 转为同名宏）。
#include "gnss/Dialect.hpp"

#include <array>
#include <cstddef>
#include <span>

#include "gnss/dialects/Comnav.hpp"
#include "gnss/dialects/Ublox.hpp"
#include "gnss/dialects/Unicore.hpp"

#if defined(RTK_GNSS_DIALECT_UNICORE) && RTK_GNSS_DIALECT_UNICORE
#define GNSS_ENABLE_UNICORE 1
#else
#define GNSS_ENABLE_UNICORE 0
#endif

#if defined(RTK_GNSS_DIALECT_UBLOX) && RTK_GNSS_DIALECT_UBLOX
#define GNSS_ENABLE_UBLOX 1
#else
#define GNSS_ENABLE_UBLOX 0
#endif

#if defined(RTK_GNSS_DIALECT_COMNAV) && RTK_GNSS_DIALECT_COMNAV
#define GNSS_ENABLE_COMNAV 1
#else
#define GNSS_ENABLE_COMNAV 0
#endif

namespace gnss {
namespace {

constexpr size_t kEnabledCount = GNSS_ENABLE_UNICORE + GNSS_ENABLE_UBLOX + GNSS_ENABLE_COMNAV;

} // namespace

std::span<const Dialect *const> enabled_dialects() noexcept
{
    // 顺序即探测优先级：Unicore → u-blox → ComNav（实测优先，实验性方言最后）。
    static constexpr std::array<const Dialect *, kEnabledCount> kList{
#if GNSS_ENABLE_UNICORE
        &unicore::kDialect,
#endif
#if GNSS_ENABLE_UBLOX
        &ublox::kDialect,
#endif
#if GNSS_ENABLE_COMNAV
        &comnav::kDialect,
#endif
    };
    return kList;
}

const Dialect *dialect_for(Family family) noexcept
{
    for (const Dialect *dialect : enabled_dialects()) {
        if (dialect != nullptr && dialect->family == family) {
            return dialect;
        }
    }
    return nullptr;
}

const char *model_name(Model model) noexcept
{
    switch (model) {
    case Model::kUm980:
        return "UM980";
    case Model::kUm981:
        return "UM981";
    case Model::kUm982:
        return "UM982";
    case Model::kZedF9p:
        return "ZED-F9P";
    case Model::kK803:
        return "K803";
    case Model::kUnknown:
    default:
        return "unknown";
    }
}

Family family_of(Model model) noexcept
{
    switch (model) {
    case Model::kUm980:
    case Model::kUm981:
    case Model::kUm982:
        return Family::kUnicore;
    case Model::kZedF9p:
        return Family::kUblox;
    case Model::kK803:
        return Family::kComnav;
    case Model::kUnknown:
    default:
        return Family::kUnknown;
    }
}

} // namespace gnss
