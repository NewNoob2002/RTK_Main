// 应用全局配置常量、模块/执行器编号与策略查询（合并原 app_version.hpp 与 app_ids.hpp 全局部分）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "rtk/core/types.hpp"
#include "rtk/os/executor.hpp"  // IWYU pragma: keep（冻结头中的 app 名称须在产品命名空间出现前解析）
#include "rtk/os/spin_lock.hpp" // IWYU pragma: keep

namespace rtk::app {

inline constexpr std::string_view kAppVersion = "6.1.0-dev";

inline constexpr uint32_t kCommandTimeoutMs = 1000;
inline constexpr uint32_t kTickPeriodMs = 100;
inline constexpr size_t kMaxSessions = core::kMaxSessions;
inline constexpr size_t kMaxAlarms = core::kMaxAlarms;
inline constexpr size_t kDeviceNameBuf = 32;

enum class ModuleKind : uint8_t { kBtLink, kRtcm, kStorage, kSystem, kSystemMode };
enum class ExecKind : uint8_t { kLink };

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t id(ModuleKind value) noexcept
{
    return static_cast<uint8_t>(value);
}
[[nodiscard]] constexpr uint8_t id(ExecKind value) noexcept
{
    return static_cast<uint8_t>(value);
}

// 线程/ISR 安全：纯函数。
[[nodiscard]] constexpr core::Policy policy_from_config() noexcept
{
#if defined(CONFIG_RTK_LINK_POLICY_PHONE_PREEMPT)
    return core::Policy::kPhonePreempt;
#else
    return core::Policy::kConcurrent;
#endif
}
[[nodiscard]] inline const char *policy_name(core::Policy policy) noexcept
{
    return policy == core::Policy::kPhonePreempt ? "PHONE_PREEMPT" : "CONCURRENT";
}

} // namespace rtk::app
