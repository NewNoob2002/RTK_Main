// Wi-Fi 配置模式强制门户 DNS 报文解析与应答构造实现（RFC 1035 + RFC 6891 EDNS0 OPT）。
// 纯函数模块，由 WebModule 在 kLink 任务调用，不阻塞、不分配堆，线程/ISR 安全。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/web/WebDns.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace app::web_cfg {
namespace {

constexpr size_t kDnsHeaderBytes = 12U;       // B，RFC 1035 固定报头长度
constexpr size_t kDnsQuestionTailBytes = 4U;  // B，QTYPE(2) + QCLASS(2)
constexpr size_t kDnsMinQueryBytes = 17U;     // B，12 B 头 + 1 B 根标签 + 4 B QTYPE/QCLASS
constexpr size_t kDnsAnswerRecordBytes = 16U; // B，指针(2)+TYPE(2)+CLASS(2)+TTL(4)+RDLEN(2)+IPv4(4)
constexpr size_t kDnsOptFixedBytes = 11U;     // B，根名(1)+TYPE(2)+UDP(2)+TTL(4)+RDLEN(2)
constexpr size_t kDnsMaxWireNameBytes = 255U; // B，RFC 1035 域名总长上限
constexpr uint8_t kDnsLabelTypeMask = 0xC0U;  // 高 2 位非 0 表示压缩指针或保留标签
constexpr uint16_t kDnsFlagQr = 0x8000U;      // Query(0) / Response(1)
constexpr uint16_t kDnsFlagOpcodeMask = 0x7800U; // OPCODE 字段掩码（0 = 标准查询）
constexpr uint16_t kDnsFlagAa = 0x0400U;         // Authoritative Answer
constexpr uint16_t kDnsFlagTc = 0x0200U;         // Truncation
constexpr uint16_t kDnsFlagRd = 0x0100U;         // Recursion Desired
constexpr uint16_t kDnsTypeA = 1U;               // IPv4 A 记录
constexpr uint16_t kDnsTypeOpt = 41U;            // RFC 6891 EDNS0 OPT 伪记录
constexpr uint16_t kDnsClassIn = 1U;             // IN 类
constexpr uint16_t kDnsClassAny = 255U;          // ANY 类
constexpr uint16_t kDnsNamePtrHeader = 0xC00CU;  // 指向偏移 12（QNAME 起点）的压缩指针
constexpr uint16_t kDnsIpv4RdLength = 4U;        // B，IPv4 RDATA 长度
constexpr uint16_t kDnsOptUdpPayload = 512U;     // B，应答 OPT 通告的 UDP 载荷上限

// 报头、应答记录与 OPT 伪记录的字段偏移（单位：B）。
constexpr size_t kHdrOffId = 0U;
constexpr size_t kHdrOffFlags = 2U;
constexpr size_t kHdrOffQdCount = 4U;
constexpr size_t kHdrOffAnCount = 6U;
constexpr size_t kHdrOffNsCount = 8U;
constexpr size_t kHdrOffArCount = 10U;
constexpr size_t kAnsOffName = 0U;
constexpr size_t kAnsOffType = 2U;
constexpr size_t kAnsOffClass = 4U;
constexpr size_t kAnsOffTtl = 6U;
constexpr size_t kAnsOffRdLen = 10U;
constexpr size_t kAnsOffRdData = 12U;
constexpr size_t kOptOffName = 0U;
constexpr size_t kOptOffType = 1U;
constexpr size_t kOptOffUdpSize = 3U;
constexpr size_t kOptOffExtFlags = 5U;
constexpr size_t kOptOffRdLen = 9U;

// 大端字节拆装位移与掩码常量。
constexpr uint32_t kByteMask = 0xFFU;
constexpr uint32_t kShiftByte1 = 8U;
constexpr uint32_t kShiftByte2 = 16U;
constexpr uint32_t kShiftByte3 = 24U;
constexpr size_t kOffByte1 = 1U;
constexpr size_t kOffByte2 = 2U;
constexpr size_t kOffByte3 = 3U;

/**
 * @brief 从报文指定偏移按大端读取 16 位无符号整数
 * @param buf: 源报文视图（调用方须已保证 offset + 2 <= buf.size()）
 * @param offset: 起始字节偏移
 * @retval 读取到的主机序 uint16_t
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] uint16_t readBe16(std::span<const uint8_t> buf, size_t offset) noexcept
{
    const uint32_t hi = static_cast<uint32_t>(buf[offset]) << kShiftByte1;
    const uint32_t lo = static_cast<uint32_t>(buf[offset + kOffByte1]);
    // 两个字节拼接结果在 0..65535，窄化到 uint16_t 无损。
    return static_cast<uint16_t>(hi | lo);
}

/**
 * @brief 向输出缓冲指定偏移按大端写入 16 位无符号整数
 * @param[out] buf: 目标缓冲（调用方须已保证 offset + 2 <= buf.size()）
 * @param offset: 起始字节偏移
 * @param value: 待写入的 16 位值
 * @note 纯函数，线程/ISR 安全。
 */
void writeBe16(std::span<uint8_t> buf, size_t offset, uint16_t value) noexcept
{
    // 移位与掩码结果在 0..255，窄化到 uint8_t 安全。
    buf[offset] = static_cast<uint8_t>((value >> kShiftByte1) & kByteMask);
    buf[offset + kOffByte1] = static_cast<uint8_t>(value & kByteMask);
}

/**
 * @brief 向输出缓冲指定偏移按大端写入 32 位无符号整数
 * @param[out] buf: 目标缓冲（调用方须已保证 offset + 4 <= buf.size()）
 * @param offset: 起始字节偏移
 * @param value: 待写入的 32 位值
 * @note 纯函数，线程/ISR 安全。
 */
void writeBe32(std::span<uint8_t> buf, size_t offset, uint32_t value) noexcept
{
    // 每字节经移位掩码约束在 0..255，窄化到 uint8_t 安全。
    buf[offset] = static_cast<uint8_t>((value >> kShiftByte3) & kByteMask);
    buf[offset + kOffByte1] = static_cast<uint8_t>((value >> kShiftByte2) & kByteMask);
    buf[offset + kOffByte2] = static_cast<uint8_t>((value >> kShiftByte1) & kByteMask);
    buf[offset + kOffByte3] = static_cast<uint8_t>(value & kByteMask);
}

/**
 * @brief 校验 QNAME 标签序列边界并跳过至 QTYPE 起点（拒绝压缩指针与越界标签）
 * @param req: 完整 DNS 查询报文
 * @param[out] nextOffset: 成功时写入 QNAME 结束后的偏移（即 QTYPE 起始位置）
 * @retval true QNAME 合法且完整
 * @retval false 截断、超长或包含压缩指针/保留标签
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool skipQuestionName(std::span<const uint8_t> req, size_t &nextOffset) noexcept
{
    size_t pos = kDnsHeaderBytes;
    size_t wireLen = 0U;
    while (pos < req.size()) {
        const uint8_t labelLen = req[pos];
        if (labelLen == 0U) {
            ++pos;
            ++wireLen;
            if (wireLen > kDnsMaxWireNameBytes) {
                return false;
            }
            nextOffset = pos;
            return true;
        }
        // 强制门户只接受标准非压缩查询名，拒绝压缩指针（0xC0）与保留标签类型（0x40/0x80）。
        if ((labelLen & kDnsLabelTypeMask) != 0U) {
            return false;
        }
        const size_t step = 1U + static_cast<size_t>(labelLen);
        if ((step >= (req.size() - pos)) || ((wireLen + step + 1U) > kDnsMaxWireNameBytes)) {
            return false;
        }
        pos += step;
        wireLen += step;
    }
    return false;
}

/**
 * @brief 严格校验 Question 之后的附加段（仅允许 ARCOUNT=0 无尾字节，或 ARCOUNT=1 完整根名 OPT）
 * @param req: 完整 DNS 查询报文
 * @param questionEnd: Question 段结束偏移
 * @param arCount: 报头 ARCOUNT 字段值
 * @param[out] hasOpt: 成功时写入是否包含合法 EDNS0 OPT 记录
 * @retval true 附加段与报文剩余字节完全一致且合法
 * @retval false ARCOUNT 超限、尾部垃圾、OPT 截断、非根名/非 OPT 或 RDLENGTH 越界
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool validateAdditional(std::span<const uint8_t> req, size_t questionEnd,
                                      uint16_t arCount, bool &hasOpt) noexcept
{
    hasOpt = false;
    const size_t remaining = req.size() - questionEnd;
    if (arCount == 0U) {
        return remaining == 0U;
    }
    if ((arCount != 1U) || (remaining < kDnsOptFixedBytes)) {
        return false;
    }
    if ((req[questionEnd + kOptOffName] != 0U) ||
        (readBe16(req, questionEnd + kOptOffType) != kDnsTypeOpt)) {
        return false;
    }
    const size_t rdLen = readBe16(req, questionEnd + kOptOffRdLen);
    if ((kDnsOptFixedBytes + rdLen) != remaining) {
        return false;
    }
    hasOpt = true;
    return true;
}

/**
 * @brief 将校验通过的查询写入应答缓冲（报头 + Question + 可选 A 记录 + 可选最小 OPT 记录）
 * @param req: 原始查询报文
 * @param id: 事务 ID
 * @param reqFlags: 请求标志位（仅保留 RD）
 * @param questionEnd: Question 段结束偏移
 * @param answerA: true 时附加一条 A 记录，false 时回 NOERROR 空应答
 * @param hasOpt: true 时附加最小 EDNS0 OPT 记录并置 ARCOUNT=1
 * @param ipv4: A 记录 IPv4 地址四字节
 * @param[out] out: 输出缓冲（调用方已保证容量充足）
 * @note 纯函数，线程/ISR 安全。
 */
void writeDnsReply(std::span<const uint8_t> req, uint16_t id, uint16_t reqFlags, size_t questionEnd,
                   bool answerA, bool hasOpt, std::array<uint8_t, 4U> ipv4,
                   std::span<uint8_t> out) noexcept
{
    const uint16_t respFlags =
        static_cast<uint16_t>(kDnsFlagQr | kDnsFlagAa | (reqFlags & kDnsFlagRd));
    writeBe16(out, kHdrOffId, id);
    writeBe16(out, kHdrOffFlags, respFlags);
    writeBe16(out, kHdrOffQdCount, 1U);
    writeBe16(out, kHdrOffAnCount, answerA ? 1U : 0U);
    writeBe16(out, kHdrOffNsCount, 0U);
    writeBe16(out, kHdrOffArCount, hasOpt ? 1U : 0U);

    // 原样拷贝 Question 段。
    std::copy_n(req.begin() + static_cast<std::ptrdiff_t>(kDnsHeaderBytes),
                questionEnd - kDnsHeaderBytes,
                out.begin() + static_cast<std::ptrdiff_t>(kDnsHeaderBytes));

    size_t writePos = questionEnd;
    if (answerA) {
        writeBe16(out, writePos + kAnsOffName, kDnsNamePtrHeader);
        writeBe16(out, writePos + kAnsOffType, kDnsTypeA);
        writeBe16(out, writePos + kAnsOffClass, kDnsClassIn);
        writeBe32(out, writePos + kAnsOffTtl, kDnsAnswerTtlSec);
        writeBe16(out, writePos + kAnsOffRdLen, kDnsIpv4RdLength);
        std::copy_n(ipv4.begin(), ipv4.size(),
                    out.begin() + static_cast<std::ptrdiff_t>(writePos + kAnsOffRdData));
        writePos += kDnsAnswerRecordBytes;
    }
    if (hasOpt) {
        out[writePos + kOptOffName] = 0U;
        writeBe16(out, writePos + kOptOffType, kDnsTypeOpt);
        writeBe16(out, writePos + kOptOffUdpSize, kDnsOptUdpPayload);
        writeBe32(out, writePos + kOptOffExtFlags, 0U);
        writeBe16(out, writePos + kOptOffRdLen, 0U);
    }
}

} // namespace

