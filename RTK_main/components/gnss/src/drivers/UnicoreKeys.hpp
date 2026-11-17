// Unicore N4 驱动的共享字面量（组件私有头）：$command/$CONFIG 应答与 #VERSIONA 探测的前缀、帧尾与超时。
// 线程/调用约束：只含 constexpr 常量，无状态、无副作用；任何 TU 均可包含。
// 堆：不使用。浮点：不使用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../AsciiCmd.hpp"

namespace gnss::unicore {

inline constexpr uint8_t kDollar = static_cast<uint8_t>('$');
inline constexpr uint8_t kStar = static_cast<uint8_t>(ascii::kStar);
inline constexpr uint8_t kCr = static_cast<uint8_t>(ascii::kCr);
inline constexpr uint8_t kLf = static_cast<uint8_t>(ascii::kLf);
inline constexpr std::string_view kCommandPrefix = "$command,";
// 读回行（ADR-0012 §2.7）："$CONFIG,<port>,<config>*hh\r\n"，与 $command 应答同为 NMEA 式帧，仅前缀不同。
inline constexpr std::string_view kConfigPrefix = "$CONFIG,";
inline constexpr std::string_view kResponseTag = ",response: ";
inline constexpr std::string_view kVersionTag = "#VERSIONA,";
/** 前缀之后的最小应答字节数（",OK" 一类），不足则继续等待。 */
inline constexpr std::size_t kMinReplyBytes = 6U;
/** 应答尾 "*hh\r\n" 的总字节数，即 '*' 距帧尾的偏移（沿用旧实现，ADR-0010）。 */
inline constexpr std::size_t kStarFromEnd = ascii::kTailBytes;
/** 单步配置命令的应答超时。 */
inline constexpr uint32_t kConfigTimeoutMs = 500U;

} // namespace gnss::unicore
