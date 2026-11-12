// 分流器测试帧生成辅助实现（独立位运算 CRC，不共享被测代码表）。
// Generated-by: pi + antigravity/gemini-3.8-flash，人工负责人：UNKNOWN（待人工审查签字）
#include "demux_gen.h"

#include <string.h>

#include "rtk_proto_frame.h"

#define CRC24Q_POLY     (0x1864CFBU)
#define CRC24Q_TOP_BIT  (0x1000000U)
#define CRC24Q_MASK     (0xFFFFFFU)
#define CRC32_REFL_POLY (0xEDB88320U)
#define CRC32_INIT_IEEE (0xFFFFFFFFU)
#define BITS_PER_BYTE   (8U)

#define RTCM3_SYNC     (0xD3U)
#define RTCM3_HDR_LEN  (3U)
#define RTCM3_CRC_LEN  (3U)
#define RTCM3_OVERHEAD (RTCM3_HDR_LEN + RTCM3_CRC_LEN)

#define UBX_SYNC1    (0xB5U)
#define UBX_SYNC2    (0x62U)
#define UBX_HDR_LEN  (6U)
#define UBX_CK_LEN   (2U)
#define UBX_OVERHEAD (UBX_HDR_LEN + UBX_CK_LEN)

#define UNICORE_SYNC1    (0xAAU)
#define UNICORE_SYNC2    (0x44U)
#define UNICORE_SYNC3    (0xB5U)
#define UNICORE_HDR_LEN  (24U)
#define UNICORE_HDR_REST (16U)
#define UNICORE_CRC_LEN  (4U)
#define UNICORE_OVERHEAD (UNICORE_HDR_LEN + UNICORE_CRC_LEN)

#define ASCII_PRINT_MIN  (0x20U)
#define ASCII_PRINT_MAX  (0x7EU)
#define NMEA_TRAILER_LEN (5U)  /* *XX\r\n */
#define HASH_CRC_TRAILER (11U) /* *XXXXXXXX\r\n */

static char nibble_to_hex(uint8_t nibble, bool lowercase)
{
    static const char s_upper[] = "0123456789ABCDEF";
    static const char s_lower[] = "0123456789abcdef";
    uint8_t idx = (uint8_t)(nibble & 0x0FU); /* 掩码后范围为 0..15 */

    return lowercase ? s_lower[idx] : s_upper[idx];
}

static bool is_upper_alpha(uint8_t ch)
{
    return (ch >= (uint8_t)'A') && (ch <= (uint8_t)'Z');
}

static bool is_digit_char(uint8_t ch)
{
    return (ch >= (uint8_t)'0') && (ch <= (uint8_t)'9');
}

static bool is_printable_body_char(uint8_t ch, bool forbid_hash)
{
    if ((ch < ASCII_PRINT_MIN) || (ch > ASCII_PRINT_MAX)) {
        return false;
    }
    if ((ch == (uint8_t)'$') || (ch == (uint8_t)'*')) {
        return false;
    }
    if (forbid_hash && (ch == (uint8_t)'#')) {
        return false;
    }
    return true;
}

static void write_u16_le(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);         /* 低字节在 0..255 */
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU); /* 高字节右移后在 0..255 */
}

static void write_u32_le(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);          /* 字节 0 */
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);  /* 字节 1 */
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU); /* 字节 2 */
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU); /* 字节 3 */
}

uint32_t demux_gen_crc24q(const uint8_t *data, size_t len)
{
    uint32_t crc = 0U;

    if ((data == NULL) || (len == 0U)) {
        return 0U;
    }
    for (size_t i = 0U; i < len; i++) {
        crc ^= ((uint32_t)data[i]) << 16U;
        for (uint32_t bit = 0U; bit < BITS_PER_BYTE; bit++) {
            crc <<= 1U;
            if ((crc & CRC24Q_TOP_BIT) != 0U) {
                crc ^= CRC24Q_POLY;
            }
        }
        crc &= CRC24Q_MASK;
    }
    return crc & CRC24Q_MASK;
}

static uint32_t crc32_reflected_raw(uint32_t init, const uint8_t *data, size_t len)
{
    uint32_t crc = init;

    for (size_t i = 0U; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint32_t bit = 0U; bit < BITS_PER_BYTE; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ CRC32_REFL_POLY;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

uint32_t demux_gen_crc32_ieee(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U)) {
        return 0U;
    }
    return crc32_reflected_raw(CRC32_INIT_IEEE, data, len) ^ CRC32_INIT_IEEE;
}

uint32_t demux_gen_crc32_novatel(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U)) {
        return 0U;
    }
    return crc32_reflected_raw(0U, data, len);
}

uint8_t demux_gen_xor(const uint8_t *data, size_t len)
{
    uint8_t acc = 0U;

    if ((data == NULL) || (len == 0U)) {
        return 0U;
    }
    for (size_t i = 0U; i < len; i++) {
        acc ^= data[i];
    }
    return acc;
}

