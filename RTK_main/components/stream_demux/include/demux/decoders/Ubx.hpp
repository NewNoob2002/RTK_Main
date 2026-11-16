// u-blox UBX：B5 62 | class | id | len u16 LE | 载荷 | CK_A CK_B（8 位 Fletcher，覆盖 class..载荷）。早拒绝：第 2
// 字节；长度使总帧长 > kMaxFrame。 实现：src/decoders/Ubx.cpp。构建开关：IDF 下 CONFIG_STREAM_DEMUX_UBX，独立 CMake 下
// STREAM_DEMUX_UBX。
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"

namespace demux::ubx {

inline constexpr std::size_t kMaxFrame = 4096;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool first_byte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 demux/Decoder.hpp。 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/** 线程/ISR 安全：是（纯函数）。对被 step 判为 kReject 的前缀写一句英文原因，见 demux/Decoder.hpp ExplainFn。 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr Decoder kDecoder{"UBX", kMaxFrame, &first_byte, &step, &explain};

} // namespace demux::ubx
