// ComNav K8（K803）方言实现：NovAtel 风格命令与 "<OK/<ERROR" 应答（ADR-0010 §2.2，实验性、未经实物验证）。
// 线程/调用约束：纯函数，无全局可变状态，可重入、不阻塞。
// 堆：不使用。注意：未取得官方手册，语法按公开的 NovAtel 兼容资料实现；改动前须先修订 ADR-0010 §2.2。
#include "gnss/dialects/comnav.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/decoders/unicore_hash.hpp"

namespace gnss::comnav {
namespace {

constexpr uint8_t kPrompt = static_cast<uint8_t>('<');
constexpr uint8_t kCr = static_cast<uint8_t>('\r');
constexpr uint8_t kLf = static_cast<uint8_t>('\n');
constexpr std::string_view kVersionTag = "#VERSIONA,";

constexpr std::array<const demux::Decoder *, 1> kDecoders{&kPromptDecoder};

bool is_printable(uint8_t c) noexcept
{
    return c >= 0x20U && c <= 0x7EU;
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

bool append_log(CmdBuf &out, const char *message, uint8_t port, const char *period) noexcept
{
    return out.append("LOG COM") && append_uint(out, port) && out.append(" ") &&
           out.append(std::string_view(message)) && out.append(" ONTIME ") && out.append(std::string_view(period)) &&
           out.append("\r\n");
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

} // namespace

bool prompt_first_byte(uint8_t byte) noexcept
{
    return byte == kPrompt;
}

demux::Step prompt_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    const size_t size = frame.size();
    if (size == 0) {
        return demux::Step::kReject;
    }
    if (frame[size - 1] == kLf) {
        if (size < 4 || frame[size - 2] != kCr) {
            return demux::Step::kReject;
        }
        for (size_t i = 1; i + 2 < size; ++i) {
            if (!is_printable(frame[i])) {
                return demux::Step::kReject;
            }
        }
        return demux::Step::kFrame;
    }
    // 末字节是 '\r' 时只差 '\n'（分块读取常在 CR 后断开），仍属有效前缀。
    if (!is_printable(frame[size - 1]) && frame[size - 1] != kCr) {
        return demux::Step::kReject;
    }
    need = 1;
    return demux::Step::kNeedMore;
}

bool build_probe(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("LOG VERSIONA ONCE\r\n");
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
    out.timeout_ms = 500;
    out.switch_baud = switch_baud;
    return true;
}

bool rover_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) noexcept
{
    (void)model; // 目前只覆盖 K803
    const char *period = period_for(cfg.rate_hz);
    CmdBuf cmd;
    size_t next = 0;

    if (index == next) {
        return cmd.append("UNLOGALL COM") && append_uint(cmd, cfg.port) && cmd.append("\r\n") &&
               finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return append_log(cmd, "GPGGA", cfg.port, period) && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return append_log(cmd, "GPRMC", cfg.port, period) && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (index == next) {
        return append_log(cmd, "GPGSA", cfg.port, period) && finish_step(cmd, true, 0, out);
    }
    ++next;
    if (cfg.gsv) {
        if (index == next) {
            return append_log(cmd, "GPGSV", cfg.port, "1") && finish_step(cmd, true, 0, out);
        }
        ++next;
    }
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

Reply match_reply(const demux::Decoder &dec, std::span<const uint8_t> frame, std::span<const uint8_t>) noexcept
{
    if (&dec != &kPromptDecoder) {
        return Reply::kNone;
    }
    if (starts_with(frame, "<OK")) {
        return Reply::kAck;
    }
    if (starts_with(frame, "<ERROR")) {
        return Reply::kNack;
    }
    return Reply::kNone;
}

uint32_t caps(Model model) noexcept
{
    if (model == Model::kK803) {
        return cap::kRtcmIn | cap::kBase;
    }
    return cap::kRtcmIn | cap::kBase;
}

const Dialect kDialect{"ComNav",     Family::kComnav, kDecoders,    &build_probe,
                       &match_probe, &rover_step,     &match_reply, &caps};

} // namespace gnss::comnav