void demux_gen_ubx_ck(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b)
{
    uint8_t a = 0U;
    uint8_t b = 0U;

    if ((ck_a == NULL) || (ck_b == NULL)) {
        return;
    }
    if ((data != NULL) && (len > 0U)) {
        for (size_t i = 0U; i < len; i++) {
            a = (uint8_t)(a + data[i]); /* 8 位模 256 累加 */
            b = (uint8_t)(b + a);       /* 8 位模 256 累加 */
        }
    }
    *ck_a = a;
    *ck_b = b;
}

bool demux_gen_rtcm3(uint8_t *out, size_t out_cap, const uint8_t *payload, size_t payload_len, size_t *out_len)
{
    size_t total = 0U;
    uint32_t crc = 0U;

    if ((out == NULL) || (out_len == NULL) || (payload_len > DEMUX_GEN_RTCM3_MAX_PAYLOAD)) {
        return false;
    }
    if ((payload_len > 0U) && (payload == NULL)) {
        return false;
    }
    total = RTCM3_OVERHEAD + payload_len;
    if (out_cap < total) {
        return false;
    }

    out[0] = RTCM3_SYNC;
    out[1] = (uint8_t)((payload_len >> 8U) & 0x03U); /* payload_len <= 1023，高 6 位固定为 0 */
    out[2] = (uint8_t)(payload_len & 0xFFU);         /* 低 8 位 */
    if (payload_len > 0U) {
        memcpy(&out[RTCM3_HDR_LEN], payload, payload_len);
    }

    crc = demux_gen_crc24q(out, RTCM3_HDR_LEN + payload_len);
    out[RTCM3_HDR_LEN + payload_len] = (uint8_t)((crc >> 16U) & 0xFFU);     /* 大端高字节 */
    out[RTCM3_HDR_LEN + payload_len + 1U] = (uint8_t)((crc >> 8U) & 0xFFU); /* 大端中字节 */
    out[RTCM3_HDR_LEN + payload_len + 2U] = (uint8_t)(crc & 0xFFU);         /* 大端低字节 */
    *out_len = total;
    return true;
}

bool demux_gen_nmea(uint8_t *out, size_t out_cap, const char *body, bool lowercase_hex, size_t *out_len)
{
    size_t body_len = 0U;
    size_t total = 0U;
    uint8_t chk = 0U;

    if ((out == NULL) || (out_len == NULL) || (body == NULL)) {
        return false;
    }
    body_len = strlen(body);
    if ((body_len == 0U) || (body_len > (DEMUX_GEN_NMEA_MAX_FRAME - 1U - NMEA_TRAILER_LEN))) {
        return false;
    }
    if (!is_upper_alpha((uint8_t)body[0]) && !is_digit_char((uint8_t)body[0])) {
        return false;
    }
    for (size_t i = 0U; i < body_len; i++) {
        if (!is_printable_body_char((uint8_t)body[i], false)) {
            return false;
        }
    }
    total = 1U + body_len + NMEA_TRAILER_LEN;
    if (out_cap < total) {
        return false;
    }

    out[0] = (uint8_t)'$';
    memcpy(&out[1], body, body_len);
    chk = demux_gen_xor(&out[1], body_len);
    out[1U + body_len] = (uint8_t)'*';
    out[1U + body_len + 1U] = (uint8_t)nibble_to_hex((uint8_t)(chk >> 4U), lowercase_hex);
    out[1U + body_len + 2U] = (uint8_t)nibble_to_hex(chk, lowercase_hex);
    out[1U + body_len + 3U] = (uint8_t)'\r';
    out[1U + body_len + 4U] = (uint8_t)'\n';
    *out_len = total;
    return true;
}

bool demux_gen_ubx(uint8_t *out, size_t out_cap, uint8_t msg_class, uint8_t msg_id, const uint8_t *payload,
                   size_t payload_len, size_t *out_len)
{
    size_t total = 0U;
    uint8_t ck_a = 0U;
    uint8_t ck_b = 0U;

    if ((out == NULL) || (out_len == NULL) || (payload_len > DEMUX_GEN_UBX_MAX_PAYLOAD)) {
        return false;
    }
    if ((payload_len > 0U) && (payload == NULL)) {
        return false;
    }
    total = UBX_OVERHEAD + payload_len;
    if (out_cap < total) {
        return false;
    }

    out[0] = UBX_SYNC1;
    out[1] = UBX_SYNC2;
    out[2] = msg_class;
    out[3] = msg_id;
    write_u16_le(&out[4], (uint16_t)payload_len); /* 已校验 payload_len <= 4088 */
    if (payload_len > 0U) {
        memcpy(&out[UBX_HDR_LEN], payload, payload_len);
    }
    demux_gen_ubx_ck(&out[2], 4U + payload_len, &ck_a, &ck_b);
    out[UBX_HDR_LEN + payload_len] = ck_a;
    out[UBX_HDR_LEN + payload_len + 1U] = ck_b;
    *out_len = total;
    return true;
}

