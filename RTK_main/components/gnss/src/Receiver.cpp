// Receiver 基类的默认实现（ADR-0012 §2.2）：输出掩码、输出配置、帧归类、帧标签、配置读回。
// 线程安全：是（全部纯函数，无全局可变状态）。ISR 安全：是（不阻塞、不分配）。
// 堆与阻塞：不使用堆、不使用浮点、不阻塞。
#include "gnss/Receiver.hpp"

#include <cstring>
#include <string_view>

#include "FrameTag.hpp"

namespace gnss {
namespace {

/**
 * @brief  OutputMsg → 掩码位
 * @param  msg: 报文类型枚举值
 * @retval 单比特掩码（1 << msg），与 outputMask() 的位序一致
 */
constexpr uint32_t bit(OutputMsg msg) noexcept
{
    return 1U << static_cast<uint32_t>(msg);
}

// NMEA 语句结构：'$' + 两位 talker + 3 位语句名 + ','。
constexpr size_t kNmeaTalkerEnd = 3U; // "$xx"
constexpr size_t kNmeaTagLen = 3U;    // "GGA"/"GSV"…
constexpr size_t kNmeaCommaPos = 6U;
constexpr size_t kNmeaHeaderLen = 7U;

/**
 * @brief  判断 NMEA 帧的语句名是否等于 tag
 * @param  frame: 已按 '$' 分流的帧，长度须 ≥ kNmeaHeaderLen（由调用方保证）
 * @param  tag: 3 字节语句名（"GGA"/"GST"…），不在内部做长度检查
 * @retval true: 第 3..5 字节与 tag 相同（大小写敏感）
 * @retval false: 不同
 */
bool tagIs(std::span<const uint8_t> frame, const char *tag) noexcept
{
    return std::memcmp(frame.subspan(kNmeaTalkerEnd, kNmeaTagLen).data(), tag, kNmeaTagLen) == 0;
}

} // namespace

uint32_t Receiver::outputMask(Model) const noexcept
{
    // 所有驱动都能输出的四类 NMEA；SATSINFO/HEADING 等厂家报文由驱动追加。
    return bit(OutputMsg::kGga) | bit(OutputMsg::kRmc) | bit(OutputMsg::kGsa) |
           bit(OutputMsg::kGsv);
}

const Receiver &Receiver::forModel(Model) const noexcept
{
    // 没有型号子类的驱动：型号就是自身（ADR-0012 §2.8）。
    return *this;
}

fw::Status Receiver::buildOutput(OutputMsg, uint32_t, Model, const OutputPlan &,
                                 CmdBuf &) const noexcept
{
    return fw::Status::kNotSupported; // 由具体驱动覆盖（ADR-0012 §2.4）
}

OutputMsg Receiver::classifyFrame(const demux::Decoder &,
                                  std::span<const uint8_t> frame) const noexcept
{
    // 通用 NMEA：忽略 talker（GP/GN/BD…），只按 3 位语句名归类。
    // 厂家私有帧（#SATSINFOA 等）由此处返回 kNone，由对应驱动覆盖后追加。
    if (frame.size() < kNmeaHeaderLen || frame[0] != static_cast<uint8_t>('$') ||
        frame[kNmeaCommaPos] != static_cast<uint8_t>(',')) {
        return OutputMsg::kNone;
    }
    if (tagIs(frame, "GGA")) {
        return OutputMsg::kGga;
    }
    if (tagIs(frame, "GST")) {
        return OutputMsg::kGst;
    }
    if (tagIs(frame, "RMC")) {
        return OutputMsg::kRmc;
    }
    if (tagIs(frame, "GSA")) {
        return OutputMsg::kGsa;
    }
    if (tagIs(frame, "GSV")) {
        return OutputMsg::kGsv;
    }
    return OutputMsg::kNone;
}

size_t Receiver::frameTag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                          std::span<char> out) const noexcept
{
    // 与 main 原 describe_frame() 文本一致：驱动接入后日志不变。
    const std::string_view name{dec.name};
    if (name == "NMEA" && frame.size() > detail::kNmeaTagLen) {
        return detail::tagWithWord(out, "NMEA ", frame, detail::kNmeaTagOffset);
    }
    if (name == "RTCM3" && frame.size() > detail::kRtcmTagOffset + 1U) {
        const uint32_t msg =
            (static_cast<uint32_t>(frame[detail::kRtcmTagOffset]) << detail::kRtcmMsgShift) |
            (static_cast<uint32_t>(frame[detail::kRtcmTagOffset + 1U]) >> detail::kRtcmMsgShift);
        const size_t pos = detail::appendText(out, 0U, "RTCM3 ");
        return detail::appendUint(out, pos, msg & detail::kRtcmMsgMask);
    }
    if (name == "UBX" && frame.size() > detail::kUbxClassOffset + 1U) {
        size_t pos = detail::appendText(out, 0U, "UBX 0x");
        pos = detail::appendHex2(out, pos, frame[detail::kUbxClassOffset]);
        pos = detail::appendText(out, pos, "/0x");
        return detail::appendHex2(out, pos, frame[detail::kUbxClassOffset + 1U]);
    }
    return detail::appendText(out, 0U, name);
}

bool Receiver::buildReadback(CmdBuf &) const noexcept
{
    return false; // 由具体驱动覆盖（ADR-0012 §2.7）
}

size_t Receiver::matchReadback(const demux::Decoder &, std::span<const uint8_t>,
                               std::span<char>) const noexcept
{
    return 0U; // 由具体驱动覆盖（ADR-0012 §2.7）
}

} // namespace gnss
