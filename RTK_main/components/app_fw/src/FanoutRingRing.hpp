// FanoutRing 的环形几何与定长头读写：读写两个实现文件（FanoutRing.cpp / FanoutRingRead.cpp）共用，
// 故放在组件内部私有头（不进 include/，不对外）。全部是纯函数，无副作用；cap 必须是 2 的幂（由 init 校验）。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/FanoutRing.hpp"

namespace fw::detail {

inline constexpr size_t kBitsPerByte = 8U;

/**
 * @brief  逻辑序号转物理下标（u32 回绕安全）
 * @param  oldest: 环首记录的逻辑序号
 * @param  cap: 环容量（2 的幂）
 * @param  pos: 目标记录的逻辑序号
 * @retval [0, cap) 内的物理下标
 */
inline size_t ringPhys(uint32_t oldest, size_t cap, uint32_t pos) noexcept
{
    const size_t base =
        static_cast<size_t>(oldest) % cap; // uint32_t 扩宽至 size_t 无损（32/64 位平台）
    const size_t offset = static_cast<size_t>(pos - oldest);
    return (base + offset) % cap;
}

/**
 * @brief  从环上读一段逻辑区间（跨环尾时折返）
 * @param  ring: 整个环缓冲
 * @param  oldest: 环首记录的逻辑序号
 * @param  pos: 起始逻辑序号
 * @param  dst: 目标缓冲，长度即读取字节数
 */
inline void ringRead(std::span<const uint8_t> ring, uint32_t oldest, uint32_t pos,
                     std::span<uint8_t> dst) noexcept
{
    if (dst.empty()) {
        return;
    }
    const size_t cap = ring.size();
    const size_t idx = ringPhys(oldest, cap, pos);
    const size_t first = std::min(dst.size(), cap - idx);
    const std::span<const uint8_t> s0 = ring.subspan(idx, first);
    std::copy(s0.begin(), s0.end(), dst.begin());
    if (first < dst.size()) {
        const std::span<const uint8_t> s1 = ring.first(dst.size() - first);
        const std::span<uint8_t> d1 = dst.subspan(first);
        std::copy(s1.begin(), s1.end(), d1.begin());
    }
}

/**
 * @brief  读取记录头里的 16 位小端负载长度
 * @param  ring: 整个环缓冲
 * @param  oldest: 环首记录的逻辑序号
 * @param  pos: 记录头起始逻辑序号
 * @retval 负载长度（0..65535）
 */
inline uint16_t readHeaderLen(std::span<const uint8_t> ring, uint32_t oldest, uint32_t pos) noexcept
{
    std::array<uint8_t, kFanoutHeaderBytes> hdr{};
    ringRead(ring, oldest, pos, hdr);
    const uint16_t lo = static_cast<uint16_t>(hdr[0]);
    const uint16_t hi = static_cast<uint16_t>(static_cast<uint16_t>(hdr[1]) << kBitsPerByte);
    return static_cast<uint16_t>(lo | hi); // hdr[0..1] 均为 uint8_t，结果在 0..65535
}

} // namespace fw::detail
