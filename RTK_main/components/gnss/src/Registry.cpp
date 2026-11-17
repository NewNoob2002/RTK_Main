// 驱动注册与型号元数据：按构建开关从 DriverList.inc 生成 enabled_receivers()（ADR-0012 §2.2/§2.3）。
// 线程安全：是（返回命名空间作用域的常量初始化地址表，无运行期状态）；可在任意任务/ISR 取用。
// 堆：不使用。构建开关：IDF 下 CONFIG_RTK_GNSS_DRV_<D>，host 下 RTK_GNSS_DRV_<D>（CMake 转为同名宏）。
#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "gnss/Receiver.hpp"
// 驱动单例声明与 GNSS_ENABLE_<D> 开关归一化（只包含已启用驱动的头）。
#include "DriverInstances.hpp"

namespace gnss {
namespace {

constexpr size_t kEnabledCount = GNSS_ENABLE_UNICORE + GNSS_ENABLE_UBLOX + GNSS_ENABLE_COMNAV;

// 地址表在命名空间作用域做常量初始化（CPP-009）：不放进函数内静态，避免动态初始化与 __cxa_guard_*。
constinit const std::array<const Receiver *, kEnabledCount> kReceivers{
#define GNSS_DRIVER(suffix, family_, type, accessor) &detail::k##type,
#include "gnss/DriverList.inc"
#undef GNSS_DRIVER
};

} // namespace

/** @brief 按构建开关排好探测优先级的驱动地址表。
 * @return 常量初始化数组的 span（Unicore → u-blox → ComNav，未启用的驱动不出现）。
 * @note 无运行期状态，可在任意任务/ISR 调用。
 */
std::span<const Receiver *const> enabled_receivers() noexcept
{
    return kReceivers;
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
