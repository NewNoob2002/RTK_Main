// 解码器共用的 CRC（实现：src/crc.cpp，256 项 constexpr 查找表放只读存储）。可分段累计：
// f(b, f(a, init)) == f(a || b, init)。线程/ISR 安全：是。
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
#pragma once

#include <cstdint>
#include <span>

namespace rtk::demux::crc {

/** CRC-24Q（RTCM3）：多项式 0x1864CFB，非反射，初值 0，无最终异或；结果低 24 位有效。"123456789" → 0xCDE703。 */
[[nodiscard]] uint32_t crc24q(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

/**
 * NovAtel/Unicore CRC-32：反射多项式 0xEDB88320，初值 0，无最终异或（≠ zlib 的 crc32）。
 * "123456789" → 0x2DFD2D88。
 */
[[nodiscard]] uint32_t crc32_novatel(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

} // namespace rtk::demux::crc
