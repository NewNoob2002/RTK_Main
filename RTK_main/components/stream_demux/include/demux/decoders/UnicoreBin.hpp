// Unicore 二进制：AA 44 B5 | 24 B 固定头，载荷长 u16 LE 在偏移 6 | 载荷 | CRC-32（0xEDB88320 反射、初值
// 0、不取反）覆盖头+载荷，4 B LE。早拒绝：第 2、3 字节；长度超限。 实现：src/decoders/UnicoreBin.cpp。构建开关：IDF 下
// CONFIG_STREAM_DEMUX_UNICORE_BIN，独立 CMake 下 STREAM_DEMUX_UNICORE_BIN。
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"

namespace demux::unicore_bin {

inline constexpr std::size_t kMaxFrame = 4096;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool firstByte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 demux/Decoder.hpp。 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/** 线程/ISR 安全：是（纯函数）。对被 step 判为 kReject 的前缀写一句英文原因，见 demux/Decoder.hpp ExplainFn。 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr Decoder kDecoder{"UnicoreBin", kMaxFrame, &firstByte, &step, &explain};

} // namespace demux::unicore_bin
