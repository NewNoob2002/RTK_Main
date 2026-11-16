// Receiver 基类的默认实现（ADR-0012 §2.2）：输出掩码、输出配置、帧归类、帧标签、配置读回。
// 线程/调用约束：全部为纯函数（无全局可变状态、不阻塞），可从任意任务或 ISR 调用。
// 堆：不使用。浮点：不使用。
#include "gnss/Receiver.hpp"

#include <cstring>
#include <string_view>

#include "FrameTag.hpp"

namespace gnss {
namespace {

/** OutputMsg → 掩码位。 */
constexpr uint32_t bit(OutputMsg msg) noexcept
{
    return 1U << static_cast<uint32_t>(msg);
}

// NMEA 语句结构：'$' + 两位 talker + 3 位语句名 + ','。
constexpr size_t kNmeaTalkerEnd = 3U; // "$xx"
constexpr size_t kNmeaTagLen = 3U;    // "GGA"/"GSV"…
constexpr size_t kNmeaCommaPos = 6U;
constexpr size_t kNmeaHeaderLen = 7U;

bool tag_is(std::span<const uint8_t> frame, const char *tag) noexcept
{
    return std::memcmp(frame.subspan(kNmeaTalkerEnd, kNmeaTagLen).data(), tag, kNmeaTagLen) == 0;
}

} // namespace

uint32_t Receiver::output_mask(Model) const noexcept
{
    // 所有驱动都能输出的四类 NMEA；SATSINFO/HEADING 等厂家报文由驱动追加。
    return bit(OutputMsg::kGga) | bit(OutputMsg::kRmc) | bit(OutputMsg::kGsa) | bit(OutputMsg::kGsv);
}

fw::Status Receiver::build_output(OutputMsg, uint32_t, Model, const OutputPlan &, CmdBuf &) const noexcept
{
    return fw::Status::kNotSupported; // P3b 在驱动里实现
}

OutputMsg Receiver::classify_frame(const demux::Decoder &, std::span<const uint8_t> frame) const noexcept
{
    // 通用 NMEA：忽略 talker（GP/GN/BD…），只按 3 位语句名归类。
    // 厂家私有帧（#SATSINFOA 等）由此处返回 kNone，由对应驱动覆盖后追加。
    if (frame.size() < kNmeaHeaderLen || frame[0] != static_cast<uint8_t>('$') ||
        frame[kNmeaCommaPos] != static_cast<uint8_t>(',')) {
        return OutputMsg::kNone;
    }
    if (tag_is(frame, "GGA")) {
        return OutputMsg::kGga;
    }
    if (tag_is(frame, "GST")) {
        return OutputMsg::kGst;
    }
    if (tag_is(frame, "RMC")) {
        return OutputMsg::kRmc;
    }
    if (tag_is(frame, "GSA")) {
        return OutputMsg::kGsa;
    }
    if (tag_is(frame, "GSV")) {
        return OutputMsg::kGsv;
    }
    return OutputMsg::kNone;
}

size_t Receiver::frame_tag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                           std::span<char> out) const noexcept
{
    // 与 main 原 describe_frame() 文本一致（P3b 替换后日志不变）。
    const std::string_view name{dec.name};
    if (name == "NMEA" && frame.size() > detail::kNmeaTagLen) {
        return detail::tag_with_word(out, "NMEA ", frame, detail::kNmeaTagOffset);
    }
    if (name == "RTCM3" && frame.size() > detail::kRtcmTagOffset + 1U) {
        const uint32_t msg = (static_cast<uint32_t>(frame[detail::kRtcmTagOffset]) << detail::kRtcmMsgShift) |
                             (static_cast<uint32_t>(frame[detail::kRtcmTagOffset + 1U]) >> detail::kRtcmMsgShift);
        const size_t pos = detail::append_text(out, 0U, "RTCM3 ");
        return detail::append_uint(out, pos, msg & detail::kRtcmMsgMask);
    }
    if (name == "UBX" && frame.size() > detail::kUbxClassOffset + 1U) {
        size_t pos = detail::append_text(out, 0U, "UBX 0x");
        pos = detail::append_hex2(out, pos, frame[detail::kUbxClassOffset]);
        pos = detail::append_text(out, pos, "/0x");
        return detail::append_hex2(out, pos, frame[detail::kUbxClassOffset + 1U]);
    }
    return detail::append_text(out, 0U, name);
}

bool Receiver::build_readback(CmdBuf &) const noexcept
{
    return false; // P3c
}

size_t Receiver::match_readback(const demux::Decoder &, std::span<const uint8_t>, std::span<char>) const noexcept
{
    return 0U; // P3c
}

} // namespace gnss
