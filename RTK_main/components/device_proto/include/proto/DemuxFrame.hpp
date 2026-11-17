// 产品 SPP 帧（AA 44 18，见 DevProtoFrame.h 与 docs/protocol/phone_protocol.md §1–§3）的分流器解码器，
// 供 stream_demux 在同一条蓝牙字节流中区分产品帧与 RTCM（ADR-0004 方案 B）。
// 校验规则与 dev_proto_parser_feed 完全一致：AA 44 18、头长字节 == 0x14、payload_len（偏移 12，u16 LE）
// ≤ DEV_PROTO_MAX_PAYLOAD、CRC-32（proto::crc32）正确。实现：components/device_proto/DemuxFrame.cpp。
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
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

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool firstByte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 demux/Decoder.hpp。 */
[[nodiscard]] demux::Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

/** 线程/ISR 安全：是（纯函数）。对被 step 判为 kReject 的前缀写一句英文原因，见 demux/Decoder.hpp ExplainFn。 */
[[nodiscard]] std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept;

inline constexpr demux::Decoder kDecoder{"RtkFrame", kMaxFrame, &firstByte, &step, &explain};

/**
 * 把一条已被 kDecoder 判定为 kFrame 的线路帧解成字段视图（payload 指向 wire 内部，生命周期同 wire）。
 * 同步字节、头长、长度一致性不满足时返回 std::nullopt；不重复校验 CRC。线程/ISR 安全：是。
 */
[[nodiscard]] std::optional<FrameView> view(std::span<const uint8_t> wire) noexcept;

} // namespace proto::demux_frame
