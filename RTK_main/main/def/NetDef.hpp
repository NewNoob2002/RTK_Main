// 4G 联网模块跨模块共享类型（ADR-0021 / N1）：net.* 配置只读视图与控制台命令邮箱常量。
// 纯值类型，无 IDF/硬件依赖；不分配堆，线程安全由 DataCenter 槽的锁保证。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "cellular/LinkFsm.hpp"

namespace app {

/** net.* 设置的只读视图（kNet 写、kLink/Web/控制台读）；密码不进入视图，只暴露是否已设置。 */
struct NetConfigView {
    bool enable = false;
    bool passSet = false;
    std::array<char, cellular::LinkConfig::kFieldBytes> apn{};
    std::array<char, cellular::LinkConfig::kFieldBytes> user{};
};

/** NetModule 命令 verb（与 settings::kVerbNet* 数值一致，见 SettingsBackend 的 static_assert）。 */
enum class NetVerb : uint8_t {
    kSetEnable = 0U,
    kSetApn = 1U,
    kSetUser = 2U,
    kSetPass = 3U,
    kPowerOn = 4U,  // DEV：net power on（强制上电，忽略 net.enable，仍受低电量/配网约束）
    kPowerOff = 5U, // DEV：net power off（优雅关机并保持关闭）
    kReset = 6U,    // DEV：net reset（手动 L3）
};

/**
 * @brief 取得 NetVerb 的命令编号
 * @param value: 命令动词
 * @retval 与枚举底层值相同的 uint8_t
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t verb(NetVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** net.apn/user/pass 文本最大字符数（不含结尾 0），与 settings::kNetTextMaxBytes 一致。 */
inline constexpr size_t kNetTextMaxChars = cellular::LinkConfig::kFieldBytes - 1U;

/** 控制台 AT 透传邮箱容量（命令/响应文本，含结尾 0）。 */
inline constexpr size_t kNetAtCmdBytes = 96U;
inline constexpr size_t kNetAtRspBytes = 192U;

} // namespace app
