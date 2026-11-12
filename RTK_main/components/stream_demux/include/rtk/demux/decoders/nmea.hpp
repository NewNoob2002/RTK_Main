// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN（待人工审查签字）
//
// NMEA 0183：$ | 第 2 字节 [A-Z0-9] | 正文可打印 ASCII 0x20–0x7E 且不含 $ | * 两位十六进制 XOR（$ 与 *
// 之间，大小写均可）| \r\n。非法字符立即拒绝。 实现：src/decoders/nmea.cpp。构建开关：IDF 下
// CONFIG_STREAM_DEMUX_NMEA，独立 CMake 下 STREAM_DEMUX_NMEA。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/demux/decoder.hpp"

namespace rtk::demux::nmea {

inline constexpr std::size_t kMaxFrame = 256;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool first_byte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 rtk/demux/decoder.hpp。 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

inline constexpr Decoder kDecoder{"NMEA", kMaxFrame, &first_byte, &step};

} // namespace rtk::demux::nmea
