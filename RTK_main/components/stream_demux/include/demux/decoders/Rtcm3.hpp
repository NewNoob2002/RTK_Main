// RTCM 3.x 解码器（冻结接口 ADR-0005）：D3 | 6 bit 保留 = 0 | 10 bit 长度 | 载荷 | CRC-24Q（0x1864CFB，
// 3 B 大端，覆盖头 3 B + 载荷）；第 2 字节高 6 位非 0 即早拒绝。实现 src/decoders/Rtcm3.cpp。
// 纯函数、无可变状态、不阻塞、无堆：线程与 ISR 安全；构建开关 CONFIG_STREAM_DEMUX_RTCM3 / STREAM_DEMUX_RTCM3。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"

namespace demux::rtcm3 {

inline constexpr std::size_t kMaxFrame = 1029;

/**
 * @brief  该字节能否作为 RTCM3 帧的首字节
 * @param  byte: 待判定的首字节
 * @retval true 是 0xD3；false 其余
 * @note   纯函数；Demux::init 时对 0..255 各调用一次。
 */
[[nodiscard]] bool firstByte(uint8_t byte) noexcept;

/**
 * @brief  判定候选前缀：完整 / 还需字节 / 拒绝
 * @param  frame: 从首字节起的候选前缀，至少 1 字节
 * @param[out] need: 写入判定给出的待补字节数；只有 kNeedMore 时有意义
 * @retval kNeedMore 还不能下结论，按 need 继续累积（长度字段一到就要求整段剩余字节）
 * @retval kFrame 前缀恰好是一条完整且 CRC-24Q 通过的帧
 * @retval kReject 不是 RTCM3 帧（同步/保留位/长度不符/CRC 失败）
 * @note   纯函数，契约见 demux/Decoder.hpp；不阻塞、无堆，ISR 安全。
 */
[[nodiscard]] Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/**
 * @brief  对被 step 判为 kReject 的前缀写一句英文原因（诊断用）
 * @param  frame: 被拒绝的前缀；空时写 "empty prefix"
 * @param[out] out: 文本缓冲，空间不足时截断
 * @retval 写入长度（不含结尾 0）
 * @note   纯函数；引擎从不调用，只由 Sink 的诊断代码调用；不阻塞、无堆，ISR 安全。
 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr Decoder kDecoder{"RTCM3", kMaxFrame, &firstByte, &step, &explain};

} // namespace demux::rtcm3
