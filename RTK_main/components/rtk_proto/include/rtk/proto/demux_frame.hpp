// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN（待人工审查签字）
//
// 产品 SPP 帧（AA 44 18，见 rtk_proto_frame.h 与 docs/protocol/phone_protocol.md §1–§3）的分流器解码器，
// 供 stream_demux 在同一条蓝牙字节流中区分产品帧与 RTCM（ADR-0004 方案 B）。
// 校验规则与 rtk_proto_parser_feed 完全一致：AA 44 18、头长字节 == 0x14、payload_len（偏移 12，u16 LE）
// ≤ RTK_PROTO_MAX_PAYLOAD、CRC-32（rtk::proto::crc32）正确。实现：components/rtk_proto/demux_frame.cpp。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "rtk/demux/decoder.hpp"
#include "rtk/proto/frame.hpp"
#include "rtk_proto_frame.h"

namespace rtk::proto::demux_frame {

inline constexpr std::size_t kMaxFrame = RTK_PROTO_MAX_FRAME;

/** 线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] bool first_byte(uint8_t byte) noexcept;

/** 线程/ISR 安全：是（纯函数）。契约见 rtk/demux/decoder.hpp。 */
[[nodiscard]] demux::Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept;

inline constexpr demux::Decoder kDecoder{"RtkFrame", kMaxFrame, &first_byte, &step};

/**
 * 把一条已被 kDecoder 判定为 kFrame 的线路帧解成字段视图（payload 指向 wire 内部，生命周期同 wire）。
 * 同步字节、头长、长度一致性不满足时返回 std::nullopt；不重复校验 CRC。线程/ISR 安全：是。
 */
[[nodiscard]] std::optional<FrameView> view(std::span<const uint8_t> wire) noexcept;

} // namespace rtk::proto::demux_frame
