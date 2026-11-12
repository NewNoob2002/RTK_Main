// Unicore N4（UM980/UM981/UM982）方言实现：$command 应答解码、VERSIONA 探测、流动站配置脚本（ADR-0010 §2.2/§2.4）。
// 线程/调用约束：全部为纯函数（无全局可变状态），可从任意任务调用，不可阻塞；应答解码器亦可用于 ISR 安全的
// demux 引擎（first_byte/step 只读入参）。
// 堆：不使用。浮点：不使用（频率参数按手册写死为文本）。
#include "gnss/dialects/unicore.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/decoders/unicore_hash.hpp"

namespace gnss::unicore {
namespace {

constexpr uint8_t kDollar = static_cast<uint8_t>('$');
constexpr uint8_t kStar = static_cast<uint8_t>('*');
constexpr uint8_t kLf = static_cast<uint8_t>('\n');
constexpr uint8_t kCr = static_cast<uint8_t>('\r');
constexpr std::string_view kCommandPrefix = "$command,";
constexpr std::string_view kResponseTag = ",response: ";
constexpr std::string_view kVersionTag = "#VERSIONA,";

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
        return static_cast<uint8_t>(c - static_cast<uint8_t>('A') + 10U);
    }
    return static_cast<uint8_t>(c - static_cast<uint8_t>('a') + 10U);
}

bool is_body_char(uint8_t c) noexcept
{
    return c >= 0x20U && c <= 0x7EU && c != kDollar && c != kStar;
}

bool append_uint(CmdBuf &out, uint32_t value) noexcept
{
    std::array<uint8_t, 10> digits{};
    size_t count = 0;
    do {
        digits[count] = static_cast<uint8_t>(static_cast<uint32_t>('0') + (value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0);
    for (size_t i = count; i > 0; --i) {
        if (!out.append(std::span<const uint8_t>(&digits[i - 1], 1))) {
            return false;
        }
    }
    return true;
}

// 频率参数按手册：1/2/5/10/20 Hz → "1"/"0.5"/"0.2"/"0.1"/"0.05"；不支持的值向下取到最近的频点。
const char *period_for(uint8_t rate_hz) noexcept
{
    if (rate_hz >= 20U) {
        return "0.05";
    }
    if (rate_hz >= 10U) {
        return "0.1";
    }
    if (rate_hz >= 5U) {
        return "0.2";
    }
    if (rate_hz >= 2U) {
        return "0.5";
    }
    return "1";
}

bool append_message(CmdBuf &out, const char *name, uint8_t port, const char *period) noexcept
{
    return out.append(std::string_view(name)) && out.append(" COM") && append_uint(out, port) && out.append(" ") &&
           out.append(std::string_view(period)) && out.append("\r\n");
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

} // namespace

bool response_first_byte(uint8_t byte) noexcept
{
    return byte == kDollar;
}

demux::Step response_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    const size_t size = frame.size();
    if (size == 0) {
        return demux::Step::kReject;
    }
    const size_t prefix_len = kCommandPrefix.size();
    const size_t compared = size < prefix_len ? size : prefix_len;
    for (size_t i = 0; i < compared; ++i) {
        if (frame[i] != static_cast<uint8_t>(kCommandPrefix[i])) {
            return demux::Step::kReject;
        }
    }
    if (size < prefix_len) {
        need = prefix_len - size;
        return demux::Step::kNeedMore;
    }
    if (size < prefix_len + 6) { // 至少还要 ",OK*" 一类的最小应答
        need = (prefix_len + 6) - size;
        return demux::Step::kNeedMore;
    }

    if (frame[size - 1] == kLf) {
        if (size < prefix_len + 6 || frame[size - 2] != kCr || frame[size - 5] != kStar || !is_hex(frame[size - 3]) ||
            !is_hex(frame[size - 4])) {
            return demux::Step::kReject;
        }
        uint8_t checksum = 0;
        for (size_t i = 1; i < size - 5; ++i) {
            if (!is_body_char(frame[i])) {
                return demux::Step::kReject;
            }
            checksum = static_cast<uint8_t>(checksum ^ frame[i]);
        }
        const uint8_t expected = static_cast<uint8_t>((hex_value(frame[size - 4]) << 4) | hex_value(frame[size - 3]));
        // UM980 R4.10Build11833 实测把 '$' 也算进 $command 校验（"$command,VERSIONA,response: OK*45"，
        // 标准 XOR 为 0x61）；SparkFun Unicore 库对旧固件有同样的兼容（Unicore/src 内 "improperly adding the '$'"）。
        const bool standard = checksum == expected;
        const bool with_dollar = static_cast<uint8_t>(checksum ^ kDollar) == expected;
        return (standard || with_dollar) ? demux::Step::kFrame : demux::Step::kReject;
    }

    // 尚未收到 '\n'：在最后 5 字节窗口里找 '*'，据此判定还差多少字节。
    size_t star_pos = size;
    const size_t window = size < 5 ? size : 5;
    for (size_t k = 0; k < window; ++k) {
        if (frame[size - 1 - k] == kStar) {
            star_pos = size - 1 - k;
            break;
        }
    }
    if (star_pos != size) {
        const size_t after = size - star_pos - 1;
        if (after > 4) {
            return demux::Step::kReject;
        }
        for (size_t i = star_pos + 1; i < size; ++i) {
            const uint8_t c = frame[i];
            if (!is_hex(c) && c != kCr && c != kLf) {
                return demux::Step::kReject;
            }
        }
        need = 5 - (size - star_pos);
        if (need == 0) {
            return demux::Step::kReject;
        }
        return demux::Step::kNeedMore;
    }

    if (!is_body_char(frame[size - 1])) {
        return demux::Step::kReject;
    }
    need = 1;
    return demux::Step::kNeedMore;
}

bool build_probe(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("VERSIONA\r\n");
}

bool match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame, ProbeResult &out) noexcept
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
    if (quoted_equals(model_text, "UM980")) {
        model = Model::kUm980;
    } else if (quoted_equals(model_text, "UM981")) {
        model = Model::kUm981;
    } else if (quoted_equals(model_text, "UM982")) {
        model = Model::kUm982;
    } else if (quoted_starts_with(model_text, "UM9")) {
        model = Model::kUm980; // 其他 UM9xx（UM960/UM960L 等）按 UM980 能力处理（ADR-0010 §2.2）
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
    out.timeout_ms = 500;
    out.switch_baud = switch_baud;
    return true;
}

