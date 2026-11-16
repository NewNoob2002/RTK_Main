// 应用全局配置常量、模块/执行器编号与策略查询（合并原 app_version.hpp 与 app_ids.hpp 全局部分）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/Types.hpp"
#include "rtos/Executor.hpp" // IWYU pragma: keep（冻结头中的 app 名称须在产品命名空间出现前解析）
#include "rtos/SpinLock.hpp" // IWYU pragma: keep

namespace app {

inline constexpr std::string_view kAppVersion = "6.1.0-dev";

inline constexpr uint32_t kCommandTimeoutMs = 1000;
inline constexpr uint32_t kTickPeriodMs = 100;
inline constexpr size_t kMaxSessions = core::kMaxSessions;
inline constexpr size_t kMaxAlarms = core::kMaxAlarms;
inline constexpr size_t kDeviceNameBuf = 32;
#if defined(CONFIG_RTK_LINK_RX_BUF_BYTES)
inline constexpr size_t kLinkRxBufBytes = static_cast<size_t>(CONFIG_RTK_LINK_RX_BUF_BYTES);
#else
inline constexpr size_t kLinkRxBufBytes = 512U;
#endif

// 统一宏（host 与 CONFIG_RTK_GNSS_ENABLE=n 下均为 0），避免各处重复判断 defined。
#if defined(CONFIG_RTK_GNSS_ENABLE) && CONFIG_RTK_GNSS_ENABLE
#define RTK_GNSS_ENABLED 1 // NOLINT(cppcoreguidelines-macro-usage): 全仓以 #if 条件编译使用，constexpr 不能参与预处理
#else
#define RTK_GNSS_ENABLED 0
#endif

#if defined(CONFIG_RTK_GNSS_APP_RING_BYTES)
inline constexpr size_t kGnssAppRingBytes = static_cast<size_t>(CONFIG_RTK_GNSS_APP_RING_BYTES);
#else
inline constexpr size_t kGnssAppRingBytes = 4096U;
#endif

#if defined(CONFIG_RTK_GNSS_STATIC_OBS_ENABLE) && CONFIG_RTK_GNSS_STATIC_OBS_ENABLE
inline constexpr bool kGnssStaticObsEnabled = true;
#else
inline constexpr bool kGnssStaticObsEnabled = false;
#endif
inline constexpr size_t kGnssObsReservedBytes = 16384U;

/** 模块编号（注册清单使用）；枚举顺序即 ID，勿插入或重排。 */
enum class ModuleKind : uint8_t { kBtLink, kRtcm, kStorage, kSystem, kSystemMode, kDebug, kGnss };
/** 执行器编号；链路与 GNSS IO 分别绑核运行。 */
enum class ExecKind : uint8_t { kLink, kIo };

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

} // namespace app
