// Unicore N4（UM980/UM981/UM982）驱动实现：$command 应答解码、VERSIONA 探测、流动站配置脚本（ADR-0010 §2.2/§2.4）。
// 结构（P3r）：cmd:: 只生成一条完整命令，resp:: 只判定应答的一部分，script 表把“第几步做什么”与流程分开；
// 其余是成帧判定（judge 家族）与驱动成员实现。
// 线程/调用约束：全部为纯函数（无全局可变状态），可从任意任务调用，不可阻塞；应答解码器亦可用于 ISR 安全的
// demux 引擎（first_byte/step 只读入参）。
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
#include "../FrameTag.hpp"

namespace gnss::unicore {
namespace {

constexpr uint8_t kDollar = static_cast<uint8_t>('$');
constexpr uint8_t kStar = static_cast<uint8_t>(ascii::kStar);
constexpr uint8_t kCr = static_cast<uint8_t>(ascii::kCr);
constexpr uint8_t kLf = static_cast<uint8_t>(ascii::kLf);
constexpr std::string_view kCommandPrefix = "$command,";
constexpr std::string_view kResponseTag = ",response: ";
constexpr std::string_view kVersionTag = "#VERSIONA,";
/** 前缀之后的最小应答字节数（",OK" 一类），不足则继续等待。 */
constexpr std::size_t kMinReplyBytes = 6U;
/** 应答尾 "*hh\r\n" 的总字节数，即 '*' 距帧尾的偏移（沿用旧实现，ADR-0010）。 */
constexpr std::size_t kStarFromEnd = ascii::kTailBytes;
constexpr uint32_t kConfigTimeoutMs = 500U;

// $command 应答（classify_reply）与 #VERSIONA 探测应答（match_probe）各需一个解码器，缺一则对应帧落入 unknown。
constexpr std::array<const demux::Decoder *, 2> kDecoders{&kResponseDecoder, &demux::unicore_hash::kDecoder};

/** 报文名字（下标 = OutputMsg）：与大写常量同名，便于与手册/日志对照。 */
constexpr std::array<const char *, kOutputMsgCount> kOutputNames = {
    "GPGGA", "SATSINFOA", "HEADINGA", "GPGST", "GPRMC", "GPGSA", "GPGSV",
};

/**
 * 命令库：每个函数只把一条完整命令（含 "\r\n"）写进 out，不关心什么时候发。
 * 失败一律返回 false（缓冲不足或参数无法表示），out 内容随即作废。
 */
namespace cmd {

/**
 * @brief 关闭该口全部报文输出："UNLOG COMn"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号（1..4）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool unlog(CmdBuf &out, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::append_uint(out, port) && out.append("\r\n");
}

/**
 * @brief 切到流动站模式："MODE ROVER"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool mode_rover(CmdBuf &out) noexcept
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
bool config_baud(CmdBuf &out, uint8_t port, uint32_t baud) noexcept
{
    out.clear();
    return out.append("CONFIG COM") && ascii::append_uint(out, port) && out.append(" ") &&
           ascii::append_uint(out, baud) && out.append("\r\n");
}

/**
 * @brief 以给定周期开启一类报文："<name> COMn <period>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGGA"）。
 * @param port: 接收机端口号。
 * @param period_ms: 周期（毫秒），必须是 10 ms 整数倍。
 * @retval true: 已写入完整命令。
 * @retval false: 周期粒度不合法或缓冲空间不足。
 */
bool log_msg(CmdBuf &out, std::string_view name, uint8_t port, uint32_t period_ms) noexcept
{
    out.clear();
    return out.append(name) && out.append(" COM") && ascii::append_uint(out, port) && out.append(" ") &&
           ascii::append_period(out, period_ms) && out.append("\r\n");
}

/**
 * @brief 关闭一类报文："UNLOG COMn <name>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGSV"）。
 * @param port: 接收机端口号。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool unlog_msg(CmdBuf &out, std::string_view name, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::append_uint(out, port) && out.append(" ") && out.append(name) &&
           out.append("\r\n");
}

/**
 * @brief 探测用的版本查询："VERSIONA"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool version_query(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("VERSIONA\r\n");
}

} // namespace cmd

/**
 * 应答库：每个函数只判一件事，组合起来才得出“这条应答是不是本步命令的、成功还是失败”。
 */
namespace resp {

/**
 * @brief 帧尾是否为合法的 "*hh\r\n"，且长度足够容纳最短应答。
 * @param frame: 整帧字节。
 * @retval true: 尾部结构与最小长度都满足。
 * @retval false: 结构不符或过短。
 */
bool has_valid_tail(std::span<const uint8_t> frame) noexcept
{
    return (frame.size() >= (kCommandPrefix.size() + kMinReplyBytes)) && ascii::has_hex_tail(frame);
}

/**
 * @brief 找 ",response: " 的位置（命令回显与应答文本的分界）。
 * @param frame: 整帧字节。
 * @retval 位置: 标签起始下标。
 * @retval std::nullopt: 没有该标签（不是 $command 应答）。
 */
std::optional<size_t> find_response_tag(std::span<const uint8_t> frame) noexcept
{
    for (size_t i = kCommandPrefix.size(); (i + kResponseTag.size()) <= frame.size(); ++i) {
        if (ascii::starts_with(frame.subspan(i), kResponseTag)) {
            return i;
        }
    }
    return std::nullopt;
}

/**
 * @brief 帧里的回显是否就是本步发送的命令（忽略发送时的 CR/LF）。
 * @param frame: 整帧字节。
 * @param tag_pos: ",response: " 的起始下标。
 * @param sent: 本步发出的命令字节。
 * @retval true: 回显与 sent 逐字节相同。
 * @retval false: 长度或内容不同（属于别的命令的应答）。
 */
bool echo_matches(std::span<const uint8_t> frame, size_t tag_pos, std::span<const uint8_t> sent) noexcept
{
    if (tag_pos < kCommandPrefix.size()) {
        return false;
    }
    const size_t sent_len = ascii::strip_line_end(sent);
    if ((tag_pos - kCommandPrefix.size()) != sent_len) {
        return false;
    }
    return ascii::bytes_starts_with(frame.subspan(kCommandPrefix.size()), sent.first(sent_len));
}

/**
 * @brief 取应答文本（标签之后到 '*' 之前，去掉尾部空格）。
 * @param frame: 整帧字节。
 * @param tag_pos: ",response: " 的起始下标。
 * @retval span: 应答文本（可能为空）。
 */
std::span<const uint8_t> response_text(std::span<const uint8_t> frame, size_t tag_pos) noexcept
{
    if (frame.size() < kStarFromEnd) {
        return {};
    }
    const size_t begin = tag_pos + kResponseTag.size();
    size_t end = frame.size() - kStarFromEnd;
    while ((end > begin) && (frame[end - 1U] == static_cast<uint8_t>(' '))) {
        --end;
    }
    if ((end <= begin) || (begin >= frame.size())) {
        return {};
    }
    return frame.subspan(begin, end - begin);
}

} // namespace resp

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
 * @brief 构造“还需要更多字节”结论。
 * @param need: 至少还差多少字节。
 * @retval verdict: step = kNeedMore。
 */
ResponseVerdict need_more(std::size_t need) noexcept
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
 * @brief 检查 "$command," 前缀与最小长度。
 * @param frame: 已收到的字节。
 * @retval std::nullopt: 前缀完整且长度够，可继续看正文。
 * @retval verdict: 需要继续收取或直接拒绝。
 */
std::optional<ResponseVerdict> check_prefix(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (size == 0U) {
        return reject(ResponseReason::kEmpty);
    }
    const size_t prefix_len = kCommandPrefix.size();
    const size_t compared = size < prefix_len ? size : prefix_len;
    for (size_t i = 0; i < compared; ++i) {
        if (frame[i] != static_cast<uint8_t>(kCommandPrefix[i])) {
            return reject(ResponseReason::kPrefix, i, frame[i]);
        }
    }
    if (size < prefix_len) {
        return need_more(prefix_len - size);
    }
    if (size < (prefix_len + kMinReplyBytes)) {
        return need_more((prefix_len + kMinReplyBytes) - size);
    }
    return std::nullopt;
}

/**
 * @brief 校验和是否匹配（标准 XOR，或 UM980 实测把 '$' 一并计入的变体）。
 * @param frame: 整帧字节。
 * @param star_pos: '*' 的下标。
 * @param received: 输出，帧内写的校验值。
 * @param computed: 输出，按报文体算出的校验值。
 * @retval true: 二者任一一致。
 * @retval false: 都不一致。
 */
bool checksum_matches(std::span<const uint8_t> frame, std::size_t star_pos, uint8_t &received,
                      uint8_t &computed) noexcept
{
    computed = ascii::xor_checksum(frame, 1U, star_pos);
    received = static_cast<uint8_t>((ascii::hex_value(frame[star_pos + 1U]) << ascii::kHexShift) |
                                    ascii::hex_value(frame[star_pos + 2U]));
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
std::optional<ResponseVerdict> check_full_frame(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (!ascii::has_hex_tail(frame)) {
        return reject(ResponseReason::kTail, size - 1U, frame[size - 1U]);
    }
    const size_t star_pos = size - kStarFromEnd;
    for (size_t i = 1U; i < star_pos; ++i) {
        if (!ascii::is_body_byte(frame[i])) {
            return reject(ResponseReason::kBody, i, frame[i]);
        }
    }
    uint8_t received = 0;
    uint8_t computed = 0;
    if (!checksum_matches(frame, star_pos, received, computed)) {
        return reject(ResponseReason::kChecksum, star_pos, received, computed);
    }
    return std::nullopt;
}

/**
 * @brief 尚未收到 '\n'：在末尾 5 字节窗口里找 '*'，据此判断补几个字节。
 * @param frame: 已收到的字节（非空）。
 * @retval std::nullopt: 不会出现（仅为与另外两个检查函数同签名）。
 * @retval verdict: 需要继续收取或直接拒绝。
 */
std::optional<ResponseVerdict> check_partial_frame(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    size_t star_pos = size;
    const size_t window = size < kStarFromEnd ? size : kStarFromEnd;
    for (size_t k = 0; k < window; ++k) {
        if (frame[size - 1U - k] == kStar) {
            star_pos = size - 1U - k;
            break;
        }
    }
    if (star_pos == size) {
        // 没有 '*' 时只校验最后一字节是否为合法报文体，避免把半个多字节字符判成帧。
        if (!ascii::is_body_byte(frame[size - 1U])) {
            return reject(ResponseReason::kBody, size - 1U, frame[size - 1U]);
        }
        return need_more(1U);
    }
    if ((size - star_pos) > kStarFromEnd) {
        return reject(ResponseReason::kStarField, star_pos);
    }
    for (size_t i = star_pos + 1U; i < size; ++i) {
        const uint8_t c = frame[i];
        if (!ascii::is_hex(c) && c != kCr && c != kLf) {
            return reject(ResponseReason::kStarByte, i, c);
        }
    }
    if ((size - star_pos) >= kStarFromEnd) {
        return reject(ResponseReason::kNoRoom, star_pos);
    }
    return need_more(kStarFromEnd - (size - star_pos));
}

/**
 * @brief 统一的帧判定入口：explain 与 step 共用，保证诊断结论与实际成帧一致。
 * @param frame: 已收到的字节。
 * @retval verdict: kFrame / kNeedMore / kReject 之一。
 */
ResponseVerdict judge(std::span<const uint8_t> frame) noexcept
{
    if (const std::optional<ResponseVerdict> head = check_prefix(frame)) {
        return *head;
    }
    if (frame[frame.size() - 1U] == kLf) {
        if (const std::optional<ResponseVerdict> tail = check_full_frame(frame)) {
            return *tail;
        }
        return complete();
    }
    if (const std::optional<ResponseVerdict> partial = check_partial_frame(frame)) {
        return *partial;
    }
    return complete();
}

/** 把判定原因写成一句英文（explain 契约）。 */
void explain_verdict(const ResponseVerdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case ResponseReason::kEmpty:
        text.literal("empty prefix");
        break;
    case ResponseReason::kPrefix:
        text.literal("byte ").decimal(verdict.offset).literal(" is ");
        text.byte_value(verdict.received).literal(", expected ");
        text.byte_value(static_cast<uint8_t>(kCommandPrefix[verdict.offset]));
        break;
    case ResponseReason::kTail:
        text.literal("missing \"*hh\\r\\n\" tail before byte ").decimal(verdict.offset).literal(" (");
        text.byte_value(verdict.received).literal(")");
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
        text.literal("'*' at offset ").decimal(verdict.offset).literal(" but fewer than two checksum digits");
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

/** 把型号与固件版本写进探测结果（固件文本截断到 kVersionLen-1）。 */
void fill_probe_result(Model model, std::span<const uint8_t> fw_text, ProbeResult &out) noexcept
{
    out = ProbeResult{};
    out.model = model;
    const size_t copy = fw_text.size() < (kVersionLen - 1) ? fw_text.size() : (kVersionLen - 1);
    for (size_t i = 0; i < copy; ++i) {
        out.fw_version[i] = static_cast<char>(fw_text[i]);
    }
    out.fw_version[copy] = '\0';
}

/** 版本文本 → 型号：UM982 双天线、UM981 单天线带 IMU，其余 UM9xx 按 UM980 能力处理（ADR-0010 §2.2）。 */
std::optional<Model> model_from_text(std::span<const uint8_t> text) noexcept
{
    if (ascii::quoted_equals(text, "UM982")) {
        return Model::kUm982;
    }
    if (ascii::quoted_equals(text, "UM981")) {
        return Model::kUm981;
    }
    if (ascii::quoted_equals(text, "UM980") || ascii::quoted_starts_with(text, "UM9")) {
        return Model::kUm980;
    }
    return std::nullopt;
}

/** 脚本步骤构造函数：把第 index 步写进 out；返回 false 表示脚本已结束。 */
using StepFn = bool (*)(const RoverConfig &cfg, ConfigStep &out) noexcept;
/** 步骤是否需要参与脚本（false = 该配置下跳过，索引映射随之顺延）。 */
using StepEnabledFn = bool (*)(const RoverConfig &cfg) noexcept;

struct ScriptStep {
    StepFn build;
    StepEnabledFn enabled;
};

/** @brief 无条件参与。 */
bool always(const RoverConfig &cfg) noexcept
{
    static_cast<void>(cfg);
    return true;
}

/** @brief 仅在要求改波特率时参与（port_baud == 0 时脚本提前结束）。 */
bool port_baud_enabled(const RoverConfig &cfg) noexcept
{
    return cfg.port_baud != 0U;
}

/**
 * @brief 第 0 步：先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。
 * @param cfg: 流动站配置（用端口号）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool step_unlog_all(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmd::unlog(out.cmd, cfg.port)) {
        return false;
    }
    out.expect_reply = true;
    out.timeout_ms = kConfigTimeoutMs;
    out.switch_baud = 0;
    return true;
}

/**
 * @brief 第 1 步：切到流动站模式。
 * @param cfg: 流动站配置（本步不使用）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool step_mode_rover(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    static_cast<void>(cfg);
    if (!cmd::mode_rover(out.cmd)) {
        return false;
    }
    out.expect_reply = true;
    out.timeout_ms = kConfigTimeoutMs;
    out.switch_baud = 0;
    return true;
}

/**
 * @brief 第 2 步：改端口波特率；手册要求切换后不再按旧速率等待应答。
 * @param cfg: 流动站配置（用端口号与目标波特率）。
 * @param out: 输出步骤（switch_baud = 目标波特率）。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool step_port_baud(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmd::config_baud(out.cmd, cfg.port, cfg.port_baud)) {
        return false;
    }
    out.expect_reply = false;
    out.timeout_ms = kConfigTimeoutMs;
    out.switch_baud = cfg.port_baud;
    return true;
}

/** 基础脚本：顺序即探测/配置顺序，可选步骤用 enabled 表达。 */
constexpr std::array<ScriptStep, 3> kRoverScript{{
    {&step_unlog_all, &always},
    {&step_mode_rover, &always},
    {&step_port_baud, &port_baud_enabled},
}};

} // namespace

bool response_first_byte(uint8_t byte) noexcept
{
    return byte == kDollar;
}

demux::Step response_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    const ResponseVerdict verdict = judge(frame);
    need = verdict.need;
    return verdict.step;
}

std::size_t explain_response(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    demux::text_writer::TextWriter text(out);
    if (frame.empty()) {
        return text.literal("empty prefix").finish();
    }
    const ResponseVerdict verdict = judge(frame);
    if (verdict.step == demux::Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explain_verdict(verdict, text);
    return text.finish();
}

bool probe_cmd(CmdBuf &out) noexcept
{
    return cmd::version_query(out);
}

bool probe_match(const demux::Decoder &dec, std::span<const uint8_t> frame, ProbeResult &out) noexcept
{
    if ((&dec != &demux::unicore_hash::kDecoder) || (frame.size() < kVersionTag.size()) ||
        !ascii::starts_with(frame, kVersionTag)) {
        return false;
    }
    size_t pos = kVersionTag.size();
    std::span<const uint8_t> model_text{};
    if (!ascii::next_quoted(frame, pos, model_text)) {
        return false;
    }
    const std::optional<Model> model = model_from_text(model_text);
    if (!model.has_value()) {
        return false;
    }
    std::span<const uint8_t> fw_text{};
    // 固件版本字段可选：旧固件没有它时只填型号。
    static_cast<void>(ascii::next_quoted(frame, pos, fw_text));
    fill_probe_result(*model, fw_text, out);
    return true;
}

bool script_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) noexcept
{
    static_cast<void>(model); // 基础脚本与型号无关；报文输出改由输出阶段（build_output）下发
    size_t position = 0;
    for (const ScriptStep &step : kRoverScript) {
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

Ack reply_ack(const demux::Decoder &dec, std::span<const uint8_t> frame, std::span<const uint8_t> sent) noexcept
{
    if (&dec != &kResponseDecoder) {
        return Ack::kNone;
    }
    if (!resp::has_valid_tail(frame)) {
        return Ack::kNone;
    }
    const std::optional<size_t> tag = resp::find_response_tag(frame);
    if (!tag.has_value() || !resp::echo_matches(frame, *tag, sent)) {
        return Ack::kNone; // 不是本步命令的应答
    }
    return ascii::quoted_equals(resp::response_text(frame, *tag), "OK") ? Ack::kAck : Ack::kNack;
}

uint32_t model_caps(Model model) noexcept
{
    switch (model) {
    case Model::kUm982:
        return cap::kRtcmIn | cap::kBase | cap::kHeading | cap::kRate20Hz;
    case Model::kUm981:
        return cap::kRtcmIn | cap::kBase | cap::kIns | cap::kRate20Hz;
    case Model::kUm980:
        return cap::kRtcmIn | cap::kBase | cap::kRate20Hz;
    default:
        return cap::kRtcmIn | cap::kBase; // 型号未知时给出保守能力
    }
}

} // namespace gnss::unicore

namespace gnss {

namespace {
constinit const UnicoreReceiver s_unicoreReceiver{};
}

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
    return unicore::kDecoders;
}

uint32_t UnicoreReceiver::caps(Model model) const noexcept
{
    return unicore::model_caps(model);
}

bool UnicoreReceiver::build_probe(CmdBuf &out) const noexcept
{
    return unicore::cmd::version_query(out);
}

bool UnicoreReceiver::match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  ProbeResult &out) const noexcept
{
    return unicore::probe_match(dec, frame, out);
}

bool UnicoreReceiver::rover_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) const noexcept
{
    return unicore::script_step(index, model, cfg, out);
}

Ack UnicoreReceiver::classify_reply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                    std::span<const uint8_t> sent) const noexcept
{
    return unicore::reply_ack(dec, frame, sent);
}

OutputMsg UnicoreReceiver::classify_frame(const demux::Decoder &dec, std::span<const uint8_t> frame) const noexcept
{
    // 厂家私有帧：其余（NMEA 等）交回基类，保持与 main 原 classify_gnss_sentence() 一致。
    if (detail::starts_with(frame, "#SATSINFOA,")) {
        return OutputMsg::kSatsInfo;
    }
    if (detail::starts_with(frame, "#HEADINGA,")) {
        return OutputMsg::kHeading;
    }
    return Receiver::classify_frame(dec, frame);
}

size_t UnicoreReceiver::frame_tag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  std::span<char> out) const noexcept
{
    const std::string_view name{dec.name};
    if (name == "UnicoreHash" && frame.size() > detail::kNmeaTagOffset) {
        return detail::tag_with_word(out, "UNICORE ", frame, detail::kNmeaTagOffset);
    }
    if (name == "UnicoreResp" && frame.size() > detail::kCommandTagOffset) {
        // "$command,<cmd>,response: ..."：命令名在第二个逗号之前。
        return detail::tag_with_word(out, "UNICORE_CMD ", frame, detail::kCommandTagOffset);
    }
    return Receiver::frame_tag(dec, frame, out);
}

