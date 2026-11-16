// 驱动注册与型号元数据：按构建开关从 DriverList.inc 生成 enabled_receivers()（ADR-0012 §2.2/§2.3）。
// 线程安全：是（返回静态常量表，编译期生成，无运行期状态）。
// 堆：不使用。构建开关：IDF 下 CONFIG_RTK_GNSS_DRV_<D>，host 下 RTK_GNSS_DRV_<D>（CMake 转为同名宏）。
#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "gnss/Receiver.hpp"

#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
#define GNSS_ENABLE_UNICORE 1
#else
#define GNSS_ENABLE_UNICORE 0
#endif

#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
#define GNSS_ENABLE_UBLOX 1
#else
#define GNSS_ENABLE_UBLOX 0
#endif

#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
#define GNSS_ENABLE_COMNAV 1
#else
#define GNSS_ENABLE_COMNAV 0
#endif

// 只包含已启用驱动的头（未启用驱动的 .cpp 不参与构建，其 accessor 也就不存在）。
#if GNSS_ENABLE_UNICORE
#include "gnss/drivers/UnicoreReceiver.hpp"
#endif
#if GNSS_ENABLE_UBLOX
#include "gnss/drivers/UbloxReceiver.hpp"
#endif
#if GNSS_ENABLE_COMNAV
#include "gnss/drivers/ComnavReceiver.hpp"
#endif

namespace gnss {
namespace {

constexpr size_t kEnabledCount = GNSS_ENABLE_UNICORE + GNSS_ENABLE_UBLOX + GNSS_ENABLE_COMNAV;

} // namespace

std::span<const Receiver *const> enabled_receivers() noexcept
{
    // 地址表：accessor 不是 constexpr 函数，故用 const 静态表（地址为常量，仍是常量初始化）。
    static const std::array<const Receiver *, kEnabledCount> kList{
#define GNSS_DRIVER(suffix, family_, type, accessor) &accessor(),
#include "gnss/DriverList.inc"
#undef GNSS_DRIVER
    };
    return kList;
}

const Receiver *receiver_for(Family family) noexcept
{
    for (const Receiver *receiver : enabled_receivers()) {
        if (receiver->family() == family) {
            return receiver;
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

const char *family_name(Family family) noexcept
{
    switch (family) {
    case Family::kUnicore:
        return "unicore";
    case Family::kUblox:
        return "ublox";
    case Family::kComnav:
        return "comnav";
    case Family::kUnknown:
    default:
        return "unknown";
    }
}

std::optional<Family> family_from_u8(uint8_t value) noexcept
{
    // NVS 里的家族号只可能来自本固件写入；越界值（旧格式/损坏）返回 nullopt，由调用方决定回落策略。
    switch (value) {
    case static_cast<uint8_t>(Family::kUnicore):
        return Family::kUnicore;
    case static_cast<uint8_t>(Family::kUblox):
        return Family::kUblox;
    case static_cast<uint8_t>(Family::kComnav):
        return Family::kComnav;
    case static_cast<uint8_t>(Family::kUnknown):
        return Family::kUnknown;
    default:
        return std::nullopt;
    }
}

std::optional<Model> model_from_u8(uint8_t value) noexcept
{
    switch (value) {
    case static_cast<uint8_t>(Model::kUm980):
        return Model::kUm980;
    case static_cast<uint8_t>(Model::kUm981):
        return Model::kUm981;
    case static_cast<uint8_t>(Model::kUm982):
        return Model::kUm982;
    case static_cast<uint8_t>(Model::kZedF9p):
        return Model::kZedF9p;
    case static_cast<uint8_t>(Model::kK803):
        return Model::kK803;
    case static_cast<uint8_t>(Model::kUnknown):
        return Model::kUnknown;
    default:
        return std::nullopt;
    }
}

} // namespace gnss
