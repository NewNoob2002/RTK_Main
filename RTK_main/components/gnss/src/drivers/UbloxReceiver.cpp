// u-blox F9（ZED-F9P）驱动实现：UBX-MON-VER 探测、配置脚本、输出计划与虚函数实现（ADR-0010 §2.2/§2.4）。
// 结构：UBX 帧层与 CFG-VALSET 编码见 UbloxUbx.cpp，线格式常量见 UbloxWire.hpp；
// 脚本步骤的回调写在 roverStep 内（要取 protected 成员的地址，匿名命名空间的自由函数没有访问权）。
// 线程/调用约束：纯函数，无全局可变状态，可重入、不阻塞。
// 堆：不使用（命令在 CmdBuf 定长缓冲内构造）。
// 键 ID 出处：u-blox F9 HPG 1.32 Interface Description，CFG-MSGOUT NMEA 段、RATE 段、UART1 段；
// 本仓库无该手册副本，ID 未经实物验证（仅 UM980 有 HIL）。
#include "gnss/drivers/UbloxReceiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/decoders/Ubx.hpp"
#include "../AsciiCmd.hpp"
#include "../DriverInstances.hpp" // 单例声明（保证定义与 extern 声明一致）
#include "../FrameTag.hpp"
#include "UbloxWire.hpp"

