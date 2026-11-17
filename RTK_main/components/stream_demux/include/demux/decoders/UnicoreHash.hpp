// Unicore Hash 语句解码器（冻结接口 ADR-0005）：# | 第 2 字节 [A-Z] | 正文可打印 ASCII 且不含 # $ * |
// * 后 8 位十六进制 CRC-32（同 unicore_bin 算法，覆盖 # 与 * 之间）或 2 位十六进制 XOR | \r\n。
// 纯函数、无可变状态、不阻塞、无堆：线程与 ISR 安全；开关 CONFIG_STREAM_DEMUX_UNICORE_HASH / STREAM_DEMUX_UNICORE_HASH。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"

namespace demux::unicore_hash {

inline constexpr std::size_t kMaxFrame = 4096;

/**
 * @brief  该字节能否作为 Hash 语句的首字节
 * @param  byte: 待判定的首字节
 * @retval true 是 '#'（0x23）；false 其余
 * @note   纯函数；Demux::init 时对 0..255 各调用一次。
 */
[[nodiscard]] bool firstByte(uint8_t byte) noexcept;

/**
 * @brief  判定候选前缀：完整 / 还需字节 / 拒绝
 * @param  frame: 从首字节起的候选前缀，至少 1 字节
 * @param[out] need: 写入判定给出的待补字节数；只有 kNeedMore 时有意义
 * @retval kNeedMore 还不能下结论，按 need 继续累积（语句长度未知，need 恒为 1）
 * @retval kFrame 前缀恰好是一条完整且 XOR 或 CRC-32 校验通过的语句
 * @retval kReject 不是 Hash 语句（同步/第 2 字节/正文/校验字段/校验值失败或超长）
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

inline constexpr Decoder kDecoder{"UnicoreHash", kMaxFrame, &firstByte, &step, &explain};

} // namespace demux::unicore_hash
