// Unicore N4（UM980/UM981/UM982）驱动实现：型号识别（#VERSIONA）、能力表、探测/读回、输出计划与虚函数实现（ADR-0010 §2.2/§2.4）。
// 线程安全：是（纯函数，无全局可变状态）。ISR 安全：是（无锁、不阻塞）。
// 堆与阻塞：不使用堆、不阻塞；应答解码见 UnicoreResp.cpp，命令与脚本见 UnicoreScript.cpp。
#include "gnss/drivers/UnicoreReceiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "demux/decoders/UnicoreHash.hpp"
#include "../AsciiCmd.hpp"
#include "../DriverInstances.hpp" // 单例声明（保证定义与 extern 声明一致）
#include "../FrameTag.hpp"
#include "UnicoreKeys.hpp"

namespace gnss {
namespace {

using unicore::kConfigPrefix;
using unicore::kStar;
using unicore::kStarFromEnd;
using unicore::kVersionTag;

// $command 应答（classifyReply）与 #VERSIONA 探测应答（matchProbe）各需一个解码器，缺一则对应帧落入 unknown。
constexpr std::array<const demux::Decoder *, 2> kDecoders{&unicore::kResponseDecoder,
                                                          &demux::unicore_hash::kDecoder};

/** 报文名字（下标 = OutputMsg）：与大写常量同名，便于与手册/日志对照。 */
constexpr std::array<const char *, kOutputMsgCount> kOutputNames = {
    "GPGGA", "SATSINFOA", "HEADINGA", "GPGST", "GPRMC", "GPGSA", "GPGSV",
};

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
 * @brief 从版本文本识别 UM9xx 和兼容 Unicore 的 CM680（ADR-0012 §2.9）。
 * @param text: VERSIONA 里第一个引号字段。
 * @retval model: 识别出的型号。
 * @retval std::nullopt: 未识别出兼容的型号。
 * @note 纯函数，线程/ISR 安全。
 */
std::optional<Model> modelFromText(std::span<const uint8_t> text) noexcept
{
    if (ascii::quotedEquals(text, "UM982")) {
        return Model::kUm982;
    }
    if (ascii::quotedEquals(text, "UM981")) {
        return Model::kUm981;
    }
    if (ascii::quotedEquals(text, "CM680")) {
        return Model::kCm680;
    }
    if (ascii::quotedEquals(text, "UM980") || ascii::quotedStartsWith(text, "UM9")) {
        return Model::kUm980;
    }
    return std::nullopt;
}
} // namespace

namespace detail {

constinit const Um980Receiver kUm980Receiver{};
constinit const Um982Receiver kUm982Receiver{};
constinit const Cm680Receiver kCm680Receiver{};

} // namespace detail

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
    case Model::kCm680: // 手册未确认 20 Hz。
    default:
        return cap::kRtcmIn | cap::kBase;
    }
}

/**
 * @brief 按型号选用 Unicore 或 Optisens 驱动实例
 * @param model: 探测得到的型号
 * @retval CM680 用 Optisens CM680 实例，UM982 用双天线实例，其余沿用当前实例
 * @note 只读，线程/ISR 安全。
 */
