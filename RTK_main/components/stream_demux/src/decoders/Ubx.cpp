// u-blox UBX 解码器（ADR-0004 §4 判定规则 / ADR-0005 §1 实现位置）：B5 62 | class | id | len u16 LE（偏移 4）|
// 载荷 | CK_A CK_B（8 位 Fletcher，覆盖 class..载荷）；第 2 字节错、长度使总长超 kMaxFrame、校验错均早拒绝。
// 纯函数、无静态可变状态、无堆、不阻塞：可重入、ISR 安全；长度一确定就要整段剩余字节，校验在帧尾整段算。

#include "demux/decoders/Ubx.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/TextWriter.hpp"
#include "demux/Decoder.hpp"

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

/** judge() 的结论：判定、原因、待补字节数与诊断数值。 */
struct Verdict {
    Step step = Step::kNeedMore;
    Reason reason = Reason::kNone;
    std::size_t need = 1U;
    std::size_t total = 0U;
    uint32_t received = 0U; // 校验类原因：收到的 CK_A | CK_B<<8
    uint32_t computed = 0U; // kSync：期望的同步字节
};

/**
 * @brief  构造一个 kReject 结论
 * @param  reason: 拒绝原因
 * @retval 结论（step=kReject，need 保持默认 1，诊断数值由调用方补写）
 */
Verdict reject(Reason reason) noexcept
{
    Verdict verdict{};
    verdict.step = Step::kReject;
    verdict.reason = reason;
    return verdict;
}

/**
 * @brief  读取小端 u16
 * @param  frame: 候选前缀
 * @param  from: 低位字节下标；调用方保证 from + 2 ≤ frame.size()
 * @retval 组合出的 16 位值
 */
uint32_t readU16Le(std::span<const uint8_t> frame, std::size_t from) noexcept
{
    return (static_cast<uint32_t>(frame[from]) | (static_cast<uint32_t>(frame[from + 1U]) << 8U));
}

// NOLINTBEGIN(readability-function-size): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
/**
 * @brief  判定一帧：同步字节、长度、Fletcher 校验
 * @param  frame: 候选前缀，非空且 frame[0] 已通过 firstByte
 * @retval step=kNeedMore 还不能下结论，need 为至少还要的字节数（长度未知时按头部补齐）
 * @retval step=kFrame 长度与 8 位 Fletcher 校验都通过
 * @retval step=kReject 同步字节错（kSync）、长度超 kMaxFrame（kTooLong）、比声明长度长（kExtra）
 *         或校验不符（kChecksum）
 * @note   不拆（ADR-0013 §2.4）：协议早拒绝阶梯，sync/长度/校验的先后次序本身就是协议语义，
 *         拆成多个函数只会让“先拒绝什么”更难看懂。
 */
Verdict judge(std::span<const uint8_t> frame) noexcept
{
    Verdict verdict{};
    if (frame[0] != kSyncA) {
        Verdict bad = reject(Reason::kSync);
        bad.received = frame[0];
        bad.computed = kSyncA;
        return bad;
    }
    if (frame.size() < kSyncLen) {
        verdict.need = kSyncLen - frame.size();
        return verdict;
    }
    if (frame[1] != kSyncB) {
        Verdict bad = reject(Reason::kSync);
        bad.received = frame[1];
        bad.computed = kSyncB;
        return bad;
    }
    if (frame.size() < kHdrLen) {
        verdict.need = kHdrLen - frame.size();
        return verdict;
    }

    const uint32_t payloadLen = readU16Le(frame, kOfsLen);
    // payloadLen ≤ 0xFFFF，加固定部分后仍在 size_t 范围内。
    const std::size_t total = kHdrLen + static_cast<std::size_t>(payloadLen) + kCkLen;
    if (total > kMaxFrame) {
        // 长度字段声称超过 maxFrame：早拒绝，让同首字节的其他解码器尝试。
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
    uint8_t ckA = 0U;
    uint8_t ckB = 0U;
    const std::size_t ckFrom = kSyncLen;
    const std::size_t ckEnd = total - kCkLen;
    for (std::size_t idx = ckFrom; idx < ckEnd; ++idx) {
        // 模 256 截断是本算法定义，窄化安全。
        ckA = static_cast<uint8_t>(ckA + frame[idx]);
        ckB = static_cast<uint8_t>(ckB + ckA);
    }

    const uint32_t ckRx =
        static_cast<uint32_t>(frame[total - 2U]) | (static_cast<uint32_t>(frame[total - 1U]) << 8U);
    const uint32_t ckCalc = static_cast<uint32_t>(ckA) | (static_cast<uint32_t>(ckB) << 8U);
    if (ckRx != ckCalc) {
        Verdict bad = reject(Reason::kChecksum);
        bad.received = ckRx;
        bad.computed = ckCalc;
        return bad;
    }
    Verdict good{};
    good.step = Step::kFrame;
    return good;
}
// NOLINTEND(readability-function-size)

/**
 * @brief  把判定理由写成一句人读文本
 * @param  verdict: 判定结果
 * @param[out] text: 文本输出器（空间不足时截断）
 * @note   纯 switch 的文本映射，每个 case 独立无状态。
 */
void explainVerdict(const Verdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case Reason::kSync:
        // 期望值区分是第 1 还是第 2 个同步字节。
        text.literal(verdict.computed == kSyncA ? "sync byte 1 is " : "sync byte 2 is ")
            .hex(verdict.received, 2U)
            .literal(", expected ")
            .hex(verdict.computed, 2U);
        break;
    case Reason::kTooLong:
        text.literal("length ")
            .decimal(verdict.total)
            .literal(" exceeds ")
            .decimal(kMaxFrame)
            .literal(" bytes");
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

bool firstByte(uint8_t byte) noexcept
{
    return byte == kSyncA;
}

Step step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    // 契约：调用方保证 frame.size() >= 1 且 frame[0] 已通过 firstByte。
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
    explainVerdict(verdict, text);
    return text.finish();
}

} // namespace demux::ubx