size_t buildDnsReply(std::span<const uint8_t> req, std::array<uint8_t, 4U> ipv4,
                     std::span<uint8_t> out) noexcept
{
    if ((req.size() < kDnsMinQueryBytes) || (req.size() > kDnsMaxPacketBytes) ||
        (out.size() < kDnsMaxPacketBytes)) {
        return 0U;
    }

    const uint16_t id = readBe16(req, kHdrOffId);
    const uint16_t flags = readBe16(req, kHdrOffFlags);
    const uint16_t qdCount = readBe16(req, kHdrOffQdCount);
    const uint16_t anCount = readBe16(req, kHdrOffAnCount);
    const uint16_t nsCount = readBe16(req, kHdrOffNsCount);
    const uint16_t arCount = readBe16(req, kHdrOffArCount);

    // 仅受理标准单问句查询（ARCOUNT 仅允许 0 或 1 条完整根名 OPT 记录）。
    if (((flags & kDnsFlagQr) != 0U) || ((flags & kDnsFlagOpcodeMask) != 0U) ||
        ((flags & kDnsFlagTc) != 0U) || (qdCount != 1U) || (anCount != 0U) || (nsCount != 0U)) {
        return 0U;
    }

    size_t qtypeOffset = 0U;
    if (!skipQuestionName(req, qtypeOffset) ||
        ((req.size() - qtypeOffset) < kDnsQuestionTailBytes)) {
        return 0U;
    }

    const uint16_t qtype = readBe16(req, qtypeOffset);
    const uint16_t qclass = readBe16(req, qtypeOffset + kOffByte2);
    const size_t questionEnd = qtypeOffset + kDnsQuestionTailBytes;
    bool hasOpt = false;
    if (!validateAdditional(req, questionEnd, arCount, hasOpt)) {
        return 0U;
    }

    const bool answerA =
        (qtype == kDnsTypeA) && ((qclass == kDnsClassIn) || (qclass == kDnsClassAny));
    const size_t replyLen =
        questionEnd + (answerA ? kDnsAnswerRecordBytes : 0U) + (hasOpt ? kDnsOptFixedBytes : 0U);
    if ((replyLen > kDnsMaxPacketBytes) || (replyLen > out.size())) {
        return 0U;
    }

    writeDnsReply(req, id, flags, questionEnd, answerA, hasOpt, ipv4, out);
    return replyLen;
}

} // namespace app::web_cfg
