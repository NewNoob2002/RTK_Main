// 产品 SPP 帧（AA 44 18）的 rtk_demux 解码器与线路帧 → 字段视图（ADR-0004 §4、§7）。
// 判定规则与 rtk_proto_frame.c 的 rtk_proto_parser_feed 完全一致，只是改写为“整段纯函数 step”：
// 同步 AA 44 18、头长字节 == 0x14、payload_len ≤ RTK_PROTO_MAX_PAYLOAD、标准 CRC-32（复用公开的
// rtk_proto_crc32()，不另建实现）。早拒绝点：第 2、3 字节、头长字节、payload 长度字段；长度已知后
// 一次要求整段剩余字节，分流器整块拷贝后只再调用一次 step，CRC 在帧尾整段计算。
// 与 Unicore 二进制（AA 44 B5）共享前两个字节，靠第 3 字节早拒绝区分（ADR-0004 §2）。
// 线程安全：是（不写任何静态状态，可被多个实例并发调用）；ISR 安全：是（无阻塞、无分配）。
//
// Generated-by: pi + DeepSeek-V4.1-Flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "rtk_proto_demux.h"

#define RTK_PROTO_PRIV_BYTE_BITS (8U)
#define RTK_PROTO_PRIV_U16_BYTES (2U)
#define RTK_PROTO_PRIV_U32_BYTES (4U)

/* 同步头长度（AA 44 18）与两个“字段收完”的位置：头长字节之后、payload 长度之后。 */
#define RTK_PROTO_PRIV_SYNC_LEN    (3U)
#define RTK_PROTO_PRIV_SYNC_B_LEN  (2U)
#define RTK_PROTO_PRIV_POS_HDR_LEN (RTK_PROTO_OFS_HDR_LEN + 1U)
#define RTK_PROTO_PRIV_POS_MSG_LEN (RTK_PROTO_OFS_MSG_LEN + RTK_PROTO_PRIV_U16_BYTES)

/* 线路帧中 CRC 固定占最后 4 B，此处按头文件常量派生。 */
#define RTK_PROTO_PRIV_OFS_CRC(len) ((len) - (size_t)RTK_PROTO_CRC_LEN)

static uint32_t read_u16_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < RTK_PROTO_PRIV_U16_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (RTK_PROTO_PRIV_BYTE_BITS * idx);
    }
    return value;
}

static uint32_t read_u32_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < RTK_PROTO_PRIV_U32_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (RTK_PROTO_PRIV_BYTE_BITS * idx);
    }
    return value;
}

static bool proto_first_byte(uint8_t byte)
{
    return byte == RTK_PROTO_SYNC_A;
}

/* step 契约：只在 len ≥ 1 且 frame[0] 通过 first_byte 时调用；返回 NEED_MORE 时 1 ≤ *need 且
 * len + *need ≤ max_frame。 */
static rtk_demux_step_t proto_step(const uint8_t *frame, size_t len, size_t *need)
{
    uint32_t payload_len = 0U;
    size_t total = 0U;
    uint32_t crc_calc = 0U;
    uint32_t crc_rx = 0U;

    if (len < RTK_PROTO_PRIV_U16_BYTES) {
        *need = (size_t)RTK_PROTO_PRIV_U16_BYTES - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[1] != RTK_PROTO_SYNC_B) {
        /* 与 NovAtel/Unicore 等其它 AA 开头协议在这里分开 */
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < (size_t)RTK_PROTO_PRIV_SYNC_LEN) {
        *need = (size_t)RTK_PROTO_PRIV_SYNC_LEN - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[2] != RTK_PROTO_SYNC_C) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < (size_t)RTK_PROTO_PRIV_POS_HDR_LEN) {
        *need = (size_t)RTK_PROTO_PRIV_POS_HDR_LEN - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[RTK_PROTO_OFS_HDR_LEN] != (uint8_t)RTK_PROTO_HEADER_LEN) {
        /* RTK_PROTO_HEADER_LEN == 20 < 256，窄化安全 */
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < (size_t)RTK_PROTO_PRIV_POS_MSG_LEN) {
        *need = (size_t)RTK_PROTO_PRIV_POS_MSG_LEN - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }

    payload_len = read_u16_le(&frame[RTK_PROTO_OFS_MSG_LEN]);
    if (payload_len > RTK_PROTO_MAX_PAYLOAD) {
        return RTK_DEMUX_STEP_REJECT;
    }
    /* 20 + 256 + 4 = 280 = max_frame，size_t 加法不溢出 */
    total = (size_t)RTK_PROTO_HEADER_LEN + (size_t)payload_len + (size_t)RTK_PROTO_CRC_LEN;

    if (len < total) {
        *need = total - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (len > total) {
        /* 分流器在 FRAME 后立即复位，正常不会到这里；防御性拒绝而非越界读 */
        return RTK_DEMUX_STEP_REJECT;
    }

    crc_calc = rtk_proto_crc32(0U, frame, RTK_PROTO_PRIV_OFS_CRC(total));
    crc_rx = read_u32_le(&frame[RTK_PROTO_PRIV_OFS_CRC(total)]);

    return (crc_calc == crc_rx) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

const rtk_demux_decoder_t rtk_proto_demux_decoder = {
    .name = "proto",
    .proto_id = RTK_PROTO_DEMUX_ID,
    .max_frame = RTK_PROTO_MAX_FRAME,
    .first_byte = proto_first_byte,
    .step = proto_step,
};

bool rtk_proto_frame_view(const uint8_t *wire, size_t len, rtk_proto_frame_t *out)
{
    uint32_t payload_len = 0U;
    size_t expected = 0U;

    if ((wire == NULL) || (out == NULL)) {
        return false;
    }
    if (len < ((size_t)RTK_PROTO_HEADER_LEN + (size_t)RTK_PROTO_CRC_LEN)) {
        return false;
    }
    if ((wire[0] != RTK_PROTO_SYNC_A) || (wire[1] != RTK_PROTO_SYNC_B) || (wire[2] != RTK_PROTO_SYNC_C)) {
        return false;
    }
    if (wire[RTK_PROTO_OFS_HDR_LEN] != (uint8_t)RTK_PROTO_HEADER_LEN) {
        return false;
    }

    payload_len = read_u16_le(&wire[RTK_PROTO_OFS_MSG_LEN]);
    if (payload_len > RTK_PROTO_MAX_PAYLOAD) {
        return false;
    }
    expected = (size_t)RTK_PROTO_HEADER_LEN + (size_t)payload_len + (size_t)RTK_PROTO_CRC_LEN;
    if (len != expected) {
        /* 长度字段与线路帧实际长度不一致：调用方给的 len 不是这一帧 */
        return false;
    }

    out->msg_id = (uint16_t)read_u16_le(&wire[RTK_PROTO_OFS_MSG_ID]); /* 16 bit 字段，截断安全 */
    out->sender = wire[RTK_PROTO_OFS_SENDER];
    out->type = wire[RTK_PROTO_OFS_TYPE];
    out->protocol = wire[RTK_PROTO_OFS_PROTOCOL];
    out->interval = wire[RTK_PROTO_OFS_INTERVAL];
    out->payload_len = (uint16_t)payload_len; /* ≤ 256，截断安全 */
    out->payload = &wire[RTK_PROTO_HEADER_LEN];
    return true;
}
