// Unicore N4（UM980/UM981/UM982）驱动实现：$command 应答解码、VERSIONA 探测、流动站配置脚本（ADR-0010 §2.2/§2.4）。
// 结构（P3s）：命令生成（cmd_*）、应答判定（resp_*）与脚本步骤（step_*）是 UnicoreReceiver 的 protected static，
// 型号子类（Um982Receiver）直接复用；文件内匿名命名空间只留成帧判定（judge 家族）与型号识别。
// 线程/调用约束：全部为纯函数（无全局可变状态），可从任意任务调用，不可阻塞；应答解码器亦可用于 ISR 安全的
// demux 引擎（firstByte/step 只读入参）。
// 堆：不使用。浮点：不使用（频率参数按手册写死为文本）。
#include "gnss/drivers/UnicoreReceiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "demux/TextWriter.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "../AsciiCmd.hpp"
#include "../DriverInstances.hpp" // 单例声明（保证定义与 extern 声明一致）
#include "../FrameTag.hpp"

namespace gnss {
namespace {

constexpr uint8_t kDollar = static_cast<uint8_t>('$');
constexpr uint8_t kStar = static_cast<uint8_t>(ascii::kStar);
constexpr uint8_t kCr = static_cast<uint8_t>(ascii::kCr);
constexpr uint8_t kLf = static_cast<uint8_t>(ascii::kLf);
constexpr std::string_view kCommandPrefix = "$command,";
// 读回行（ADR-0012 §2.7）："$CONFIG,<port>,<config>*hh\r\n"，与 $command 应答同为 NMEA 式帧，仅前缀不同。
constexpr std::string_view kConfigPrefix = "$CONFIG,";
constexpr std::string_view kResponseTag = ",response: ";
constexpr std::string_view kVersionTag = "#VERSIONA,";
/** 前缀之后的最小应答字节数（",OK" 一类），不足则继续等待。 */
constexpr std::size_t kMinReplyBytes = 6U;
/** 应答尾 "*hh\r\n" 的总字节数，即 '*' 距帧尾的偏移（沿用旧实现，ADR-0010）。 */
constexpr std::size_t kStarFromEnd = ascii::kTailBytes;
constexpr uint32_t kConfigTimeoutMs = 500U;

// $command 应答（classifyReply）与 #VERSIONA 探测应答（matchProbe）各需一个解码器，缺一则对应帧落入 unknown。
constexpr std::array<const demux::Decoder *, 2> kDecoders{&unicore::kResponseDecoder,
                                                          &demux::unicore_hash::kDecoder};

/** 报文名字（下标 = OutputMsg）：与大写常量同名，便于与手册/日志对照。 */
constexpr std::array<const char *, kOutputMsgCount> kOutputNames = {
    "GPGGA", "SATSINFOA", "HEADINGA", "GPGST", "GPRMC", "GPGSA", "GPGSV",
};

/** 应答帧的判定原因，explain 与 step 共用同一份判定逻辑。 */
enum class ResponseReason : uint8_t {
    kNone = 0,
    kEmpty,
    kPrefix,
    kTail,
    kBody,
    kChecksum,
    kStarField,
    kStarByte,
    kNoRoom,
};

struct ResponseVerdict {
    demux::Step step = demux::Step::kReject;
    ResponseReason reason = ResponseReason::kNone;
    std::size_t need = 0;
    std::size_t offset = 0;
    uint8_t received = 0;
    uint8_t computed = 0;
    uint8_t alt = 0; // 第二个期望字节（两种前缀都还成立时才有，0 表示没有）
};

/**
 * @brief 构造“拒绝”结论。
 * @param reason: 拒绝原因（explain 用）。
 * @param offset: 出问题的字节位置。
 * @param received: 实际读到的字节/校验值。
 * @param computed: 期望的字节/校验值。
 * @retval verdict: step = kReject。
 */
ResponseVerdict reject(ResponseReason reason, std::size_t offset = 0, uint8_t received = 0,
                       uint8_t computed = 0) noexcept
{
    ResponseVerdict verdict{};
    verdict.reason = reason;
    verdict.offset = offset;
    verdict.received = received;
    verdict.computed = computed;
    return verdict;
}

/**
 * @brief 构造“前缀字节不符”的拒绝结论（两种前缀都可能时给出两个期望字节）。
 * @param offset: 出问题的字节位置。
 * @param received: 实际读到的字节。
 * @param wantFirst: 第一个仍可能的期望字节（0 表示该候选已失效）。
 * @param wantSecond: 第二个仍可能的期望字节（0 表示没有）。
 * @retval verdict: step = kReject，reason = kPrefix。
 */
ResponseVerdict rejectPrefix(std::size_t offset, uint8_t received, uint8_t wantFirst,
                             uint8_t wantSecond) noexcept
{
    ResponseVerdict verdict = reject(ResponseReason::kPrefix, offset, received, wantFirst);
    verdict.alt = wantSecond;
    return verdict;
}

/**
 * @brief 构造“还需要更多字节”结论。
 * @param need: 至少还差多少字节。
 * @retval verdict: step = kNeedMore。
 */
ResponseVerdict needMore(std::size_t need) noexcept
{
    ResponseVerdict verdict{};
    verdict.step = demux::Step::kNeedMore;
    verdict.need = need;
    return verdict;
}

/**
 * @brief 构造“完整帧”结论。
 * @retval verdict: step = kFrame。
 */
ResponseVerdict complete() noexcept
{
    ResponseVerdict verdict{};
    verdict.step = demux::Step::kFrame;
    return verdict;
}

/**
 * @brief 检查两种可接受前缀（"$command," / "$CONFIG,"）与最小长度。
 *
 * 逐字节判定：只要仍有前缀成立就继续收取；两个候选同时失配才拒绝。两者从第 2 字节起分叉（'c' / 'C'），
 * 因此绝大多数拒收只报一个期望字节，第 2 字节失配时报两个。
 * @param frame: 已收到的字节。
 * @retval std::nullopt: 前缀完整且长度够，可继续看正文。
 * @retval verdict: 需要继续收取或直接拒绝。
 */
std::optional<ResponseVerdict> checkPrefix(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (size == 0U) {
        return reject(ResponseReason::kEmpty);
    }
    const size_t cmdLen = kCommandPrefix.size();
    const size_t cfgLen = kConfigPrefix.size();
    const size_t longest = cmdLen > cfgLen ? cmdLen : cfgLen;
    const size_t compared = size < longest ? size : longest;
    bool cmdAlive = true;
    bool cfgAlive = true;
    for (size_t i = 0; i < compared; ++i) {
        const uint8_t byte = frame[i];
        // 前缀已走完的候选不再受本字节约束（视为仍然成立）。
        const bool cmdNext =
            cmdAlive && ((i >= cmdLen) || (byte == static_cast<uint8_t>(kCommandPrefix[i])));
        const bool cfgNext =
            cfgAlive && ((i >= cfgLen) || (byte == static_cast<uint8_t>(kConfigPrefix[i])));
        if (!cmdNext && !cfgNext) {
            // 只有失配前仍成立的候选才参与“expected”文本，避免报出已排除的字节。
            const uint8_t wantCmd =
                (cmdAlive && (i < cmdLen)) ? static_cast<uint8_t>(kCommandPrefix[i]) : 0U;
            const uint8_t wantCfg =
                (cfgAlive && (i < cfgLen)) ? static_cast<uint8_t>(kConfigPrefix[i]) : 0U;
            return rejectPrefix(i, byte, wantCmd, wantCfg);
        }
        cmdAlive = cmdNext;
        cfgAlive = cfgNext;
    }
    // 最小帧长按较长前缀（$command）计算：对 $CONFIG 只多要 1 字节，仍远小于真实读回行。
    if (size < cmdLen) {
        return needMore(cmdLen - size);
    }
    if (size < (cmdLen + kMinReplyBytes)) {
        return needMore((cmdLen + kMinReplyBytes) - size);
    }
    return std::nullopt;
}

/**
 * @brief 校验和是否匹配（标准 XOR，或 UM980 实测把 '$' 一并计入的变体）。
 * @param frame: 整帧字节。
 * @param starPos: '*' 的下标。
 * @param received: 输出，帧内写的校验值。
 * @param computed: 输出，按报文体算出的校验值。
 * @retval true: 二者任一一致。
 * @retval false: 都不一致。
 */
bool checksumMatches(std::span<const uint8_t> frame, std::size_t starPos, uint8_t &received,
                     uint8_t &computed) noexcept
{
    computed = ascii::xorChecksum(frame, 1U, starPos);
    // hexValue 返回 uint8_t，整型提升为 int 后再移位属有符号位运算：先转 uint32_t（值域 0..15，安全）。
    const uint32_t high = static_cast<uint32_t>(ascii::hexValue(frame[starPos + 1U]))
                          << ascii::kHexShift;
    const uint32_t low = static_cast<uint32_t>(ascii::hexValue(frame[starPos + 2U]));
    received = static_cast<uint8_t>(high | low);
    if (computed == received) {
        return true;
    }
    // UM980 R4.10Build11833 实测把 '$' 也算进 $command 校验（"$command,VERSIONA,response: OK*45"，
    // 标准 XOR 为 0x61）；SparkFun Unicore 库对旧固件有同样的兼容（Unicore/src 内 "improperly adding the '$'"）。
    return static_cast<uint8_t>(computed ^ kDollar) == received;
}

/**
 * @brief 收到 '\n' 之后的完整帧检查：尾结构、报文体字节、校验和。
 * @param frame: 整帧字节。
 * @retval std::nullopt: 全部通过。
 * @retval verdict: 拒绝原因。
 */
std::optional<ResponseVerdict> checkFullFrame(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (!ascii::hasHexTail(frame)) {
        return reject(ResponseReason::kTail, size - 1U, frame[size - 1U]);
    }
    const size_t starPos = size - kStarFromEnd;
    for (size_t i = 1U; i < starPos; ++i) {
        if (!ascii::isBodyByte(frame[i])) {
            return reject(ResponseReason::kBody, i, frame[i]);
        }
    }
    uint8_t received = 0;
    uint8_t computed = 0;
    if (!checksumMatches(frame, starPos, received, computed)) {
        return reject(ResponseReason::kChecksum, starPos, received, computed);
    }
    return std::nullopt;
}

/**
 * @brief 尚未收到 '\n'：在末尾 5 字节窗口里找 '*'，据此判断补几个字节。
 * @param frame: 已收到的字节（非空）。
 * @retval std::nullopt: 不会出现（仅为与另外两个检查函数同签名）。
 * @retval verdict: 需要继续收取或直接拒绝。
 */
std::optional<ResponseVerdict> checkPartialFrame(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    size_t starPos = size;
    const size_t window = size < kStarFromEnd ? size : kStarFromEnd;
    for (size_t k = 0; k < window; ++k) {
        if (frame[size - 1U - k] == kStar) {
            starPos = size - 1U - k;
            break;
        }
    }
    if (starPos == size) {
        // 没有 '*' 时只校验最后一字节是否为合法报文体，避免把半个多字节字符判成帧。
        if (!ascii::isBodyByte(frame[size - 1U])) {
            return reject(ResponseReason::kBody, size - 1U, frame[size - 1U]);
        }
        return needMore(1U);
    }
    if ((size - starPos) > kStarFromEnd) {
        return reject(ResponseReason::kStarField, starPos);
    }
    for (size_t i = starPos + 1U; i < size; ++i) {
        const uint8_t c = frame[i];
        if (!ascii::isHex(c) && c != kCr && c != kLf) {
            return reject(ResponseReason::kStarByte, i, c);
        }
    }
    if ((size - starPos) >= kStarFromEnd) {
        return reject(ResponseReason::kNoRoom, starPos);
    }
    return needMore(kStarFromEnd - (size - starPos));
}

/**
 * @brief 统一的帧判定入口：explain 与 step 共用，保证诊断结论与实际成帧一致。
 * @param frame: 已收到的字节。
 * @retval verdict: kFrame / kNeedMore / kReject 之一。
 */
ResponseVerdict judge(std::span<const uint8_t> frame) noexcept
{
    if (const std::optional<ResponseVerdict> head = checkPrefix(frame)) {
        return *head;
    }
    if (frame[frame.size() - 1U] == kLf) {
        if (const std::optional<ResponseVerdict> tail = checkFullFrame(frame)) {
            return *tail;
        }
        return complete();
    }
    if (const std::optional<ResponseVerdict> partial = checkPartialFrame(frame)) {
        return *partial;
    }
    return complete();
}

/**
 * @brief 把判定原因写成一句英文（explain 契约）。
 * @param verdict: judge 的结论。
 * @param text: 输出文本写入器。
 */
void explainVerdict(const ResponseVerdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case ResponseReason::kEmpty:
        text.literal("empty prefix");
        break;
    case ResponseReason::kPrefix:
        text.literal("byte ").decimal(verdict.offset).literal(" is ");
        text.byteValue(verdict.received).literal(", expected ");
        text.byteValue(verdict.computed);
        if (verdict.alt != 0U) {
            text.literal(" or ");
            text.byteValue(verdict.alt);
        }
        break;
    case ResponseReason::kTail:
        text.literal("missing \"*hh\\r\\n\" tail before byte ")
            .decimal(verdict.offset)
            .literal(" (");
        text.byteValue(verdict.received).literal(")");
        break;
    case ResponseReason::kBody:
        text.literal("illegal byte ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(" at offset ")
            .decimal(verdict.offset);
        break;
    case ResponseReason::kChecksum:
        text.literal("bad checksum, received ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(", computed ")
            .hex(static_cast<uint32_t>(verdict.computed), 2U);
        break;
    case ResponseReason::kStarField:
        text.literal("'*' at offset ")
            .decimal(verdict.offset)
            .literal(" but fewer than two checksum digits");
        break;
    case ResponseReason::kStarByte:
        text.literal("illegal byte ")
            .hex(static_cast<uint32_t>(verdict.received), 2U)
            .literal(" in checksum field at offset ")
            .decimal(verdict.offset);
        break;
    case ResponseReason::kNoRoom:
        text.literal("no room for CR LF after checksum at offset ").decimal(verdict.offset);
        break;
    case ResponseReason::kNone:
    default:
        text.literal("rejected");
        break;
    }
}

/**
 * @brief 把型号与固件版本写进探测结果（固件文本截断到 kVersionLen-1）。
 * @param model: 识别出的型号。
 * @param fwText: VERSIONA 里的固件文本。
 * @param out: 输出探测结果。
 */
void fillProbeResult(Model model, std::span<const uint8_t> fwText, ProbeResult &out) noexcept
{
    out = ProbeResult{};
    out.model = model;
    const size_t copy = fwText.size() < (kVersionLen - 1) ? fwText.size() : (kVersionLen - 1);
    for (size_t i = 0; i < copy; ++i) {
        out.fwVersion[i] = static_cast<char>(fwText[i]);
    }
    out.fwVersion[copy] = '\0';
}

/**
 * @brief 版本文本 → 型号：UM982 双天线、UM981 单天线带 IMU，其余 UM9xx 按 UM980 能力处理（ADR-0010 §2.2）。
 * @param text: VERSIONA 里第一个引号字段。
 * @retval model: 识别出的型号。
 * @retval std::nullopt: 不是 Unicore N4 型号。
 */
std::optional<Model> modelFromText(std::span<const uint8_t> text) noexcept
{
    if (ascii::quotedEquals(text, "UM982")) {
        return Model::kUm982;
    }
    if (ascii::quotedEquals(text, "UM981")) {
        return Model::kUm981;
    }
    if (ascii::quotedEquals(text, "UM980") || ascii::quotedStartsWith(text, "UM9")) {
        return Model::kUm980;
    }
    return std::nullopt;
}

} // namespace

namespace unicore {

/**
 * @brief $command 应答帧的起始字节判定。
 * @param byte: 待判字节。
 * @retval true: 是 '$'。
 * @retval false: 其他。
 */
bool responseFirstByte(uint8_t byte) noexcept
{
    return byte == kDollar;
}

/**
 * @brief $command 应答帧的成帧判定（demux 引擎每收到一字节调用，须 ISR 安全）。
 * @param frame: 已收到的字节。
 * @param need: 输出，至少还差多少字节（kNeedMore 时有效）。
 * @retval step: kFrame / kNeedMore / kReject。
 */
demux::Step responseStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    const ResponseVerdict verdict = judge(frame);
    need = verdict.need;
    return verdict.step;
}

/**
 * @brief 把拒绝原因写成一句英文（诊断为“not rejected”表示帧本身没问题）。
 * @param frame: 已收到的字节。
 * @param out: 输出缓冲。
 * @retval len: 写入的字符数。
 */
std::size_t explainResponse(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    demux::text_writer::TextWriter text(out);
    if (frame.empty()) {
        return text.literal("empty prefix").finish();
    }
    const ResponseVerdict verdict = judge(frame);
    if (verdict.step == demux::Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explainVerdict(verdict, text);
    return text.finish();
}

} // namespace unicore

namespace detail {

// NOLINTNEXTLINE(readability-identifier-naming): 外部链接只读单例，命名见 DriverInstances.hpp
constinit const Um980Receiver kUm980Receiver{};
// NOLINTNEXTLINE(readability-identifier-naming): 同上
constinit const Um982Receiver kUm982Receiver{};

} // namespace detail

// ---- 命令库：每个函数只把一条完整命令（含 "\r\n"）写进 out，不关心什么时候发 ----

/**
 * @brief 关闭该口全部报文输出："UNLOG COMn"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号（1..4）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdUnlog(CmdBuf &out, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::appendUint(out, port) && out.append("\r\n");
}

/**
 * @brief 切到流动站模式："MODE ROVER"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdModeRover(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("MODE ROVER\r\n");
}

/**
 * @brief 改端口波特率："CONFIG COMn <baud>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号。
 * @param baud: 目标波特率（如 921600）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdConfigBaud(CmdBuf &out, uint8_t port, uint32_t baud) noexcept
{
    out.clear();
    return out.append("CONFIG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           ascii::appendUint(out, baud) && out.append("\r\n");
}

/**
 * @brief 以给定周期开启一类报文："<name> COMn <period>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGGA"）。
 * @param port: 接收机端口号。
 * @param periodMs: 周期（毫秒），必须是 10 ms 整数倍。
 * @retval true: 已写入完整命令。
 * @retval false: 周期粒度不合法或缓冲空间不足。
 */
bool UnicoreReceiver::cmdLogMsg(CmdBuf &out, std::string_view name, uint8_t port,
                                uint32_t periodMs) noexcept
{
    out.clear();
    return out.append(name) && out.append(" COM") && ascii::appendUint(out, port) &&
           out.append(" ") && ascii::appendPeriod(out, periodMs) && out.append("\r\n");
}

/**
 * @brief 关闭一类报文："UNLOG COMn <name>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGSV"）。
 * @param port: 接收机端口号。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdUnlogMsg(CmdBuf &out, std::string_view name, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           out.append(name) && out.append("\r\n");
}

/**
 * @brief 探测用的版本查询："VERSIONA"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdVersionQuery(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("VERSIONA\r\n");
}

// ---- 应答解析：每个函数只判一件事，组合起来才得出“这条应答是不是本步命令的、成功还是失败” ----

/**
 * @brief 帧尾是否为合法的 "*hh\r\n"，且长度足够容纳最短应答。
 * @param frame: 整帧字节。
 * @retval true: 尾部结构与最小长度都满足。
 * @retval false: 结构不符或过短。
 */
bool UnicoreReceiver::respHasValidTail(std::span<const uint8_t> frame) noexcept
{
    return (frame.size() >= (kCommandPrefix.size() + kMinReplyBytes)) && ascii::hasHexTail(frame);
}

/**
 * @brief 找 ",response: " 的位置（命令回显与应答文本的分界）。
 * @param frame: 整帧字节。
 * @retval 位置: 标签起始下标。
 * @retval std::nullopt: 没有该标签（不是 $command 应答）。
 */
std::optional<size_t> UnicoreReceiver::respFindTag(std::span<const uint8_t> frame) noexcept
{
    for (size_t i = kCommandPrefix.size(); (i + kResponseTag.size()) <= frame.size(); ++i) {
        if (ascii::startsWith(frame.subspan(i), kResponseTag)) {
            return i;
        }
    }
    return std::nullopt;
}

/**
 * @brief 帧里的回显是否就是本步发送的命令（忽略发送时的 CR/LF）。
 * @param frame: 整帧字节。
 * @param tagPos: ",response: " 的起始下标。
 * @param sent: 本步发出的命令字节。
 * @retval true: 回显与 sent 逐字节相同。
 * @retval false: 长度或内容不同（属于别的命令的应答）。
 */
bool UnicoreReceiver::respEchoMatches(std::span<const uint8_t> frame, size_t tagPos,
                                      std::span<const uint8_t> sent) noexcept
{
    if (tagPos < kCommandPrefix.size()) {
        return false;
    }
    const size_t sentLen = ascii::stripLineEnd(sent);
    if ((tagPos - kCommandPrefix.size()) != sentLen) {
        return false;
    }
    return ascii::bytesStartsWith(frame.subspan(kCommandPrefix.size()), sent.first(sentLen));
}

/**
 * @brief 取应答文本（标签之后到 '*' 之前，去掉尾部空格）。
 * @param frame: 整帧字节。
 * @param tagPos: ",response: " 的起始下标。
 * @retval span: 应答文本（可能为空）。
 */
std::span<const uint8_t> UnicoreReceiver::respText(std::span<const uint8_t> frame,
                                                   size_t tagPos) noexcept
{
    if (frame.size() < kStarFromEnd) {
        return {};
    }
    const size_t begin = tagPos + kResponseTag.size();
    size_t end = frame.size() - kStarFromEnd;
    while ((end > begin) && (frame[end - 1U] == static_cast<uint8_t>(' '))) {
        --end;
    }
    if ((end <= begin) || (begin >= frame.size())) {
        return {};
    }
    return frame.subspan(begin, end - begin);
}

// ---- 基础脚本的步骤与启用条件 ----

/**
 * @brief 步骤启用条件：总是。
 * @param cfg: 流动站配置（本判断不使用）。
 * @retval true: 恒为 true。
 */
bool UnicoreReceiver::stepAlways(const RoverConfig &cfg) noexcept
{
    static_cast<void>(cfg);
    return true;
}

/**
 * @brief 步骤启用条件：cfg.portBaud != 0（为 0 时脚本提前结束）。
 * @param cfg: 流动站配置。
 * @retval true: 需要改波特率。
 * @retval false: 不改波特率。
 */
bool UnicoreReceiver::stepIfPortBaud(const RoverConfig &cfg) noexcept
{
    return cfg.portBaud != 0U;
}

/**
 * @brief 第 0 步：先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。
 * @param cfg: 流动站配置（用端口号）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepUnlogAll(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmdUnlog(out.cmd, cfg.port)) {
        return false;
    }
    out.expectReply = true;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = 0;
    return true;
}

/**
 * @brief 第 1 步：切到流动站模式。
 * @param cfg: 流动站配置（本步不使用）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepModeRover(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    static_cast<void>(cfg);
    if (!cmdModeRover(out.cmd)) {
        return false;
    }
    out.expectReply = true;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = 0;
    return true;
}

/**
 * @brief 第 2 步：改端口波特率；手册要求切换后不再按旧速率等待应答。
 * @param cfg: 流动站配置（用端口号与目标波特率）。
 * @param out: 输出步骤（switchBaud = 目标波特率）。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepPortBaud(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmdConfigBaud(out.cmd, cfg.port, cfg.portBaud)) {
        return false;
    }
    out.expectReply = false;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = cfg.portBaud;
    return true;
}

// ---- 驱动成员 ----

Family UnicoreReceiver::family() const noexcept
{
    return Family::kUnicore;
}

std::string_view UnicoreReceiver::name() const noexcept
{
    return "unicore";
}

std::span<const demux::Decoder *const> UnicoreReceiver::decoders() const noexcept
{
    return kDecoders;
}

uint32_t UnicoreReceiver::caps(Model model) const noexcept
{
    switch (model) {
    case Model::kUm981:
        return cap::kRtcmIn | cap::kBase | cap::kIns | cap::kRate20Hz;
    case Model::kUm982: // 家族视角：双天线能力由 Um982Receiver 追加（ADR-0012 §2.8）
    case Model::kUm980:
        return cap::kRtcmIn | cap::kBase | cap::kRate20Hz;
    default:
        return cap::kRtcmIn | cap::kBase; // 型号未知时给出保守能力
    }
}

const Receiver &UnicoreReceiver::forModel(Model model) const noexcept
{
    // 双天线型号有单独驱动（多 kHeading 能力），其余型号就是家族驱动本身（ADR-0012 §2.8）。
    return (model == Model::kUm982) ? detail::kUm982Receiver : *this;
}

bool UnicoreReceiver::buildProbe(CmdBuf &out) const noexcept
{
    return cmdVersionQuery(out);
}

bool UnicoreReceiver::buildReadback(CmdBuf &out) const noexcept
{
    out.clear();
    return out.append("CONFIG\r\n");
}

size_t UnicoreReceiver::matchReadback(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                      std::span<char> out) const noexcept
{
    // 只认应答解码器交付的 $CONFIG 行：$command,CONFIG,response: OK*.. 是命令应答，不算配置行。
    if ((&dec != &unicore::kResponseDecoder) ||
        (frame.size() <= (kConfigPrefix.size() + kStarFromEnd)) ||
        !ascii::startsWith(frame, kConfigPrefix)) {
        return 0U;
    }
    const size_t begin = kConfigPrefix.size();
    const size_t end = frame.size() - kStarFromEnd; // '*' 的位置：帧尾固定 "*hh\r\n"
    if ((frame[end] != kStar) || (end <= begin)) {
        return 0U;
    }
    const size_t body = end - begin;
    const size_t copy = body < out.size() ? body : out.size();
    for (size_t i = 0; i < copy; ++i) {
        out[i] = static_cast<char>(frame[begin + i]); // 帧体已由解码器限定为可打印 ASCII
    }
    return copy;
}

bool UnicoreReceiver::matchProbe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                 ProbeResult &out) const noexcept
{
    if ((&dec != &demux::unicore_hash::kDecoder) || (frame.size() < kVersionTag.size()) ||
        !ascii::startsWith(frame, kVersionTag)) {
        return false;
    }
    size_t pos = kVersionTag.size();
    std::span<const uint8_t> modelText{};
    if (!ascii::nextQuoted(frame, pos, modelText)) {
        return false;
    }
    const std::optional<Model> model = modelFromText(modelText);
    if (!model.has_value()) {
        return false;
    }
    std::span<const uint8_t> fwText{};
    // 固件版本字段可选：旧固件没有它时只填型号。
    static_cast<void>(ascii::nextQuoted(frame, pos, fwText));
    fillProbeResult(*model, fwText, out);
    return true;
}

bool UnicoreReceiver::roverStep(size_t index, Model model, const RoverConfig &cfg,
                                ConfigStep &out) const noexcept
{
    static_cast<void>(model); // 基础脚本与型号无关；报文输出改由输出阶段（buildOutput）下发
    size_t position = 0;
    for (const ScriptStep &step : roverScript()) {
        if (!step.enabled(cfg)) {
            continue;
        }
        if (position == index) {
            return step.build(cfg, out);
        }
        ++position;
    }
    return false; // 脚本结束
}

std::span<const UnicoreReceiver::ScriptStep> UnicoreReceiver::roverScript() const noexcept
{
    // 表放在成员函数作用域内：ScriptStep 与步骤函数都是 protected，只有成员作用域才能取地址。
    static constexpr std::array<ScriptStep, 3> kScript{{
        {&stepUnlogAll, &stepAlways},
        {&stepModeRover, &stepAlways},
        {&stepPortBaud, &stepIfPortBaud},
    }};
    return kScript;
}

Ack UnicoreReceiver::classifyReply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                   std::span<const uint8_t> sent) const noexcept
{
    if (&dec != &unicore::kResponseDecoder) {
        return Ack::kNone;
    }
    if (!respHasValidTail(frame)) {
        return Ack::kNone;
    }
    const std::optional<size_t> tag = respFindTag(frame);
    if (!tag.has_value() || !respEchoMatches(frame, *tag, sent)) {
        return Ack::kNone; // 不是本步命令的应答
    }
    return ascii::quotedEquals(respText(frame, *tag), "OK") ? Ack::kAck : Ack::kNack;
}

OutputMsg UnicoreReceiver::classifyFrame(const demux::Decoder &dec,
                                         std::span<const uint8_t> frame) const noexcept
{
    // 厂家私有帧：其余（NMEA 等）交回基类，保持与 main 原 classify_gnss_sentence() 一致。
    if (detail::startsWith(frame, "#SATSINFOA,")) {
        return OutputMsg::kSatsInfo;
    }
    if (detail::startsWith(frame, "#HEADINGA,")) {
        return OutputMsg::kHeading;
    }
    return Receiver::classifyFrame(dec, frame);
}

size_t UnicoreReceiver::frameTag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                 std::span<char> out) const noexcept
{
    const std::string_view name{dec.name};
    if (name == "UnicoreHash" && frame.size() > detail::kNmeaTagOffset) {
        return detail::tagWithWord(out, "UNICORE ", frame, detail::kNmeaTagOffset);
    }
    if (name == "UnicoreResp" && frame.size() > detail::kCommandTagOffset) {
        // "$command,<cmd>,response: ..."：命令名在第二个逗号之前。
        return detail::tagWithWord(out, "UNICORE_CMD ", frame, detail::kCommandTagOffset);
    }
    return Receiver::frameTag(dec, frame, out);
}

uint32_t UnicoreReceiver::outputMask(Model model) const noexcept
{
    constexpr uint32_t kSatsInfoBit = 1U << static_cast<uint32_t>(OutputMsg::kSatsInfo);
    constexpr uint32_t kHeadingBit = 1U << static_cast<uint32_t>(OutputMsg::kHeading);
    constexpr uint32_t kGstBit = 1U << static_cast<uint32_t>(OutputMsg::kGst);
    // UM9xx 都能输出 GPGST（旧脚本一直下发 GPGST），基线基类默认只含 GGA/RMC/GSA/GSV。
    uint32_t mask = Receiver::outputMask(model) | kSatsInfoBit | kGstBit;
    // 经虚函数问能力：Um982Receiver 覆盖 caps() 后这里自动带上 HEADINGA。
    if ((caps(model) & cap::kHeading) != 0U) {
        mask |= kHeadingBit; // 仅双天线型号（UM982）有 HEADINGA
    }
    return mask;
}

fw::Status UnicoreReceiver::buildOutput(OutputMsg msg, uint32_t periodMs, Model model,
                                        const OutputPlan &plan, CmdBuf &out) const noexcept
{
    const auto idx = static_cast<size_t>(static_cast<uint8_t>(msg));
    if ((msg == OutputMsg::kNone) || (idx >= kOutputNames.size())) {
        return fw::Status::kInvalidArg;
    }
    // 同上：能力位判断走虚调用，型号子类只覆盖 caps() 就能放开 HEADINGA。
    if ((msg == OutputMsg::kHeading) && ((caps(model) & cap::kHeading) == 0U)) {
        return fw::Status::kNotSupported; // 单天线型号没有 HEADINGA
    }
    const std::string_view name{kOutputNames[idx]};
    // period 0 = 关闭该类；旧 apply 旁路同样用 "UNLOG COMn <name>"。
    const bool written = (periodMs == 0U) ? cmdUnlogMsg(out, name, plan.port)
                                          : cmdLogMsg(out, name, plan.port, periodMs);
    if (!written) {
        out.clear();
        return fw::Status::kInvalidArg;
    }
    return fw::Status::kOk;
}

std::string_view Um982Receiver::name() const noexcept
{
    return "unicore-um982";
}

uint32_t Um982Receiver::caps(Model model) const noexcept
{
    // 家族能力 + HEADINGA 输出（双天线）；其余能力与 UM980/UM981 相同。
    return UnicoreReceiver::caps(model) | cap::kHeading;
}

const Receiver &Um982Receiver::forModel(Model) const noexcept
{
    return *this;
}

} // namespace gnss