namespace gnss {
namespace {

using ublox::kClsCfg;
using ublox::kClsMon;
using ublox::kClsOffset;
using ublox::kConfigTimeoutMs;
using ublox::kCrcBytes;
using ublox::kDefaultPeriodMs;
using ublox::kHalfSecondMs;
using ublox::kHeaderBytes;
using ublox::kIdAck;
using ublox::kIdCfgValset;
using ublox::kIdMonVer;
using ublox::kIdOffset;
using ublox::kKeyMsgoutGgaUart1;
using ublox::kKeyMsgoutGsaUart1;
using ublox::kKeyMsgoutGstUart1;
using ublox::kKeyMsgoutGsvUart1;
using ublox::kKeyMsgoutRmcUart1;
using ublox::kMaxEpochsPerEpoch;
using ublox::kMaxRateHz;
using ublox::kMinRateHz;
using ublox::kMonVerExtBytes;
using ublox::kMonVerMinLen;
using ublox::kMonVerPollBytes;
using ublox::kMonVerPollCkA;
using ublox::kMonVerPollCkB;
using ublox::kMsPerSecond;
using ublox::kPayloadOffset;
using ublox::kSync1;
using ublox::kSync2;

// MON-VER 探测应答与 ACK/NAK 都是 UBX 帧（matchProbe / classifyReply 均按 ubx 解码器判定）。
constexpr std::array<const demux::Decoder *, 1> kDecoders{&demux::ubx::kDecoder};

/**
 * @brief 把 MON-VER 的文本项拷进定长缓冲（NUL 结尾，超长截断）。
 * @param text: 源文本（可能不满 30 字节，以 NUL 或长度结束）。
 * @param out: 输出缓冲（kVersionLen 字节）。
 */
void copyText(std::span<const uint8_t> text, std::array<char, kVersionLen> &out) noexcept
{
    size_t len = 0;
    while ((len < text.size()) && (text[len] != 0)) {
        ++len;
    }
    const size_t copy = len < (kVersionLen - 1) ? len : (kVersionLen - 1);
    for (size_t i = 0; i < copy; ++i) {
        out[i] = static_cast<char>(text[i]);
    }
    out[copy] = '\0';
}

/**
 * @brief 文本项是否以给定前缀开头（"MOD=" 等）。
 * @param ext: 扩展项字节。
 * @param prefix: 期望前缀。
 * @param tail: 输出，去掉前缀后的剩余字节（前缀不匹配时不动）。
 * @retval true: 前缀匹配。
 * @retval false: 不匹配。
 */
bool extText(std::span<const uint8_t> ext, std::string_view prefix,
             std::span<const uint8_t> &tail) noexcept
{
    if (!ascii::startsWith(ext, prefix)) {
        return false;
    }
    tail = ext.subspan(prefix.size());
    return true;
}

/**
 * @brief 扫描 MON-VER 载荷的扩展项，取型号与固件版本。
 * @param payload: MON-VER 载荷（长度 >= 40）。
 * @param out: 输出，型号与固件文本。
 * @retval true: 找到 "MOD=ZED-F9P"。
 * @retval false: 不是 F9P（同家族其他型号不认作本驱动）。
 */
bool readMonVer(std::span<const uint8_t> payload, ProbeResult &out) noexcept
{
    ProbeResult result{};
    bool haveModel = false;
    for (size_t off = kMonVerMinLen; (off + kMonVerExtBytes) <= payload.size();
         off += kMonVerExtBytes) {
        const std::span<const uint8_t> ext = payload.subspan(off, kMonVerExtBytes);
        std::span<const uint8_t> tail{};
        if (extText(ext, "MOD=", tail)) {
            if (!ascii::quotedStartsWith(tail, "ZED-F9P")) {
                return false;
            }
            haveModel = true;
        } else if (extText(ext, "FWVER=", tail)) {
            copyText(tail, result.fwVersion);
        }
    }
    if (!haveModel) {
        return false;
    }
    result.model = Model::kZedF9p;
    out = result;
    return true;
}

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

/** @brief 仅在要求改波特率时参与（portBaud == 0 时脚本提前结束）。 */
bool portBaudEnabled(const RoverConfig &cfg) noexcept
{
    return cfg.portBaud != 0U;
}

/**
 * @brief 把配置频率夹到驱动支持范围（1..20 Hz），并换算成导航周期毫秒。
 * @param cfg: 流动站配置。
 * @retval 周期: 导航周期（毫秒），rateHz = 0 视为 1 Hz。
 */
uint16_t navPeriodMs(const RoverConfig &cfg) noexcept
{
    uint8_t rate = cfg.rateHz;
    if (rate == 0U) {
        rate = kMinRateHz;
    }
    if (rate > kMaxRateHz) {
        rate = kMaxRateHz;
    }
    const auto period = static_cast<uint16_t>(kDefaultPeriodMs / rate);
    return period == 0U ? kDefaultPeriodMs : period;
}
} // namespace

namespace detail {

// NOLINTNEXTLINE(readability-identifier-naming): 外部链接只读单例，命名见 DriverInstances.hpp
constinit const ZedF9pReceiver kZedF9pReceiver{};

} // namespace detail

// ---- 命令库：探测命令；配置命令见 UbloxUbx.cpp ----

/**
 * @brief 探测命令：UBX-MON-VER 轮询（长度 0、无载荷，校验和已算好）。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整帧。
 * @retval false: 缓冲空间不足。
 */
bool UbloxReceiver::cmdPollMonVer(CmdBuf &out) noexcept
{
    out.clear();
    return out.append(std::array<uint8_t, kMonVerPollBytes>{
        kSync1, kSync2, kClsMon, kIdMonVer, 0x00, 0x00, kMonVerPollCkA, kMonVerPollCkB});
}

// ---- 驱动成员 ----

Family UbloxReceiver::family() const noexcept
{
    return Family::kUblox;
}

std::string_view UbloxReceiver::name() const noexcept
{
    return "ublox";
}

std::span<const demux::Decoder *const> UbloxReceiver::decoders() const noexcept
{
    return kDecoders;
}

uint32_t UbloxReceiver::caps(Model model) const noexcept
{
    if (model == Model::kZedF9p) {
        return cap::kRtcmIn | cap::kBase | cap::kRate20Hz;
    }
    return cap::kRtcmIn | cap::kBase;
}

bool UbloxReceiver::buildProbe(CmdBuf &out) const noexcept
{
    return cmdPollMonVer(out);
}

bool UbloxReceiver::matchProbe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                               ProbeResult &out) const noexcept
{
    if ((&dec != &demux::ubx::kDecoder) || (frame.size() < (kHeaderBytes + kCrcBytes))) {
        return false;
    }
    if ((frame[kClsOffset] != kClsMon) || (frame[kIdOffset] != kIdMonVer)) {
        return false;
    }
    const size_t len = respPayloadLen(frame);
    if ((len < kMonVerMinLen) || (frame.size() < (kPayloadOffset + len + kCrcBytes))) {
        return false;
    }
    return readMonVer(frame.subspan(kPayloadOffset, len), out);
}

