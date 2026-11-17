// 系统命令内部契约：状态打印组与共用常量，只在 modules/system/*.cpp 之间共享。
// 常量是编译期值；kibTenths 与 cmdStatus* 由 SystemStatusCmds.cpp 定义，console REPL 任务调用。

#pragma once

#include <cstddef>
#include <cstdint>

#include "AppConfig.hpp"

namespace app::detail {

/** 1 KiB 的字节数。 */
inline constexpr uint64_t kBytesPerKib = 1024ULL;
/** 十分之一单位换算（打印时避免小数点）。 */
inline constexpr uint64_t kTenthsPerUnit = 10ULL;
/** 微秒/毫秒换算。 */
inline constexpr uint64_t kUsPerMs = 1000ULL;
/** 毫秒/秒换算。 */
inline constexpr uint64_t kMsPerSec = 1000ULL;
/** 芯片修订号除以该值得到主版本。 */
inline constexpr uint16_t kChipRevDivisor = 100U;
/** 重启命令等待控制台排空的毫秒数。 */
inline constexpr uint32_t kRebootDelayMs = 100U;
/** log level 子命令的参数个数。 */
inline constexpr size_t kLogLevelArgs = 3U;
/** info 命令里核心名缓冲长度。 */
inline constexpr size_t kCoreTextLen = 16U;
#if RTK_GNSS_ENABLED
/** GNSS 静态观测预留字节数（打印用）。 */
inline constexpr uint32_t kStaticObsReservedBytes = 16384U;
#endif

/** @brief 把字节数换算成 KiB 的十分之一
 * @param bytes: 字节数
 * @retval 以 0.1 KiB 为单位的整数
 * @note 纯函数，任意任务可调用。
 */
uint64_t kibTenths(uint64_t bytes) noexcept;

/** @brief 打印整机状态概览（link → rtcm → gnss → sys 顺序）
 * @param ctx: cli 上下文（未用）
 * @param argc: 参数个数
 * @param argv: 参数
 * @retval 0 表示成功
 */
int cmdStatusOverview(void *ctx, int argc, char **argv) noexcept;

/** @brief 打印 link 段状态 */
int cmdStatusLink(void *ctx, int argc, char **argv) noexcept;

/** @brief 打印 rtcm 段状态 */
int cmdStatusRtcm(void *ctx, int argc, char **argv) noexcept;

/** @brief 打印 gnss 段状态 */
int cmdStatusGnss(void *ctx, int argc, char **argv) noexcept;

/** @brief 打印 sys 段状态 */
int cmdStatusSys(void *ctx, int argc, char **argv) noexcept;

/** @brief 打印全部段状态 */
int cmdStatusAll(void *ctx, int argc, char **argv) noexcept;

} // namespace app::detail
