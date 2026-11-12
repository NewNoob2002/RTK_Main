// 冻结接口（ADR-0004）。实现方不得修改；变更须先修订 ADR-0004。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN
//
// 内置的通用 GNSS 协议解码器描述符（纯 C11，无平台依赖）。每个都是无状态的 const 对象，
// 可同时挂到多个 rtk_demux_t 实例。proto_id 取下列 RTK_DEMUX_PROTO_* 值；使用方自定义协议
// （如产品帧，见 rtk_proto_demux.h）从 RTK_DEMUX_PROTO_USER 起编号。
// 校验规则与帧格式的出处见 docs/adr/ADR-0004-stream-demux.md §4。
#pragma once

#include "rtk_demux.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTK_DEMUX_PROTO_RTCM3 (1U)
#define RTK_DEMUX_PROTO_NMEA (2U)
#define RTK_DEMUX_PROTO_UBX (3U)
#define RTK_DEMUX_PROTO_UNICORE_BIN (4U)
#define RTK_DEMUX_PROTO_UNICORE_HASH (5U)
#define RTK_DEMUX_PROTO_USER (0x100U)

/** RTCM 3.x：D3 | 6 bit 保留=0 | 10 bit 长度 | 载荷 | CRC-24Q(3 B, 大端)。max_frame = 1029。 */
extern const rtk_demux_decoder_t rtk_demux_dec_rtcm3;

/**
 * NMEA 0183（含厂商专有语句 $P…）：'$' | 地址与字段（可打印 ASCII 0x20..0x7E，不含 '$' '*'）
 * | '*' | 2 位十六进制 XOR 校验（大小写均可） | "\r\n"。max_frame = 256（允许超出标准 82 的长语句）。
 * 第 2 字节必须是大写字母或数字，以便与同以 '$' 开头的其他协议区分。
 */
extern const rtk_demux_decoder_t rtk_demux_dec_nmea;

/** u-blox UBX：B5 62 | class | id | len(u16 LE) | 载荷 | CK_A CK_B（8 位 Fletcher，覆盖 class..载荷）。max_frame = 4096。 */
extern const rtk_demux_decoder_t rtk_demux_dec_ubx;

/**
 * Unicore 二进制：固定 24 B 头部 AA 44 B5 | CPU 空闲%(1) | 消息 ID u16 LE(偏移 4) | 载荷长 u16 LE(偏移 6) | 其余时间字段
 * | 载荷 | CRC-32（4 B LE）。
 * CRC-32 为 IEEE 反射多项式 0xEDB88320、初值 0、不取反的 NovAtel 风格，覆盖头部+载荷。max_frame = 4096。
 */
extern const rtk_demux_decoder_t rtk_demux_dec_unicore_bin;

/**
 * Unicore ASCII（Hash）语句：'#' | 可打印 ASCII（不含 '#' '*' '$'）| '*' | 校验 | "\r\n"。校验两种格式都接受：
 * 8 位十六进制 CRC-32（覆盖 '#' 之后到 '*' 之前，同 Unicore 二进制的 CRC 算法），或 2 位十六进制 XOR 校验
 * （同 NMEA 规则，覆盖 '#' 之后到 '*' 之前）。十六进制大小写均可。第 2 字节必须是大写字母。max_frame = 1024。
 */
extern const rtk_demux_decoder_t rtk_demux_dec_unicore_hash;

#ifdef __cplusplus
}
#endif
