// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN（待人工审查签字）
//
// Unicore Hash 语句：# | 第 2 字节 [A-Z] | 正文可打印 ASCII 且不含 # $ | * 后 8 位十六进制 CRC-32（同 unicore_bin 算法，覆盖 # 与 * 之间）或 2 位十六进制 XOR | \r\n。非法字符立即拒绝。
// 实现：src/decoders/unicore_hash.cpp。构建开关：IDF 下 CONFIG_STREAM_DEMUX_UNICORE_HASH，独立 CMake 下 STREAM_DEMUX_UNICORE_HASH。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoder.hpp"

namespace demux::unicore_hash {

inline constexpr std::size_t kMaxFrame = 1024;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool first_byte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 demux/decoder.hpp。 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

inline constexpr Decoder kDecoder{"UnicoreHash", kMaxFrame, &first_byte, &step};

} // namespace demux::unicore_hash
