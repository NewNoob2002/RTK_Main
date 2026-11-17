// Unicore Hash 语句：# | 第 2 字节 [A-Z] | 正文可打印 ASCII 且不含 # $ | * 后 8 位十六进制 CRC-32（同 unicore_bin
// 算法，覆盖 # 与 * 之间）或 2 位十六进制 XOR | \r\n。非法字符立即拒绝。
// 实现：src/decoders/UnicoreHash.cpp。构建开关：IDF 下 CONFIG_STREAM_DEMUX_UNICORE_HASH，独立 CMake 下
// STREAM_DEMUX_UNICORE_HASH。
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"

namespace demux::unicore_hash {

inline constexpr std::size_t kMaxFrame = 4096;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool firstByte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 demux/Decoder.hpp。 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/** 线程/ISR 安全：是（纯函数）。对被 step 判为 kReject 的前缀写一句英文原因，见 demux/Decoder.hpp ExplainFn。 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr Decoder kDecoder{"UnicoreHash", kMaxFrame, &firstByte, &step, &explain};

} // namespace demux::unicore_hash
