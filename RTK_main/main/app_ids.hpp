// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once

#include <cstdint>
#include "rtk/core/types.hpp"
#include "rtk/os/executor.hpp"  // IWYU pragma: keep（冻结头中的 app 名称须在产品命名空间出现前解析）
#include "rtk/os/spin_lock.hpp" // IWYU pragma: keep

namespace rtk::app {
inline constexpr uint32_t kCommandTimeoutMs = 1000;
inline constexpr uint32_t kTickPeriodMs = 100;
inline constexpr size_t kMaxSessions = core::kMaxSessions;
inline constexpr size_t kMaxAlarms = core::kMaxAlarms;
inline constexpr size_t kDeviceNameBuf = 32;

enum class ModuleKind : uint8_t { kBtLink, kRtcm, kStorage, kSystem, kSystemMode };
enum class ExecKind : uint8_t { kLink };
enum class LinkVerb : uint8_t { kAlarmSet, kAlarmClear, kPair, kWhitelistClear };
enum class StorageVerb : uint8_t { kSaveSn };
enum class RtcmVerb : uint8_t { kReset };
enum class SystemVerb : uint8_t { kPublish };

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t id(ModuleKind value) noexcept
{
    return static_cast<uint8_t>(value);
}
[[nodiscard]] constexpr uint8_t id(ExecKind value) noexcept
{
    return static_cast<uint8_t>(value);
}
[[nodiscard]] constexpr uint8_t verb(LinkVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}
[[nodiscard]] constexpr uint8_t verb(StorageVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}
[[nodiscard]] constexpr uint8_t verb(RtcmVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}
[[nodiscard]] constexpr uint8_t verb(SystemVerb value) noexcept
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
