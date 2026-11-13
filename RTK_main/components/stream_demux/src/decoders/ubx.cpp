// u-blox UBX 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：
// B5 62 | class | id | len u16 LE（偏移 4）| 载荷 | CK_A CK_B（8 位 Fletcher，覆盖 class..载荷）。
// 早拒绝点：第 2 字节不是 0x62；长度字段使总帧长超过 kMaxFrame；Fletcher 校验错。
// 长度一确定就一次要求整段剩余字节，校验在帧尾整段算。纯函数、可重入、无静态可变状态；ISR 安全。
// step 与 explain 共用 judge()。

#include "demux/decoders/ubx.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/text_writer.hpp"
#include "demux/decoder.hpp"

namespace demux::ubx {

namespace {

constexpr uint8_t kSyncA = 0xB5U;
constexpr uint8_t kSyncB = 0x62U;
constexpr std::size_t kSyncLen = 2U;
constexpr std::size_t kHdrLen = 6U; // 同步 2 + class + id + len 2
constexpr std::size_t kCkLen = 2U;
constexpr std::size_t kOfsLen = 4U;

/** 拒绝原因；explain 据此生成一句话。 */
enum class Reason : uint8_t {
    kNone,
    kSync,
    kTooLong,
    kExtra,
    kChecksum,
};

struct Verdict {
    Step step = Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;
    std::size_t total = 0U;
    uint32_t received = 0U; // 校验类原因：收到的 CK_A | CK_B<<8
    uint32_t computed = 0U;
};

Verdict reject(Reason reason) noexcept
{
    Verdict verdict{};
    verdict.step = Step::kReject;
    verdict.reason = reason;
    return verdict;
}

/** 小端 u16；调用方保证 from + 2 <= frame.size()。 */
uint32_t read_u16_le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    return (static_cast<uint32_t>(frame[from]) | (static_cast<uint32_t>(frame[from + 1U]) << 8U));
}

Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    if (frame[0] != kSyncA) {
        return reject(Reason::kSync);
    }
    if (frame.size() < kSyncLen) {
        verdict.need = kSyncLen - frame.size();
        return verdict;
    }
    if (frame[1] != kSyncB) {
        Verdict bad = reject(Reason::kSync);
        bad.received = frame[1];
        return bad;
    }
    if (frame.size() < kHdrLen) {
        verdict.need = kHdrLen - frame.size();
        return verdict;
    }

    const uint32_t payload_len = read_u16_le(frame, kOfsLen);
    // payload_len ≤ 0xFFFF，加固定部分后仍在 size_t 范围内。
    const std::size_t total = kHdrLen + static_cast<std::size_t>(payload_len) + kCkLen;
    if (total > kMaxFrame) {
        // 长度字段声称超过 max_frame：早拒绝，让同首字节的其他解码器尝试。
        Verdict bad = reject(Reason::kTooLong);
        bad.total = total;
        return bad;
    }
    if (frame.size() < total) {
        verdict.need = total - frame.size();
        return verdict;
    }
    if (frame.size() > total) {
        // 分流器在 kFrame 后立即复位，正常不会出现；防御性拒绝而非按超长下标读。
        Verdict bad = reject(Reason::kExtra);
        bad.total = total;
        return bad;
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

    const uint32_t ck_rx = static_cast<uint32_t>(frame[total - 2U]) | (static_cast<uint32_t>(frame[total - 1U]) << 8U);
    const uint32_t ck_calc = static_cast<uint32_t>(ck_a) | (static_cast<uint32_t>(ck_b) << 8U);
    if (ck_rx != ck_calc) {
        Verdict bad = reject(Reason::kChecksum);
        bad.received = ck_rx;
        bad.computed = ck_calc;
        return bad;
    }
    Verdict good{};
    good.step = Step::kFrame;
    return good;
}

void explain_verdict(const Verdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case Reason::kSync:
        text.literal("sync byte 2 is ").hex(verdict.received, 2U).literal(", expected 0x62");
        break;
    case Reason::kTooLong:
        text.literal("length ").decimal(verdict.total).literal(" exceeds ").decimal(kMaxFrame).literal(" bytes");
        break;
    case Reason::kExtra:
        text.literal("frame longer than the declared ").decimal(verdict.total).literal(" bytes");
        break;
    case Reason::kChecksum:
        text.literal("bad checksum, received ")
            .hex(verdict.received, 4U)
            .literal(", computed ")
            .hex(verdict.computed, 4U);
        break;
    case Reason::kNone:
    default:
        text.literal("rejected");
        break;
    }
}

} // namespace

bool first_byte(uint8_t byte) noexcept
{
    return byte == kSyncA;
}

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 first_byte。
    const Verdict verdict = judge(frame);
    need = verdict.need;
    return verdict.step;
}

std::size_t explain(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    demux::text_writer::TextWriter text(out);
    if (frame.empty()) {
        return text.literal("empty prefix").finish();
    }
    const Verdict verdict = judge(frame);
    if (verdict.step == Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explain_verdict(verdict, text);
    return text.finish();
}

} // namespace demux::ubx