bool UbloxReceiver::roverStep(size_t index, Model model, const RoverConfig &cfg,
                              ConfigStep &out) const noexcept
{
    static_cast<void>(model); // ZED-F9P 单型号，脚本与型号无关
    // 表放在成员函数作用域内：步骤回调要调 protected 的 cmd_*，匿名命名空间的自由函数没有访问权。
    static constexpr std::array<ScriptStep, 2> kRoverScript{{
        {[](const RoverConfig &c, ConfigStep &o) noexcept {
             if (!cmdConfigureReceiver(o.cmd, navPeriodMs(c))) {
                 return false;
             }
             o.expectReply = true;
             o.timeoutMs = kConfigTimeoutMs;
             o.switchBaud = 0;
             return true;
         },
         &always},
        {[](const RoverConfig &c, ConfigStep &o) noexcept {
             if (!cmdSetBaudrate(o.cmd, c.portBaud)) {
                 return false;
             }
             o.expectReply = false;
             o.timeoutMs = kConfigTimeoutMs;
             o.switchBaud = c.portBaud;
             return true;
         },
         &portBaudEnabled},
    }};
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

Ack UbloxReceiver::classifyReply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                 std::span<const uint8_t> sent) const noexcept
{
    static_cast<void>(sent); // ACK/NAK 只回显 class/id，不回显命令文本
    if (&dec != &demux::ubx::kDecoder) {
        return Ack::kNone;
    }
    if (!respHasValidAck(frame) || !respAckClassId(frame, kClsCfg, kIdCfgValset)) {
        return Ack::kNone; // 只认 CFG-VALSET（06 8A）的应答
    }
    return frame[kIdOffset] == kIdAck ? Ack::kAck : Ack::kNack;
}

size_t UbloxReceiver::frameTag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                               std::span<char> out) const noexcept
{
    if ((std::string_view{dec.name} == "UBX") && (frame.size() > (detail::kUbxClassOffset + 1U))) {
        size_t pos = detail::appendText(out, 0U, "UBX 0x");
        pos = detail::appendHex2(out, pos, frame[detail::kUbxClassOffset]);
        pos = detail::appendText(out, pos, "/0x");
        return detail::appendHex2(out, pos, frame[detail::kUbxClassOffset + 1U]);
    }
    return Receiver::frameTag(dec, frame, out);
}

uint32_t UbloxReceiver::outputMask(Model model) const noexcept
{
    // ZED-F9P 能输出这 5 类；SATSINFOA/HEADINGA 是 Unicore 私有帧，返回 kNotSupported。
    constexpr uint32_t kGgaBit = 1U << static_cast<uint32_t>(OutputMsg::kGga);
    constexpr uint32_t kGstBit = 1U << static_cast<uint32_t>(OutputMsg::kGst);
    constexpr uint32_t kRmcBit = 1U << static_cast<uint32_t>(OutputMsg::kRmc);
    constexpr uint32_t kGsaBit = 1U << static_cast<uint32_t>(OutputMsg::kGsa);
    constexpr uint32_t kGsvBit = 1U << static_cast<uint32_t>(OutputMsg::kGsv);
    static_cast<void>(model); // 单型号
    return kGgaBit | kGstBit | kRmcBit | kGsaBit | kGsvBit;
}

fw::Status UbloxReceiver::buildOutput(OutputMsg msg, uint32_t periodMs, Model model,
                                      const OutputPlan &plan, CmdBuf &out) const noexcept
{
    static_cast<void>(model);
    uint32_t key = 0U;
    switch (msg) {
    case OutputMsg::kGga:
        key = kKeyMsgoutGgaUart1;
        break;
    case OutputMsg::kRmc:
        key = kKeyMsgoutRmcUart1;
        break;
    case OutputMsg::kGsa:
        key = kKeyMsgoutGsaUart1;
        break;
    case OutputMsg::kGst:
        key = kKeyMsgoutGstUart1;
        break;
    case OutputMsg::kGsv:
        key = kKeyMsgoutGsvUart1;
        break;
    case OutputMsg::kSatsInfo:
    case OutputMsg::kHeading:
        return fw::Status::kNotSupported;
    case OutputMsg::kNone:
    case OutputMsg::kCount:
    default:
        return fw::Status::kInvalidArg;
    }

    // 值 = 每历元次数：round(periodMs * rateHz / 1000)，0 关闭；算下来为 0 时按 1（最慢 1 历元一次）。
    const uint32_t rate = (plan.rateHz == 0U) ? kMinRateHz : static_cast<uint32_t>(plan.rateHz);
    uint32_t epochs = 0U;
    if (periodMs != 0U) {
        // periodMs 最大 128000、rate 最大 20，乘积 2.56e6 在 uint32_t 内，无需 64 位。
        const uint32_t scaled = (periodMs * rate) + kHalfSecondMs;
        epochs = scaled / kMsPerSecond;
        if (epochs == 0U) {
            epochs = 1U;
        }
        if (epochs > kMaxEpochsPerEpoch) {
            return fw::Status::kInvalidArg; // 键类型是 U1，超出可表示范围视为不可实现参数
        }
    }
    if (!cmdValsetMsgout(out, key, static_cast<uint8_t>(epochs))) {
        out.clear();
        return fw::Status::kInvalidArg;
    }
    return fw::Status::kOk;
}

} // namespace gnss
