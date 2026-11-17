// demux/Crc.hpp 的实现（ADR-0005 §1）：解码器共用的 CRC-24Q 与 NovAtel/Unicore 风格 CRC-32。
// 两张 256 项表由 constexpr 函数在编译期生成（任务卡要求不得手抄），放只读存储：各 1 KB。
// 逐位算法与查表算法等价，核对值 "123456789" → 0xCDE703（CRC-24Q）/ 0x2DFD2D88（CRC-32 反射），
// 已用独立的 Python 逐位实现交叉核对（见 docs/reports/H2.md §3）。
// 无静态可变状态、无浮点、无堆；两个函数都是纯函数，可对同一前缀重复调用。

#include "demux/Crc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace demux::crc {

namespace {

// CRC-24Q（RTCM 10403.3 §3.3，等价 CRC-24/LTE-A）：poly 0x1864CFB，非反射，init 0，无最终异或。
constexpr uint32_t kCrc24qPoly = 0x1864CFBU;
constexpr uint32_t kCrc24qTopBit = 0x800000U;
constexpr uint32_t kCrc24qMask = 0xFFFFFFU;
// NovAtel/Unicore 风格 CRC-32：反射 poly 0xEDB88320，init 0，无最终异或（≠ zlib 的 crc32）。
constexpr uint32_t kCrc32Poly = 0xEDB88320U;
constexpr std::size_t kTableSize = 256U;
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kBitsPerByte = 8U;

/** 单字节表的第 index 项：i << 16 对多项式迭代 8 次（非反射，结果 < 2^24）。 */
constexpr uint32_t crc24qEntry(uint32_t index) noexcept
{
    // index < 256，左移 16 位后 < 2^24，uint32_t 内不溢出。
    uint32_t crc = index << 16U;

    for (uint32_t bit = 0U; bit < kBitsPerByte; ++bit) {
        if ((crc & kCrc24qTopBit) != 0U) {
            crc = ((crc << 1U) ^ kCrc24qPoly) & kCrc24qMask;
        } else {
            crc = (crc << 1U) & kCrc24qMask;
        }
    }
    return crc;
}

/** 单字节表的第 index 项：i 对反射多项式迭代 8 次。 */
constexpr uint32_t crc32Entry(uint32_t index) noexcept
{
    uint32_t crc = index;

    for (uint32_t bit = 0U; bit < kBitsPerByte; ++bit) {
        if ((crc & 1U) != 0U) {
            crc = (crc >> 1U) ^ kCrc32Poly;
        } else {
            crc >>= 1U;
        }
    }
    return crc;
}

constexpr std::array<uint32_t, kTableSize> makeCrc24qTable() noexcept
{
    std::array<uint32_t, kTableSize> table{};

    for (std::size_t idx = 0U; idx < kTableSize; ++idx) {
        // idx < 256，窄化到 uint32_t 安全（表项生成只需要 8 位下标）。
        table[idx] = crc24qEntry(static_cast<uint32_t>(idx));
    }
    return table;
}

constexpr std::array<uint32_t, kTableSize> makeCrc32Table() noexcept
{
    std::array<uint32_t, kTableSize> table{};

    for (std::size_t idx = 0U; idx < kTableSize; ++idx) {
        // 同上：idx < 256，窄化安全。
        table[idx] = crc32Entry(static_cast<uint32_t>(idx));
    }
    return table;
}

// 编译期生成的两个表；static 给出内部链接，内容进 .rodata（只读，ISR 安全）。
static constexpr std::array<uint32_t, kTableSize> kCrc24qTable = makeCrc24qTable();
static constexpr std::array<uint32_t, kTableSize> kCrc32Table = makeCrc32Table();

} // namespace

uint32_t crc24q(std::span<const uint8_t> data, uint32_t crc) noexcept
{
    // 只保留低 24 位：调用方传入的分段初值同样按 CRC-24Q 的寄存器宽度截断。
    uint32_t value = crc & kCrc24qMask;

    for (const uint8_t byte : data) {
        // 非反射：寄存器高 8 位与数据字节异或作表下标，下标 < 256。
        const uint32_t index = ((value >> 16U) ^ static_cast<uint32_t>(byte)) & kByteMask;

        value = ((value << kBitsPerByte) & kCrc24qMask) ^ kCrc24qTable[index];
    }
    return value;
}

uint32_t crc32Novatel(std::span<const uint8_t> data, uint32_t crc) noexcept
{
    uint32_t value = crc;

    for (const uint8_t byte : data) {
        // 反射：寄存器低 8 位与数据字节异或作表下标，下标 < 256。
        const uint32_t index = (value ^ static_cast<uint32_t>(byte)) & kByteMask;

        value = (value >> kBitsPerByte) ^ kCrc32Table[index];
    }
    return value;
}

} // namespace demux::crc