bool demux_gen_unicore_bin(uint8_t *out, size_t out_cap, uint16_t msg_id, uint8_t cpu_idle, const uint8_t *hdr_rest_16,
                           const uint8_t *payload, size_t payload_len, size_t *out_len)
{
    size_t total = 0U;
    uint32_t crc = 0U;

    if ((out == NULL) || (out_len == NULL) || (payload_len > DEMUX_GEN_UNICORE_BIN_MAX_PAYLOAD)) {
        return false;
    }
    if ((payload_len > 0U) && (payload == NULL)) {
        return false;
    }
    total = UNICORE_OVERHEAD + payload_len;
    if (out_cap < total) {
        return false;
    }

    out[0] = UNICORE_SYNC1;
    out[1] = UNICORE_SYNC2;
    out[2] = UNICORE_SYNC3;
    out[3] = cpu_idle;
    write_u16_le(&out[4], msg_id);
    write_u16_le(&out[6], (uint16_t)payload_len); /* 已校验 payload_len <= 4068 */
    if (hdr_rest_16 != NULL) {
        memcpy(&out[8], hdr_rest_16, UNICORE_HDR_REST);
    } else {
        memset(&out[8], 0, UNICORE_HDR_REST);
    }
    if (payload_len > 0U) {
        memcpy(&out[UNICORE_HDR_LEN], payload, payload_len);
    }
    crc = demux_gen_crc32_novatel(out, UNICORE_HDR_LEN + payload_len);
    write_u32_le(&out[UNICORE_HDR_LEN + payload_len], crc);
    *out_len = total;
    return true;
}

static bool validate_unicore_hash_body(const char *body, size_t body_len, size_t max_body)
{
    if ((body_len == 0U) || (body_len > max_body)) {
        return false;
    }
    if (!is_upper_alpha((uint8_t)body[0])) {
        return false;
    }
    for (size_t i = 0U; i < body_len; i++) {
        if (!is_printable_body_char((uint8_t)body[i], true)) {
            return false;
        }
    }
    return true;
}

bool demux_gen_unicore_hash_crc32(uint8_t *out, size_t out_cap, const char *body, bool lowercase_hex, size_t *out_len)
{
    size_t body_len = 0U;
    size_t total = 0U;
    uint32_t crc = 0U;

    if ((out == NULL) || (out_len == NULL) || (body == NULL)) {
        return false;
    }
    body_len = strlen(body);
    if (!validate_unicore_hash_body(body, body_len, DEMUX_GEN_UNICORE_HASH_MAX_FRAME - 1U - HASH_CRC_TRAILER)) {
        return false;
    }
    total = 1U + body_len + HASH_CRC_TRAILER;
    if (out_cap < total) {
        return false;
    }

    out[0] = (uint8_t)'#';
    memcpy(&out[1], body, body_len);
    crc = demux_gen_crc32_novatel(&out[1], body_len);
    out[1U + body_len] = (uint8_t)'*';
    for (size_t i = 0U; i < 8U; i++) {
        uint32_t shift = (7U - (uint32_t)i) * 4U; /* i in 0..7，位移在 28..0 */
        uint8_t nib = (uint8_t)((crc >> shift) & 0x0FU);
        out[2U + body_len + i] = (uint8_t)nibble_to_hex(nib, lowercase_hex);
    }
    out[1U + body_len + 9U] = (uint8_t)'\r';
    out[1U + body_len + 10U] = (uint8_t)'\n';
    *out_len = total;
    return true;
}

bool demux_gen_unicore_hash_xor(uint8_t *out, size_t out_cap, const char *body, bool lowercase_hex, size_t *out_len)
{
    size_t body_len = 0U;
    size_t total = 0U;
    uint8_t chk = 0U;

    if ((out == NULL) || (out_len == NULL) || (body == NULL)) {
        return false;
    }
    body_len = strlen(body);
    if (!validate_unicore_hash_body(body, body_len, DEMUX_GEN_UNICORE_HASH_MAX_FRAME - 1U - NMEA_TRAILER_LEN)) {
        return false;
    }
    total = 1U + body_len + NMEA_TRAILER_LEN;
    if (out_cap < total) {
        return false;
    }

    out[0] = (uint8_t)'#';
    memcpy(&out[1], body, body_len);
    chk = demux_gen_xor(&out[1], body_len);
    out[1U + body_len] = (uint8_t)'*';
    out[1U + body_len + 1U] = (uint8_t)nibble_to_hex((uint8_t)(chk >> 4U), lowercase_hex);
    out[1U + body_len + 2U] = (uint8_t)nibble_to_hex(chk, lowercase_hex);
    out[1U + body_len + 3U] = (uint8_t)'\r';
    out[1U + body_len + 4U] = (uint8_t)'\n';
    *out_len = total;
    return true;
}

bool demux_gen_product(uint8_t *out, size_t out_cap, uint16_t msg_id, uint8_t sender, uint8_t type,
                       const uint8_t *payload, uint16_t payload_len, size_t *out_len)
{
    return rtk_proto_frame_build(out, out_cap, msg_id, sender, type, payload, payload_len, out_len) == RTK_PROTO_OK;
}
