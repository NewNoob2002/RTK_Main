// u-blox UBX 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：
// B5 62 | class | id | len u16 LE（偏移 4）| 载荷 | CK_A CK_B（8 位 Fletcher，覆盖 class..载荷）。
// 早拒绝点：第 2 字节不是 0x62；长度字段使总帧长超过 kMaxFrame；Fletcher 校验错。
// 长度一确定就一次要求整段剩余字节，校验在帧尾整段算。纯函数、可重入、无静态可变状态；ISR 安全。
//
// Generated-by: pi + workbuddy/deepseek-v4.1-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "rtk/demux/decoders/ubx.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/demux/decoder.hpp"

namespace rtk::demux::ubx {

namespace {

constexpr uint8_t kSyncA = 0xB5U;
constexpr uint8_t kSyncB = 0x62U;
constexpr std::size_t kSyncLen = 2U;
constexpr std::size_t kHdrLen = 6U; // 同步 2 + class + id + len 2
constexpr std::size_t kCkLen = 2U;
constexpr std::size_t kOfsLen = 4U;
constexpr std::size_t kU16Bytes = 2U;
constexpr std::size_t kBytesToLenField = kOfsLen + kU16Bytes;

/** 小端 u16；调用方保证 from + 2 <= frame.size()。 */
uint32_t read_u16_le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    return (static_cast<uint32_t>(frame[from]) | (static_cast<uint32_t>(frame[from + 1U]) << 8U));
}

} // namespace

bool first_byte(uint8_t byte) noexcept
{
    return byte == kSyncA;
}

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 first_byte。
    if (frame.size() < kSyncLen) {
        need = kSyncLen - frame.size();
        return Step::kNeedMore;
    }
    if (frame[1] != kSyncB) {
        return Step::kReject;
    }
    if (frame.size() < kHdrLen) {
        need = kHdrLen - frame.size();
        return Step::kNeedMore;
    }

    const uint32_t payload_len = read_u16_le(frame, kOfsLen);
    // payload_len ≤ 0xFFFF，加固定部分后仍在 size_t 范围内。
    const std::size_t total = kHdrLen + static_cast<std::size_t>(payload_len) + kCkLen;
    if (total > kMaxFrame) {
        // 长度字段声称超过 max_frame：早拒绝，让同首字节的其他解码器尝试。
        return Step::kReject;
    }
    if (frame.size() < total) {
        need = total - frame.size();
        return Step::kNeedMore;
    }
    if (frame.size() > total) {
        // 分流器在 kFrame 后立即复位，正常不会出现；防御性拒绝而非按超长下标读。
        return Step::kReject;
    }

    // 8 位 Fletcher：CK_A 累加字节、CK_B 累加 CK_A，均模 256（uint8_t 自然截断），覆盖 class..载荷。
    uint8_t ck_a = 0U;
    uint8_t ck_b = 0U;
    const std::size_t ck_from = kSyncLen;
    const std::size_t ck_end = total - kCkLen;
    for (std::size_t idx = ck_from; idx < ck_end; ++idx) {
        // 模 256 截断是本算法定义，窄化安全。
        ck_a = static_cast<uint8_t>(ck_a + frame[idx]);
        ck_b = static_cast<uint8_t>(ck_b + ck_a);
    }

    if ((ck_a != frame[total - 2U]) || (ck_b != frame[total - 1U])) {
        return Step::kReject;
    }
    return Step::kFrame;
}

} // namespace rtk::demux::ubx