const Receiver &UnicoreReceiver::forModel(Model model) const noexcept
{
    if (model == Model::kCm680) {
        return detail::kCm680Receiver;
    }
    // 双天线型号有单独驱动（多 kHeading 能力），其余 UM9xx 沿用家族驱动（ADR-0012 §2.8）。
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

/**
 * @brief 生成执行清空、流动站模式及可选切速的基础脚本步骤。
 * @param index: 启用步骤的序号。
 * @param model: 接收机型号，CM680 使用 UNLOGALL 清空端口。
 * @param cfg: 流动站配置。
 * @param[out] out: 命令和应答策略。
 * @retval true: 已生成步骤。
 * @retval false: 脚本结束或命令写入失败。
 * @note 纯函数，线程/ISR 安全。
 */
bool UnicoreReceiver::roverStep(size_t index, Model model, const RoverConfig &cfg,
                                ConfigStep &out) const noexcept
{
    if (index == 0U && model == Model::kCm680) {
        // P2 实测 CM680 拒绝不带报文名的 UNLOG，清空该口需用 UNLOGALL。
        out = ConfigStep{.timeoutMs = unicore::kConfigTimeoutMs};
        return out.cmd.append("UNLOGALL COM") && ascii::appendUint(out.cmd, cfg.port) &&
               out.cmd.append("\r\n");
    }
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
    // P2 实测 CM680 支持 GPGST；卫星信息沿用基类的 GSV。
    uint32_t mask = Receiver::outputMask(model) | kGstBit;
    if (model != Model::kCm680) {
        mask |= kSatsInfoBit;
    }
    // 经虚函数问能力：Um982Receiver 覆盖 caps() 后这里自动带上 HEADINGA。
    if ((caps(model) & cap::kHeading) != 0U) {
        mask |= kHeadingBit; // 仅双天线型号（UM982）有 HEADINGA
    }
    return mask;
}

/**
 * @brief 生成一条开启或关闭报文输出的命令。
 * @param msg: 报文类型。
 * @param periodMs: 请求周期，0 表示关闭，CM680 不支持亚秒 GSA。
 * @param model: 接收机型号。
 * @param plan: 提供接收机连接端口。
 * @param[out] out: 命令缓冲。
 * @retval kOk 已生成命令。
 * @retval kInvalidArg 报文类型、周期或缓冲不合法。
 * @retval kNotSupported 该型号不支持此报文。
 * @note 纯函数，线程/ISR 安全，命令缓冲由调用者独占。
 */
fw::Status UnicoreReceiver::buildOutput(OutputMsg msg, uint32_t periodMs, Model model,
                                        const OutputPlan &plan, CmdBuf &out) const noexcept
{
    const auto idx = static_cast<size_t>(static_cast<uint8_t>(msg));
    if ((msg == OutputMsg::kNone) || (idx >= kOutputNames.size())) {
        return fw::Status::kInvalidArg;
    }
    // 重配也会关闭周期为零的报文，开启和关闭都须遵循同一型号能力表。
    const uint32_t bit = 1U << static_cast<uint32_t>(idx); // 报文下标已验证，小于 32。
    if ((outputMask(model) & bit) == 0U) {
        return fw::Status::kNotSupported;
    }
    const std::string_view name{kOutputNames[idx]};
    // CM680 实测不接受亚秒 GSA。旧 NVS 值由应用在启动时恢复。
    if (model == Model::kCm680 && msg == OutputMsg::kGsa && periodMs != 0U &&
        periodMs < ascii::kMsPerSecond) {
        return fw::Status::kInvalidArg;
    }
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

/**
 * @brief 双天线实例收到 CM680 型号查询时转给 Optisens CM680 实例
 * @param model: 探测得到的型号
 * @retval CM680 用 Optisens CM680 实例，其余沿用当前实例
 * @note 只读，线程/ISR 安全。
 */
const Receiver &Um982Receiver::forModel(Model model) const noexcept
{
    if (model == Model::kCm680) {
        return detail::kCm680Receiver;
    }
    return *this;
}

/** @brief 返回 CM680 协议 V1.2.8 表 1-1 明列的定位输出频率。 @param model: 型号。 @retval 静态频率集合。 */
std::span<const uint8_t> Cm680Receiver::navRatesHz(Model model) const noexcept
{
    static constexpr std::array<uint8_t, 4U> kRates{1U, 2U, 5U, 10U};
    return model == Model::kCm680 ? std::span<const uint8_t>{kRates} : std::span<const uint8_t>{};
}

/** @brief 返回 CM680 已确认的周期。 @param msg: 报文。 @param model: 型号。 @retval 毫秒周期集合。 */
std::span<const uint16_t> Cm680Receiver::outputPeriodsMs(OutputMsg msg, Model model) const noexcept
{
    static constexpr std::array<uint16_t, 5U> kTimed{0U, 100U, 200U, 500U, 1000U};
    static constexpr std::array<uint16_t, 2U> kSecond{0U, 1000U};
    // V1.2.8 §6.2 给出 GSV 1 s，ADR-0018 §1 记录 5 s 实测。
    static constexpr std::array<uint16_t, 3U> kGsv{0U, 1000U, 5000U};
    if (model != Model::kCm680) {
        return {};
    }
    switch (msg) {
    case OutputMsg::kGga:
    case OutputMsg::kGst:
    case OutputMsg::kRmc:
        return kTimed;
    case OutputMsg::kGsa:
        return kSecond;
    case OutputMsg::kGsv:
        return kGsv;
    default:
        return {};
    }
}

Family Cm680Receiver::family() const noexcept
{
    return Family::kOptisens;
}

std::string_view Cm680Receiver::name() const noexcept
{
    return "optisens";
}

const Receiver &Cm680Receiver::forModel(Model model) const noexcept
{
    if (model == Model::kCm680) {
        return *this;
    }
    return detail::kUm980Receiver.forModel(model);
}
} // namespace gnss