bool rover_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) noexcept
{
    (void)model; // Unicore 配置脚本与型号无关
    const char *period = period_for(cfg.rate_hz);
    CmdBuf cmd;
    size_t next = 0;

    if (index == next) {
        return cmd.append("UNLOG COM") && append_uint(cmd, cfg.port) && cmd.append("\r\n") &&
               finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return append_message(cmd, "GPGGA", cfg.port, period) && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return append_message(cmd, "GPRMC", cfg.port, period) && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return append_message(cmd, "GPGSA", cfg.port, period) && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (cfg.gsv) {
        if (index == next) {
            // GSV 与定位同频，但至少 1 Hz，避免高速率时刷爆串口。
            return append_message(cmd, "GPGSV", cfg.port, "1") && finish_step(cmd, true, 0, out);
        }
        ++next;
    }
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

Reply match_reply(const demux::Decoder &dec, std::span<const uint8_t> frame, std::span<const uint8_t> sent) noexcept
{
    if (&dec != &kResponseDecoder) {
        return Reply::kNone;
    }
    const size_t size = frame.size();
    const size_t prefix_len = kCommandPrefix.size();
    if (size < prefix_len + kResponseTag.size() + 3 || frame[size - 1] != kLf || frame[size - 2] != kCr ||
        frame[size - 5] != kStar) {
        return Reply::kNone;
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
        return Reply::kNone;
    }

    size_t sent_len = sent.size();
    while (sent_len > 0 && (sent[sent_len - 1] == kCr || sent[sent_len - 1] == kLf)) {
        --sent_len;
    }
    const size_t echo_len = sep - prefix_len;
    if (echo_len != sent_len) {
        return Reply::kNone;
    }
    for (size_t i = 0; i < sent_len; ++i) {
        if (frame[prefix_len + i] != sent[i]) {
            return Reply::kNone; // 不是本步命令的应答
        }
    }

    size_t text_begin = sep + kResponseTag.size();
    size_t text_end = size - 5; // '*' 之前
    while (text_end > text_begin && frame[text_end - 1] == static_cast<uint8_t>(' ')) {
        --text_end;
    }
    const std::span<const uint8_t> text = frame.subspan(text_begin, text_end - text_begin);
    return quoted_equals(text, "OK") ? Reply::kAck : Reply::kNack;
}

uint32_t caps(Model model) noexcept
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

const Dialect kDialect{"Unicore",    Family::kUnicore, kDecoders,    &build_probe,
                       &match_probe, &rover_step,      &match_reply, &caps};

} // namespace gnss::unicore
