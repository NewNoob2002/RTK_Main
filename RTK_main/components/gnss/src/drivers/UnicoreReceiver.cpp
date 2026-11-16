// Unicore N4（UM980/UM981/UM982）驱动实现：$command 应答解码、VERSIONA 探测、流动站配置脚本（ADR-0010 §2.2/§2.4）。
// 线程/调用约束：全部为纯函数（无全局可变状态），可从任意任务调用，不可阻塞；应答解码器亦可用于 ISR 安全的
// demux 引擎（first_byte/step 只读入参）。
// 堆：不使用。浮点：不使用（频率参数按手册写死为文本）。
#include "gnss/drivers/UnicoreReceiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/TextWriter.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "../FrameTag.hpp"

namespace gnss::unicore {
namespace {

constexpr uint8_t kDollar = static_cast<uint8_t>('$');
constexpr uint8_t kStar = static_cast<uint8_t>('*');
constexpr uint8_t kLf = static_cast<uint8_t>('\n');
constexpr uint8_t kCr = static_cast<uint8_t>('\r');
constexpr std::string_view kCommandPrefix = "$command,";
constexpr std::string_view kResponseTag = ",response: ";
constexpr std::string_view kVersionTag = "#VERSIONA,";
// 文本协议常量：可打印区间、十进制与十六进制的基数/位数、应答尾 "*hh\r\n" 的长度等，避免散落魔法数字。
constexpr uint8_t kPrintableMin = 0x20U;
constexpr uint8_t kPrintableMax = 0x7EU;
constexpr uint8_t kDecimalBase = 10U;
constexpr uint8_t kUintDigits = 10U;
constexpr uint8_t kHexDigitOffset = 10U; // 'A' 的数值
constexpr uint8_t kHexShift = 4U;
constexpr std::size_t kMinReplyBytes = 6U; // 最短应答 ",OK*" 一类
constexpr std::size_t kTailBytes = 5U;     // "*hh\r\n"
constexpr uint32_t kConfigTimeoutMs = 500U;

// $command 应答（match_reply）与 #VERSIONA 探测应答（match_probe）各需一个解码器，缺一则对应帧落入 unknown。
constexpr std::array<const demux::Decoder *, 2> kDecoders{&kResponseDecoder, &demux::unicore_hash::kDecoder};

bool is_hex(uint8_t c) noexcept
{
    return (c >= static_cast<uint8_t>('0') && c <= static_cast<uint8_t>('9')) ||
           (c >= static_cast<uint8_t>('A') && c <= static_cast<uint8_t>('F')) ||
           (c >= static_cast<uint8_t>('a') && c <= static_cast<uint8_t>('f'));
}

uint8_t hex_value(uint8_t c) noexcept
{
    if (c >= static_cast<uint8_t>('0') && c <= static_cast<uint8_t>('9')) {
        return static_cast<uint8_t>(c - static_cast<uint8_t>('0'));
    }
    if (c >= static_cast<uint8_t>('A') && c <= static_cast<uint8_t>('F')) {
        return static_cast<uint8_t>(c - static_cast<uint8_t>('A') + kHexDigitOffset);
    }
    return static_cast<uint8_t>(c - static_cast<uint8_t>('a') + kHexDigitOffset);
}

bool is_body_char(uint8_t c) noexcept
{
    return c >= kPrintableMin && c <= kPrintableMax && c != kDollar && c != kStar;
}

bool append_uint(CmdBuf &out, uint32_t value) noexcept
{
    std::array<uint8_t, kUintDigits> digits{};
    size_t count = 0;
    while (value != 0 && count < digits.size()) {
        digits[count] = static_cast<uint8_t>(static_cast<uint32_t>('0') + (value % kDecimalBase));
        ++count;
        value /= kDecimalBase;
    }
    for (size_t i = count; i > 0; --i) {
        if (!out.append(std::span<const uint8_t>(&digits[i - 1], 1))) {
            return false;
        }
    }
    return true;
}

// 报文体名字（下标 = OutputMsg）：与大写常量同名，便于与手册/日志对照。
constexpr std::array<const char *, kOutputMsgCount> kOutputNames = {
    "GPGGA", "SATSINFOA", "HEADINGA", "GPGST", "GPRMC", "GPGSA", "GPGSV",
};

constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint32_t kCentiPerSecond = 100U;
/** 响应尾部：最少的 `*hh` 字节数，以及 '*' 距帧尾的偏移（沿用旧实现，ADR-0010）。 */
constexpr size_t kMinTailBytes = 3U;
constexpr size_t kStarFromEnd = 5U;
/** 命令周期的最小粒度：接收机只接受 10 ms 整数倍。 */
constexpr uint32_t kMillisPerCentiSecond = 10U;

// 把周期毫秒写成人可读秒文本（最多两位小数，如 0.05/0.2/1/5）：Unicore 只接受 10 ms 整数倍。
bool append_period(CmdBuf &out, uint32_t period_ms) noexcept
{
    if ((period_ms % kMillisPerCentiSecond) != 0U) {
        return false;
    }
    const uint32_t whole = period_ms / kMsPerSecond;
    // 四舍五入到百分位：period_ms 已是 10 ms 倍数，centi 必为整数。
    const uint32_t centi = (period_ms % kMsPerSecond) / 10U;
    if (whole == 0U) {
        if (!out.append("0")) {
            return false;
        }
    } else if (!append_uint(out, whole)) {
        return false;
    }
    if (centi == 0U) {
        return true;
    }
    if (!out.append(".")) {
        return false;
    }
    // 只写有效数字：100 ms 写 "0.1"、500 ms 写 "0.5"，50 ms 写 "0.05"（与基线脚本一致）。
    if ((centi % kMillisPerCentiSecond) == 0U) {
        return append_uint(out, centi / kMillisPerCentiSecond);
    }
    if ((centi < kMillisPerCentiSecond) && !out.append("0")) {
        return false;
    }
    return append_uint(out, centi);
}

// 取 frame 中 pos 之后的第一个引号字段；成功时 out 指向引号内文本，pos 移到结束引号之后。
bool next_quoted(std::span<const uint8_t> frame, size_t &pos, std::span<const uint8_t> &out) noexcept
{
    while (pos < frame.size() && frame[pos] != static_cast<uint8_t>('"')) {
        if (frame[pos] == kStar) {
            return false; // 到校验和仍未出现型号字段
        }
        ++pos;
    }
    if (pos >= frame.size()) {
        return false;
    }
    ++pos;
    const size_t start = pos;
    while (pos < frame.size() && frame[pos] != static_cast<uint8_t>('"')) {
        ++pos;
    }
    if (pos >= frame.size()) {
        return false;
    }
    out = frame.subspan(start, pos - start);
    ++pos;
    return true;
}

bool quoted_equals(std::span<const uint8_t> field, std::string_view text) noexcept
{
    if (field.size() != text.size()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (field[i] != static_cast<uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

bool quoted_starts_with(std::span<const uint8_t> field, std::string_view text) noexcept
{
    if (field.size() < text.size()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (field[i] != static_cast<uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

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

ResponseVerdict judge(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (size == 0) {
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
        ResponseVerdict verdict{};
        verdict.step = demux::Step::kNeedMore;
        verdict.need = prefix_len - size;
        return verdict;
    }
    if (size < prefix_len + kMinReplyBytes) { // 至少还要 ",OK*" 一类的最小应答
        ResponseVerdict verdict{};
        verdict.step = demux::Step::kNeedMore;
        verdict.need = (prefix_len + kMinReplyBytes) - size;
        return verdict;
    }

    if (frame[size - 1] == kLf) {
        const size_t tail = size - kTailBytes; // '*' 的位置
        if (size < prefix_len + kMinReplyBytes || frame[tail] != kStar || !is_hex(frame[tail + 1U]) ||
            !is_hex(frame[tail + 2U]) || frame[size - 2U] != kCr) {
            return reject(ResponseReason::kTail, size - 1, frame[size - 1]);
        }
        uint8_t checksum = 0;
        for (size_t i = 1; i < tail; ++i) {
            if (!is_body_char(frame[i])) {
                return reject(ResponseReason::kBody, i, frame[i]);
            }
            checksum = static_cast<uint8_t>(checksum ^ frame[i]);
        }
        const uint8_t expected =
            static_cast<uint8_t>((hex_value(frame[tail + 1U]) << kHexShift) | hex_value(frame[tail + 2U]));
        // UM980 R4.10Build11833 实测把 '$' 也算进 $command 校验（"$command,VERSIONA,response: OK*45"，
        // 标准 XOR 为 0x61）；SparkFun Unicore 库对旧固件有同样的兼容（Unicore/src 内 "improperly adding the '$'"）。
        const bool standard = checksum == expected;
        const bool with_dollar = static_cast<uint8_t>(checksum ^ kDollar) == expected;
        if (!standard && !with_dollar) {
            return reject(ResponseReason::kChecksum, tail, expected, checksum);
        }
        ResponseVerdict verdict{};
        verdict.step = demux::Step::kFrame;
        return verdict;
    }

    // 尚未收到 '\n'：在最后 5 字节窗口里找 '*'，据此判定还差多少字节。
    size_t star_pos = size;
    const size_t window = size < kTailBytes ? size : kTailBytes;
    for (size_t k = 0; k < window; ++k) {
        if (frame[size - 1 - k] == kStar) {
            star_pos = size - 1 - k;
            break;
        }
    }
    if (star_pos != size) {
        const size_t after = size - star_pos - 1;
        if (after + 1U > kTailBytes) {
            return reject(ResponseReason::kStarField, star_pos);
        }
        for (size_t i = star_pos + 1; i < size; ++i) {
            const uint8_t c = frame[i];
            if (!is_hex(c) && c != kCr && c != kLf) {
                return reject(ResponseReason::kStarByte, i, c);
            }
        }
        const size_t missing = kTailBytes - (size - star_pos);
        if (missing == 0) {
            return reject(ResponseReason::kNoRoom, star_pos);
        }
        ResponseVerdict verdict{};
        verdict.step = demux::Step::kNeedMore;
        verdict.need = missing;
        return verdict;
    }

    if (!is_body_char(frame[size - 1])) {
        return reject(ResponseReason::kBody, size - 1, frame[size - 1]);
    }
    ResponseVerdict verdict{};
    verdict.step = demux::Step::kNeedMore;
    verdict.need = 1;
    return verdict;
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
    out.clear();
    return out.append("VERSIONA\r\n");
}

bool probe_match(const demux::Decoder &dec, std::span<const uint8_t> frame, ProbeResult &out) noexcept
{
    if (&dec != &demux::unicore_hash::kDecoder || frame.size() < kVersionTag.size()) {
        return false;
    }
    for (size_t i = 0; i < kVersionTag.size(); ++i) {
        if (frame[i] != static_cast<uint8_t>(kVersionTag[i])) {
            return false;
        }
    }

    size_t pos = kVersionTag.size();
    std::span<const uint8_t> model_text{};
    if (!next_quoted(frame, pos, model_text)) {
        return false;
    }
    Model model = Model::kUnknown;
    if (quoted_equals(model_text, "UM981")) {
        model = Model::kUm981;
    } else if (quoted_equals(model_text, "UM982")) {
        model = Model::kUm982;
    } else if (quoted_equals(model_text, "UM980") || quoted_starts_with(model_text, "UM9")) {
        // 其他 UM9xx（UM960/UM960L 等）按 UM980 能力处理（ADR-0010 §2.2）。
        model = Model::kUm980;
    } else {
        return false;
    }

    std::span<const uint8_t> fw_text{};
    const bool have_fw = next_quoted(frame, pos, fw_text);
    out = ProbeResult{};
    out.model = model;
    if (have_fw) {
        const size_t copy = fw_text.size() < (kVersionLen - 1) ? fw_text.size() : (kVersionLen - 1);
        for (size_t i = 0; i < copy; ++i) {
            out.fw_version[i] = static_cast<char>(fw_text[i]);
        }
        out.fw_version[copy] = '\0';
    }
    return true;
}

bool finish_step(CmdBuf &cmd, bool expect_reply, uint32_t switch_baud, ConfigStep &out) noexcept
{
    out.cmd = cmd;
    out.expect_reply = expect_reply;
    out.timeout_ms = kConfigTimeoutMs;
    out.switch_baud = switch_baud;
    return true;
}

uint32_t model_caps(Model model) noexcept;

bool script_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) noexcept
{
    (void)model; // 基础脚本与型号无关；HEADINGA/SATSINFOA 改由输出阶段（build_output）下发
    CmdBuf cmd;
    size_t next = 0;

    if (index == next) {
        // 先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。
        return cmd.append("UNLOG COM") && append_uint(cmd, cfg.port) && cmd.append("\r\n") &&
               finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return cmd.append("MODE ROVER\r\n") && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (cfg.port_baud != 0 && index == next) {
        // 手册要求切换波特率后不再按旧速率等待应答。
        return cmd.append("CONFIG COM") && append_uint(cmd, cfg.port) && cmd.append(" ") &&
               append_uint(cmd, cfg.port_baud) && cmd.append("\r\n") && finish_step(cmd, false, cfg.port_baud, out);
    }
    return false; // 脚本结束
}

Ack reply_ack(const demux::Decoder &dec, std::span<const uint8_t> frame, std::span<const uint8_t> sent) noexcept
{
    if (&dec != &kResponseDecoder) {
        return Ack::kNone;
    }
    const size_t size = frame.size();
    const size_t prefix_len = kCommandPrefix.size();
    if (size < prefix_len + kResponseTag.size() + kMinTailBytes || frame[size - 1] != kLf || frame[size - 2] != kCr ||
        frame[size - kStarFromEnd] != kStar) {
        return Ack::kNone;
    }

    size_t sep = size;
    for (size_t i = prefix_len; i + kResponseTag.size() <= size; ++i) {
        bool match = true;
        for (size_t k = 0; k < kResponseTag.size(); ++k) {
            if (frame[i + k] != static_cast<uint8_t>(kResponseTag[k])) {
                match = false;
                break;
            }
        }
        if (match) {
            sep = i;
            break;
        }
    }
    if (sep == size) {
        return Ack::kNone;
    }

    size_t sent_len = sent.size();
    while (sent_len > 0 && (sent[sent_len - 1] == kCr || sent[sent_len - 1] == kLf)) {
        --sent_len;
    }
    const size_t echo_len = sep - prefix_len;
    if (echo_len != sent_len) {
        return Ack::kNone;
    }
    for (size_t i = 0; i < sent_len; ++i) {
        if (frame[prefix_len + i] != sent[i]) {
            return Ack::kNone; // 不是本步命令的应答
        }
    }

    size_t text_begin = sep + kResponseTag.size();
    size_t text_end = size - kStarFromEnd; // '*' 之前
    while (text_end > text_begin && frame[text_end - 1] == static_cast<uint8_t>(' ')) {
        --text_end;
    }
    const std::span<const uint8_t> text = frame.subspan(text_begin, text_end - text_begin);
    return quoted_equals(text, "OK") ? Ack::kAck : Ack::kNack;
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
    return unicore::probe_cmd(out);
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
    out.clear();
    const auto idx = static_cast<size_t>(static_cast<uint8_t>(msg));
    if ((msg == OutputMsg::kNone) || (idx >= unicore::kOutputNames.size())) {
        return fw::Status::kInvalidArg;
    }
    if ((msg == OutputMsg::kHeading) && ((unicore::model_caps(model) & cap::kHeading) == 0U)) {
        return fw::Status::kNotSupported; // 单天线型号没有 HEADINGA
    }
    const char *name = unicore::kOutputNames[idx];
    if (!out.append(name)) {
        return fw::Status::kInvalidArg;
    }
    if (period_ms == 0U) {
        // 关闭该类：UNLOG COMn <name>（与旧 apply 旁路一致）。
        out.clear();
        if (!out.append("UNLOG COM") || !unicore::append_uint(out, plan.port) || !out.append(" ") ||
            !out.append(std::string_view{name}) || !out.append("\r\n")) {
            return fw::Status::kInvalidArg;
        }
        return fw::Status::kOk;
    }
    if (!out.append(" COM") || !unicore::append_uint(out, plan.port) || !out.append(" ") ||
        !unicore::append_period(out, period_ms) || !out.append("\r\n")) {
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
