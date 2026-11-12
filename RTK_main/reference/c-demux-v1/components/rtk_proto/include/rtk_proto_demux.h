// 冻结接口（ADR-0004）。实现方不得修改；变更须先修订 ADR-0004。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN
//
// 产品 SPP 帧（AA 44 18，见 rtk_proto_frame.h 与 docs/protocol/phone_protocol.md §1–§3）的分流器解码器描述符，
// 供 rtk_demux 在同一条蓝牙字节流中区分产品帧与 RTCM（ADR-0004 方案 B）。
// 校验规则与 rtk_proto_parser_feed 完全一致：头长字节 == 0x14、payload_len ≤ RTK_PROTO_MAX_PAYLOAD、CRC-32 正确。
// FRAME 回调得到的是完整线路帧（含头与 CRC），需要字段时用 rtk_proto_frame_view() 解出。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rtk_demux.h"
#include "rtk_proto_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 产品帧的 proto_id（RTK_DEMUX_PROTO_USER + 0）。 */
#define RTK_PROTO_DEMUX_ID (0x100U)

/** 产品帧解码器描述符；max_frame = RTK_PROTO_MAX_FRAME。 */
extern const rtk_demux_decoder_t rtk_proto_demux_decoder;

/**
 * 把一条已由 rtk_proto_demux_decoder 判定为 FRAME 的线路帧解成字段视图（payload 指向 wire 内部）。
 * wire/len 不满足帧格式（同步、头长、长度一致性）时返回 false 且不写 out；不重复校验 CRC。
 */
bool rtk_proto_frame_view(const uint8_t *wire, size_t len, rtk_proto_frame_t *out);

#ifdef __cplusplus
}
#endif
