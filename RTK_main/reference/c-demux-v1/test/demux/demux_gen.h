// 分流器测试帧生成辅助接口（独立位运算 CRC 实现，黑盒测试用）。
// Generated-by: pi + antigravity/gemini-3.8-flash，人工负责人：UNKNOWN（待人工审查签字）
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 协议帧上限常量（与 ADR-0004 §4 一致） */
#define DEMUX_GEN_RTCM3_MAX_PAYLOAD       (1023U)
#define DEMUX_GEN_RTCM3_MAX_FRAME         (1029U)
#define DEMUX_GEN_NMEA_MAX_FRAME          (256U)
#define DEMUX_GEN_UBX_MAX_FRAME           (4096U)
#define DEMUX_GEN_UBX_MAX_PAYLOAD         (4088U)
#define DEMUX_GEN_UNICORE_BIN_MAX_FRAME   (4096U)
#define DEMUX_GEN_UNICORE_BIN_MAX_PAYLOAD (4068U)
#define DEMUX_GEN_UNICORE_HASH_MAX_FRAME  (1024U)

/** CRC-24Q（多项式 0x1864CFB，初值 0，无取反）；返回低 24 位。 */
uint32_t demux_gen_crc24q(const uint8_t *data, size_t len);

/** 标准 IEEE CRC-32（反射多项式 0xEDB88320，初值 0xFFFFFFFF，末位取反）。 */
uint32_t demux_gen_crc32_ieee(const uint8_t *data, size_t len);

/** NovAtel / Unicore CRC-32（反射多项式 0xEDB88320，初值 0，不取反）。 */
uint32_t demux_gen_crc32_novatel(const uint8_t *data, size_t len);

/** 逐字节异或校验。 */
uint8_t demux_gen_xor(const uint8_t *data, size_t len);

/** 8 位 Fletcher 校验（UBX CK_A/CK_B）。 */
void demux_gen_ubx_ck(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b);

/** 生成合法 RTCM3 帧；payload_len <= 1023，payload_len == 0 时 payload 可为 NULL。 */
bool demux_gen_rtcm3(uint8_t *out, size_t out_cap, const uint8_t *payload, size_t payload_len, size_t *out_len);

/** 生成合法 NMEA 0183 语句；body 为 '$' 与 '*' 之间的内容（含首字符 [A-Z0-9]）。 */
bool demux_gen_nmea(uint8_t *out, size_t out_cap, const char *body, bool lowercase_hex, size_t *out_len);

/** 生成合法 u-blox UBX 帧；payload_len <= 4088，payload_len == 0 时 payload 可为 NULL。 */
bool demux_gen_ubx(uint8_t *out, size_t out_cap, uint8_t msg_class, uint8_t msg_id, const uint8_t *payload,
                   size_t payload_len, size_t *out_len);

/** 生成合法 Unicore 二进制帧；hdr_rest_16 可为 NULL（填 0），payload_len <= 4068。 */
bool demux_gen_unicore_bin(uint8_t *out, size_t out_cap, uint16_t msg_id, uint8_t cpu_idle, const uint8_t *hdr_rest_16,
                           const uint8_t *payload, size_t payload_len, size_t *out_len);

/** 生成合法 Unicore Hash 语句（8 位十六进制 CRC-32）；body 为 '#' 与 '*' 之间的内容。 */
bool demux_gen_unicore_hash_crc32(uint8_t *out, size_t out_cap, const char *body, bool lowercase_hex, size_t *out_len);

/** 生成合法 Unicore Hash 语句（2 位十六进制 XOR）；body 为 '#' 与 '*' 之间的内容。 */
bool demux_gen_unicore_hash_xor(uint8_t *out, size_t out_cap, const char *body, bool lowercase_hex, size_t *out_len);

/** 生成合法产品 SPP 帧（内部调用 rtk_proto_frame_build）。 */
bool demux_gen_product(uint8_t *out, size_t out_cap, uint16_t msg_id, uint8_t sender, uint8_t type,
                       const uint8_t *payload, uint16_t payload_len, size_t *out_len);