uint32_t UnicoreReceiver::output_mask(Model model) const noexcept
{
    constexpr uint32_t kSatsInfoBit = 1U << static_cast<uint32_t>(OutputMsg::kSatsInfo);
    constexpr uint32_t kHeadingBit = 1U << static_cast<uint32_t>(OutputMsg::kHeading);
    constexpr uint32_t kGstBit = 1U << static_cast<uint32_t>(OutputMsg::kGst);
    // UM9xx 都能输出 GPGST（旧脚本一直下发 GPGST），基线基类默认只含 GGA/RMC/GSA/GSV。
    uint32_t mask = Receiver::output_mask(model) | kSatsInfoBit | kGstBit;
    if ((unicore::model_caps(model) & cap::kHeading) != 0U) {
        mask |= kHeadingBit; // 仅双天线型号（UM982）有 HEADINGA
    }
    return mask;
}

fw::Status UnicoreReceiver::build_output(OutputMsg msg, uint32_t period_ms, Model model, const OutputPlan &plan,
                                         CmdBuf &out) const noexcept
{
    const auto idx = static_cast<size_t>(static_cast<uint8_t>(msg));
    if ((msg == OutputMsg::kNone) || (idx >= unicore::kOutputNames.size())) {
        return fw::Status::kInvalidArg;
    }
    if ((msg == OutputMsg::kHeading) && ((unicore::model_caps(model) & cap::kHeading) == 0U)) {
        return fw::Status::kNotSupported; // 单天线型号没有 HEADINGA
    }
    const std::string_view name{unicore::kOutputNames[idx]};
    // period 0 = 关闭该类；旧 apply 旁路同样用 "UNLOG COMn <name>"。
    const bool written = (period_ms == 0U) ? unicore::cmd::unlog_msg(out, name, plan.port)
                                           : unicore::cmd::log_msg(out, name, plan.port, period_ms);
    if (!written) {
        out.clear();
        return fw::Status::kInvalidArg;
    }
    return fw::Status::kOk;
}

const Receiver &unicore_receiver() noexcept
{
    return s_unicoreReceiver;
}

} // namespace gnss
