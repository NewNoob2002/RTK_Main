// ComNav K8（K803）驱动实现：NovAtel 风格命令与 "<OK/<ERROR" 应答（ADR-0010 §2.2，实验性、未经实物验证）。
// 线程/调用约束：纯函数，无全局可变状态，可重入、不阻塞。
// 堆：不使用。注意：未取得官方手册，语法按公开的 NovAtel 兼容资料实现；改动前须先修订 ADR-0010 §2.2。
#include "gnss/drivers/ComnavReceiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/TextWriter.hpp"
#include "demux/decoders/UnicoreHash.hpp"

namespace gnss::comnav {
namespace {

constexpr uint8_t kPrompt = static_cast<uint8_t>('<');
constexpr uint8_t kCr = static_cast<uint8_t>('\r');
constexpr uint8_t kLf = static_cast<uint8_t>('\n');
constexpr std::string_view kVersionTag = "#VERSIONA,";
// 文本协议常量（与 Unicore.cpp 同名同义，见那里的说明）。
constexpr uint8_t kPrintableMin = 0x20U;
constexpr uint8_t kPrintableMax = 0x7EU;
constexpr uint8_t kDecimalBase = 10U;
constexpr uint8_t kUintDigits = 10U;
constexpr std::size_t kMinPromptBytes = 4U; // 最短有效应答 "<x\r\n"
constexpr uint32_t kConfigTimeoutMs = 500U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint32_t kCentiPerSecond = 100U;
/** 命令周期的最小粒度：接收机只接受 10 ms 整数倍。 */
constexpr uint32_t kMillisPerCentiSecond = 10U;

// 报文名字（下标 = OutputMsg）。ComNav 只支持这 4 类 NMEA。
constexpr std::array<const char *, kOutputMsgCount> kOutputNames = {
    "GPGGA", "SATSINFOA", "HEADINGA", "GPGST", "GPRMC", "GPGSA", "GPGSV",
};

// <OK 提示符应答（match_reply）与 #VERSIONA 探测应答（match_probe）各需一个解码器。
constexpr std::array<const demux::Decoder *, 2> kDecoders{&kPromptDecoder, &demux::unicore_hash::kDecoder};

bool is_printable(uint8_t c) noexcept
{
    return c >= kPrintableMin && c <= kPrintableMax;
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

// 把周期毫秒写成人可读秒文本（最多两位小数，如 0.05/0.2/1/5）：ONTIME 只接受 10 ms 整数倍。
bool append_period(CmdBuf &out, uint32_t period_ms) noexcept
{
    if ((period_ms % kMillisPerCentiSecond) != 0U) {
        return false;
    }
    const uint32_t whole = period_ms / kMsPerSecond;
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

bool starts_with(std::span<const uint8_t> text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (text[i] != static_cast<uint8_t>(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool next_quoted(std::span<const uint8_t> frame, size_t &pos, std::span<const uint8_t> &out) noexcept
{
    while (pos < frame.size() && frame[pos] != static_cast<uint8_t>('"')) {
        if (frame[pos] == static_cast<uint8_t>('*')) {
            return false;
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

/** 应答行的判定原因，explain 与 step 共用同一份判定逻辑。 */
enum class PromptReason : uint8_t {
    kNone = 0,
    kEmpty,
    kNoCr,
    kBody,
};

struct PromptVerdict {
    demux::Step step = demux::Step::kReject;
    PromptReason reason = PromptReason::kNone;
    std::size_t need = 0;
    std::size_t offset = 0;
    uint8_t byte = 0;
};

PromptVerdict judge(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (size == 0) {
        PromptVerdict verdict{};
        verdict.reason = PromptReason::kEmpty;
        return verdict;
    }
    if (frame[size - 1] == kLf) {
        if (size < kMinPromptBytes || frame[size - 2] != kCr) {
            PromptVerdict verdict{};
            verdict.reason = PromptReason::kNoCr;
            verdict.offset = size - 1;
            verdict.byte = frame[size - 1];
            return verdict;
        }
        for (size_t i = 1; i + 2 < size; ++i) {
            if (!is_printable(frame[i])) {
                PromptVerdict verdict{};
                verdict.reason = PromptReason::kBody;
                verdict.offset = i;
                verdict.byte = frame[i];
                return verdict;
            }
        }
        PromptVerdict verdict{};
        verdict.step = demux::Step::kFrame;
        return verdict;
    }
    // 末字节是 '\r' 时只差 '\n'（分块读取常在 CR 后断开），仍属有效前缀。
    if (!is_printable(frame[size - 1]) && frame[size - 1] != kCr) {
        PromptVerdict verdict{};
        verdict.reason = PromptReason::kBody;
        verdict.offset = size - 1;
        verdict.byte = frame[size - 1];
        return verdict;
    }
    PromptVerdict verdict{};
    verdict.step = demux::Step::kNeedMore;
    verdict.need = 1;
    return verdict;
}

void explain_verdict(const PromptVerdict &verdict, demux::text_writer::TextWriter &text) noexcept
{
    switch (verdict.reason) {
    case PromptReason::kEmpty:
        text.literal("empty prefix");
        break;
    case PromptReason::kNoCr:
        text.literal("no CR before LF at offset ").decimal(verdict.offset);
        break;
    case PromptReason::kBody:
        text.literal("illegal byte ")
            .hex(static_cast<uint32_t>(verdict.byte), 2U)
            .literal(" at offset ")
            .decimal(verdict.offset);
        break;
    case PromptReason::kNone:
    default:
        text.literal("rejected");
        break;
    }
}

} // namespace

bool prompt_first_byte(uint8_t byte) noexcept
{
    return byte == kPrompt;
}

demux::Step prompt_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    const PromptVerdict verdict = judge(frame);
    need = verdict.need;
    return verdict.step;
}

std::size_t explain_prompt(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    demux::text_writer::TextWriter text(out);
    if (frame.empty()) {
        return text.literal("empty prefix").finish();
    }
    const PromptVerdict verdict = judge(frame);
    if (verdict.step == demux::Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explain_verdict(verdict, text);
    return text.finish();
}

bool probe_cmd(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("LOG VERSIONA ONCE\r\n");
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
    if (!next_quoted(frame, pos, model_text) || !starts_with(model_text, "K803")) {
        return false;
    }
    ProbeResult result{};
    result.model = Model::kK803;
    out = result;
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

bool script_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) noexcept
{
    (void)model; // 目前只覆盖 K803；报文输出改由输出阶段（build_output）下发
    CmdBuf cmd;
    size_t next = 0;

    if (index == next) {
        // 先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。
        return cmd.append("UNLOGALL COM") && append_uint(cmd, cfg.port) && cmd.append("\r\n") &&
               finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return cmd.append("MODE ROVER\r\n") && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (cfg.port_baud != 0 && index == next) {
        // 切换波特率后不再按旧速率等待应答。
        return cmd.append("COM COM") && append_uint(cmd, cfg.port) && cmd.append(" ") &&
               append_uint(cmd, cfg.port_baud) && cmd.append("\r\n") && finish_step(cmd, false, cfg.port_baud, out);
    }
    return false; // 脚本结束
}

Ack reply_ack(const demux::Decoder &dec, std::span<const uint8_t> frame, std::span<const uint8_t>) noexcept
{
    if (&dec != &kPromptDecoder) {
        return Ack::kNone;
    }
    if (starts_with(frame, "<OK")) {
        return Ack::kAck;
    }
    if (starts_with(frame, "<ERROR")) {
        return Ack::kNack;
    }
    return Ack::kNone;
}

uint32_t model_caps(Model model) noexcept
{
    if (model == Model::kK803) {
        return cap::kRtcmIn | cap::kBase;
    }
    return cap::kRtcmIn | cap::kBase;
}

} // namespace gnss::comnav

namespace gnss {

namespace {
constinit const ComnavReceiver s_comnavReceiver{};
}

Family ComnavReceiver::family() const noexcept
{
    return Family::kComnav;
}

std::string_view ComnavReceiver::name() const noexcept
{
    return "comnav";
}

std::span<const demux::Decoder *const> ComnavReceiver::decoders() const noexcept
{
    return comnav::kDecoders;
}

uint32_t ComnavReceiver::caps(Model model) const noexcept
{
    return comnav::model_caps(model);
}

bool ComnavReceiver::build_probe(CmdBuf &out) const noexcept
{
    return comnav::probe_cmd(out);
}

bool ComnavReceiver::match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                 ProbeResult &out) const noexcept
{
    return comnav::probe_match(dec, frame, out);
}

bool ComnavReceiver::rover_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) const noexcept
{
    return comnav::script_step(index, model, cfg, out);
}

Ack ComnavReceiver::classify_reply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                   std::span<const uint8_t> sent) const noexcept
{
    return comnav::reply_ack(dec, frame, sent);
}

uint32_t ComnavReceiver::output_mask(Model model) const noexcept
{
    // K803 只能输出这四类 NMEA；SATSINFOA/HEADINGA/GST 一律 kNotSupported（与旧脚本一致）。
    constexpr uint32_t kGgaBit = 1U << static_cast<uint32_t>(OutputMsg::kGga);
    constexpr uint32_t kRmcBit = 1U << static_cast<uint32_t>(OutputMsg::kRmc);
    constexpr uint32_t kGsaBit = 1U << static_cast<uint32_t>(OutputMsg::kGsa);
    constexpr uint32_t kGsxBit = 1U << static_cast<uint32_t>(OutputMsg::kGsv);
    static_cast<void>(model); // 单型号
    return kGgaBit | kRmcBit | kGsaBit | kGsxBit;
}

fw::Status ComnavReceiver::build_output(OutputMsg msg, uint32_t period_ms, Model model, const OutputPlan &plan,
                                        CmdBuf &out) const noexcept
{
    out.clear();
    const auto idx = static_cast<size_t>(static_cast<uint8_t>(msg));
    if ((msg == OutputMsg::kNone) || (idx >= comnav::kOutputNames.size())) {
        return fw::Status::kInvalidArg;
    }
    if ((output_mask(model) & (1U << static_cast<uint32_t>(msg))) == 0U) {
        return fw::Status::kNotSupported;
    }
    const char *name = comnav::kOutputNames[idx];
    if (period_ms == 0U) {
        // 关闭该类：UNLOG COMn <name>（与旧 apply 旁路一致）。
        if (!out.append("UNLOG COM") || !comnav::append_uint(out, plan.port) || !out.append(" ") ||
            !out.append(std::string_view{name}) || !out.append("\r\n")) {
            return fw::Status::kInvalidArg;
        }
        return fw::Status::kOk;
    }
    if (!out.append("LOG COM") || !comnav::append_uint(out, plan.port) || !out.append(" ") ||
        !out.append(std::string_view{name}) || !out.append(" ONTIME ") || !comnav::append_period(out, period_ms) ||
        !out.append("\r\n")) {
        out.clear();
        return fw::Status::kInvalidArg;
    }
    return fw::Status::kOk;
}

const Receiver &comnav_receiver() noexcept
{
    return s_comnavReceiver;
}

} // namespace gnss
