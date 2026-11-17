// 产品 SPP 帧（AA 44 18）的 demux::Decoder 描述符，供 streamDemux 区分产品帧与 RTCM（ADR-0004 方案 B、ADR-0005 冻结接口）。
// 判定与 dev_proto_parser_feed 一致：AA 44 18、头长 0x14、payloadLen ≤ DEV_PROTO_MAX_PAYLOAD、CRC-32 正确（实现见 DemuxFrame.cpp）。
// 无状态纯函数，任意任务/ISR 可调用；无阻塞、不使用堆。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "demux/Decoder.hpp"
#include "proto/Frame.hpp"
#include "DevProtoFrame.h"

namespace proto::demux_frame {

inline constexpr std::size_t kMaxFrame = DEV_PROTO_MAX_FRAME;

/**
 * @brief  判断字节是否是产品帧的同步首字节
 * @param  byte: 待检查的字节
 * @retval true  等于 kSyncA（0xAA）
 * @retval false 其他值
 * @note   纯函数，任意任务/ISR 可调用；无阻塞
 */
[[nodiscard]] bool firstByte(uint8_t byte) noexcept;

/**
 * @brief  判定候选前缀：成帧 / 需要更多字节 / 拒绝
 * @param  frame: 从同步字节开始的前缀（调用方保证非空且 frame[0] 已通过 firstByte）
 * @param[out] need: 返回 kNeedMore 时写入还差的字节数（≥1）；kFrame/kReject 时调用方忽略
 * @retval demux::Step::kFrame    前缀恰好是一帧完整且 CRC 正确
 * @retval demux::Step::kNeedMore 还不能下结论，需再补 need 字节
 * @retval demux::Step::kReject   同步/头长/帧长/CRC 不符，不是产品帧
 * @note   契约见 demux/Decoder.hpp；纯函数、可重入，任意任务/ISR 可调用；无阻塞
 */
[[nodiscard]] demux::Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/**
 * @brief  为被 step 判为 kReject 的前缀写一句英文拒绝原因
 * @param  frame: 待解释的前缀（空时写 "empty prefix"）
 * @param[out] out: 输出文本缓冲，内容以 0 结尾（空间不足时截断）
 * @retval 写入的字节数（不含结尾 0）
 * @note   frame 为空写 "empty prefix"；已是完整帧（judge 为 kFrame）写 "not rejected"；
 *         引擎只在拒绝时调用。纯函数，只依赖入参；任意任务/ISR 可调用、无阻塞。
 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr demux::Decoder kDecoder{"RtkFrame", kMaxFrame, &firstByte, &step, &explain};

/**
 * @brief  把已通过 kDecoder 判定的线路帧解成字段视图
 * @param  wire: 完整线路帧（从同步字节到 CRC 末尾）
 * @retval FrameView   全部检查通过时返回；payload 指向 wire 内部
 * @retval std::nullopt 长度不足 24、同步/头长不符、payloadLen > DEV_PROTO_MAX_PAYLOAD，
 *                      或总长与长度字段不一致（wire 不恰好是这一帧）
 * @note   不重复校验 CRC；payload 生命周期同 wire；纯函数、ISR 安全
 */
[[nodiscard]] std::optional<FrameView> view(std::span<const uint8_t> wire) noexcept;

} // namespace proto::demux_frame
