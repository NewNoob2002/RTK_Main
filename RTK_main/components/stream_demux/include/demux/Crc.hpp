// 解码器共用的 CRC（实现 src/Crc.cpp，256 项 constexpr 查找表在只读存储）。可分段累计：
// f(b, f(a, init)) == f(a || b, init)；冻结接口 ADR-0005，实现方不得修改，变更须先修订该 ADR。
// 纯函数、无全局可变状态、不阻塞、无堆：线程与 ISR 安全。
#pragma once

#include <cstdint>
#include <span>

namespace demux::crc {

/**
 * @brief  计算 CRC-24Q（RTCM3 用）
 * @param  data: 待校验字节；为空时原样返回 crc
 * @param  crc: 续算初值（上一段的累计值），默认 0 表示从头开始
 * @retval 校验值，低 24 位有效；多项式 0x1864CFB、非反射、初值 0、无最终异或；"123456789" → 0xCDE703
 * @note   纯函数、查表实现；不阻塞、ISR 安全。
 */
[[nodiscard]] uint32_t crc24q(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

/**
 * @brief  计算 NovAtel/Unicore 用 CRC-32
 * @param  data: 待校验字节；为空时原样返回 crc
 * @param  crc: 续算初值（上一段的累计值），默认 0 表示从头开始
 * @retval 校验值；反射多项式 0xEDB88320、初值 0、无最终异或（≠ zlib 的 crc32）；"123456789" → 0x2DFD2D88
 * @note   纯函数、查表实现；不阻塞、ISR 安全。
 */
[[nodiscard]] uint32_t crc32Novatel(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

} // namespace demux::crc
