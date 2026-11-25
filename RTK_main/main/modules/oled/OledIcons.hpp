// OLED 自绘单色位图图标（列主序垂直字节格式，适配 WouoUI_CanvasDrawBMP，零版权素材）。
// 全部为 constexpr 只读常量表（落 .rodata Flash，0 DRAM / 0 IRAM），线程/ISR 安全。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstdint>

namespace app::oled::icons {

// 每个字节表示 8 行垂直像素（bit0=顶行，bit7=底行），按列从左到右排列。

/** 8x8 卫星图标（状态栏左端）。 */
inline constexpr std::array<uint8_t, 8U> kSat8x8 = {
    0x06U, 0x0DU, 0x1AU, 0x34U, 0x2CU, 0x58U, 0xB0U, 0x60U,
};

/** 8x8 蓝牙待机图标（空心/细线符文）。 */
inline constexpr std::array<uint8_t, 8U> kBtWait8x8 = {
    0x00U, 0x24U, 0x18U, 0xFFU, 0x99U, 0x66U, 0x00U, 0x00U,
};

/** 8x8 蓝牙已连接图标（两侧带连接点）。 */
inline constexpr std::array<uint8_t, 8U> kBtConn8x8 = {
    0x18U, 0x24U, 0x18U, 0xFFU, 0x99U, 0x66U, 0x00U, 0x18U,
};

/** 8x8 Wi-Fi 扇形波图标。 */
inline constexpr std::array<uint8_t, 8U> kWifi8x8 = {
    0x0CU, 0x12U, 0x29U, 0x45U, 0x45U, 0x29U, 0x12U, 0x0CU,
};

/** 7x8 告警三角感叹号图标（自检有失败项时显示在状态栏）。 */
inline constexpr std::array<uint8_t, 7U> kWarn7x8 = {
    0x60U, 0x58U, 0x46U, 0x5FU, 0x46U, 0x58U, 0x60U,
};

/** 5x8 充电闪电图标（电量旁）。 */
inline constexpr std::array<uint8_t, 5U> kBolt5x8 = {
    0x18U, 0x1CU, 0x7EU, 0x38U, 0x18U,
};

/** 7x8 自检通过勾选图标（✓）。 */
inline constexpr std::array<uint8_t, 7U> kCheck7x8 = {
    0x10U, 0x20U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U,
};

/** 7x8 自检失败叉号图标（✗）。 */
inline constexpr std::array<uint8_t, 7U> kCross7x8 = {
    0x42U, 0x24U, 0x18U, 0x18U, 0x24U, 0x42U, 0x00U,
};

/** 7x8 缺省/缺失横杠图标（-，充电芯片无应答不算失败）。 */
inline constexpr std::array<uint8_t, 7U> kDash7x8 = {
    0x00U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x00U,
};

// ---- 表盘底部 4 个 14x14（按 16 px = 2 页存储）自绘入口图标 ----

/** 14x14 卫星图标（表盘入口 0：轨道与太阳能板）。 */
inline constexpr std::array<uint8_t, 28U> kDockSat14x14 = {
    // 页 0（y=0..7）
    0x00U,
    0x0CU,
    0x1EU,
    0x3CU,
    0x18U,
    0x70U,
    0xE0U,
    0xC0U,
    0x40U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    // 页 1（y=8..13）
    0x00U,
    0x00U,
    0x10U,
    0x20U,
    0x20U,
    0x14U,
    0x0BU,
    0x03U,
    0x07U,
    0x0EU,
    0x1EU,
    0x3CU,
    0x18U,
    0x00U,
};

/** 14x14 位置准星/坐标针图标（表盘入口 1）。 */
inline constexpr std::array<uint8_t, 28U> kDockPos14x14 = {
    // 页 0（y=0..7）
    0x00U,
    0x80U,
    0xF0U,
    0x98U,
    0x8CU,
    0x84U,
    0xFFU,
    0xFFU,
    0x84U,
    0x8CU,
    0x98U,
    0xF0U,
    0x80U,
    0x00U,
    // 页 1（y=8..13）
    0x00U,
    0x00U,
    0x03U,
    0x06U,
    0x0CU,
    0x08U,
    0x3FU,
    0x3FU,
    0x08U,
    0x0CU,
    0x06U,
    0x03U,
    0x00U,
    0x00U,
};

/** 14x14 设备信息芯片/清单图标（表盘入口 2）。 */
inline constexpr std::array<uint8_t, 28U> kDockDev14x14 = {
    // 页 0（y=0..7）
    0x00U,
    0x00U,
    0xFCU,
    0x04U,
    0x24U,
    0x24U,
    0xE4U,
    0x24U,
    0x24U,
    0x24U,
    0x04U,
    0xFCU,
    0x00U,
    0x00U,
    // 页 1（y=8..13）
    0x00U,
    0x00U,
    0x0FU,
    0x08U,
    0x0AU,
    0x0AU,
    0x0BU,
    0x0AU,
    0x0AU,
    0x0AU,
    0x08U,
    0x0FU,
    0x00U,
    0x00U,
};

/** 14x14 电源电池/闪电图标（表盘入口 3）。 */
inline constexpr std::array<uint8_t, 28U> kDockPwr14x14 = {
    // 页 0（y=0..7）
    0x00U,
    0x00U,
    0x00U,
    0xF8U,
    0x08U,
    0x4EU,
    0x62U,
    0x7EU,
    0x32U,
    0x0EU,
    0x08U,
    0xF8U,
    0x00U,
    0x00U,
    // 页 1（y=8..13）
    0x00U,
    0x00U,
    0x00U,
    0x1FU,
    0x10U,
    0x10U,
    0x17U,
    0x13U,
    0x11U,
    0x10U,
    0x10U,
    0x1FU,
    0x00U,
    0x00U,
};

/** 32 点单位圆定点偏移表（半径 15 px，顺时针从 12 点开始，用于关机进度环，0 浮点）。 */
struct RingPoint {
    int8_t dx;
    int8_t dy;
};

inline constexpr std::array<RingPoint, 32U> kRingPts32 = {{
    {0, -15}, {3, -15},  {6, -14},  {8, -12},  {11, -11},  {12, -8},  {14, -6},  {15, -3},
    {15, 0},  {15, 3},   {14, 6},   {12, 8},   {11, 11},   {8, 12},   {6, 14},   {3, 15},
    {0, 15},  {-3, 15},  {-6, 14},  {-8, 12},  {-11, 11},  {-12, 8},  {-14, 6},  {-15, 3},
    {-15, 0}, {-15, -3}, {-14, -6}, {-12, -8}, {-11, -11}, {-8, -12}, {-6, -14}, {-3, -15},
}};

} // namespace app::oled::icons
