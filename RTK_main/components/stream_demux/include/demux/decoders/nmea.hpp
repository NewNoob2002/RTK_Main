// NMEA 0183：$ | 第 2 字节 [A-Z0-9] | 正文可打印 ASCII 0x20–0x7E 且不含 $ | * 两位十六进制 XOR（$ 与 *
// 之间，大小写均可）| \r\n。非法字符立即拒绝。 实现：src/decoders/nmea.cpp。构建开关：IDF 下
// CONFIG_STREAM_DEMUX_NMEA，独立 CMake 下 STREAM_DEMUX_NMEA。
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoder.hpp"

namespace demux::nmea {

inline constexpr std::size_t kMaxFrame = 256;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool first_byte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 demux/decoder.hpp。 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/** 线程/ISR 安全：是（纯函数）。对被 step 判为 kReject 的前缀写一句英文原因，见 demux/decoder.hpp ExplainFn。 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr Decoder kDecoder{"NMEA", kMaxFrame, &first_byte, &step, &explain};

} // namespace demux::nmea
