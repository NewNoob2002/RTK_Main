// 4G 联网模块跨模块共享类型（ADR-0021 / N1）：net.* 配置只读视图与控制台命令邮箱常量。
// 纯值类型，无 IDF/硬件依赖；不分配堆，线程安全由 DataCenter 槽的锁保证。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "cellular/LinkFsm.hpp"

namespace app {

/**
 * net.* 设置的只读视图（kStorage 写、控制台/设置后端读）；密码不进入视图，只暴露是否已设置。
 * 内容是 NVS 已提交值；4G 模块只在启动时读取一次，故 restartPending 表示“已保存但需重启才生效”。
 */
struct NetConfigView {
    bool enable = false;
    bool passSet = false;
    bool restartPending = false; // 保存后的值与本次启动使用的值不同
    bool available = false;      // 板型含 kCellular（否则 net.* 写被拒绝）
    std::array<char, cellular::LinkConfig::kFieldBytes> apn{};
    std::array<char, cellular::LinkConfig::kFieldBytes> user{};
};

/** NetModule 命令 verb（仅 DEV 控制台；net.* 配置由 StorageModule 持久化，不经 kNet）。 */
enum class NetVerb : uint8_t {
    kPowerOn = 0U,  // DEV：net power on（强制上电，仍受低电量/配网/已分配约束）
    kPowerOff = 1U, // DEV：net power off（优雅关机并保持关闭）
    kReset = 2U,    // DEV：net reset（手动 L3）
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
