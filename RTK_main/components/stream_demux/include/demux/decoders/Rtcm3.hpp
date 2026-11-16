// RTCM 3.x：D3 | 6 bit 保留 = 0 | 10 bit 长度 | 载荷 | CRC-24Q（多项式 0x1864CFB，初值 0，覆盖头 3 B + 载荷，3 B
// 大端）。早拒绝：第 2 字节高 6 位非 0。 实现：src/decoders/Rtcm3.cpp。构建开关：IDF 下 CONFIG_STREAM_DEMUX_RTCM3，独立
// CMake 下 STREAM_DEMUX_RTCM3。
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"

namespace demux::rtcm3 {

inline constexpr std::size_t kMaxFrame = 1029;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool first_byte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 demux/Decoder.hpp。 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/** 线程/ISR 安全：是（纯函数）。对被 step 判为 kReject 的前缀写一句英文原因，见 demux/Decoder.hpp ExplainFn。 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr Decoder kDecoder{"RTCM3", kMaxFrame, &first_byte, &step, &explain};

} // namespace demux::rtcm3
