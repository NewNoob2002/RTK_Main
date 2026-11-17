// 驱动单例的外部声明（gnss::detail）：让注册表在命名空间作用域完成常量初始化，
// 避免函数内静态的动态初始化与 __cxa_guard_*（CPP-009）。
// 由 src/Registry.cpp 与各驱动 .cpp 包含；只含宏与 extern 声明，无运行期状态。
#pragma once

// 构建开关：IDF 下 CONFIG_RTK_GNSS_DRV_<D>，host 下 RTK_GNSS_DRV_<D>（CMake 转为同名宏）。
// GNSS_ENABLE_<D> 是 0/1 归一化结果，本文件与 Registry.cpp 的驱动表共用。
// NOLINTBEGIN(cppcoreguidelines-macro-usage): 0/1 构建开关须参与 #if，constexpr 不能用于预处理
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
// NOLINTEND(cppcoreguidelines-macro-usage)

// 只包含已启用驱动的头（未启用驱动的 .cpp 不参与构建，其单例也就不存在）。
#if GNSS_ENABLE_UNICORE
#include "gnss/drivers/UnicoreReceiver.hpp"
#endif
#if GNSS_ENABLE_UBLOX
#include "gnss/drivers/UbloxReceiver.hpp"
#endif
#if GNSS_ENABLE_COMNAV
#include "gnss/drivers/ComnavReceiver.hpp"
#endif

namespace gnss::detail {

#if GNSS_ENABLE_UNICORE
// NOLINTNEXTLINE(readability-identifier-naming): 外部链接只读单例沿用 k 前缀；s_ 只用于文件作用域静态
extern const Um980Receiver kUm980Receiver; ///< UM980/UM981 叶子单例（定义在 UnicoreReceiver.cpp）。
// NOLINTNEXTLINE(readability-identifier-naming): 同上
extern const Um982Receiver kUm982Receiver; ///< UM982 双天线叶子单例（同文件）。
#endif
#if GNSS_ENABLE_UBLOX
// NOLINTNEXTLINE(readability-identifier-naming): 同上
extern const ZedF9pReceiver kZedF9pReceiver; ///< ZED-F9P 叶子单例（定义在 UbloxReceiver.cpp）。
#endif
#if GNSS_ENABLE_COMNAV
// NOLINTNEXTLINE(readability-identifier-naming): 同上
extern const K803Receiver kK803Receiver; ///< K803 叶子单例（定义在 ComnavReceiver.cpp）。
#endif

} // namespace gnss::detail
