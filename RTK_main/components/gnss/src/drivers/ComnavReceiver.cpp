// ComNav K8（K803）驱动实现：NovAtel 风格命令与 "<OK/<ERROR" 应答（ADR-0010 §2.2，实验性、未经实物验证）。
// 结构（P3s）：命令生成（cmd_*）与应答判定（resp_*）是 ComnavReceiver 的 protected static，子类可直接复用；
// 脚本步骤的回调写在 roverStep 内（要取 protected 成员的地址，匿名命名空间的自由函数没有访问权）。
// 线程/调用约束：纯函数，无全局可变状态，可重入、不阻塞。
// 堆：不使用。注意：未取得官方手册，语法按公开的 NovAtel 兼容资料实现；改动前须先修订 ADR-0010 §2.2。
#include "gnss/drivers/ComnavReceiver.hpp"

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

namespace gnss {
namespace {

constexpr uint8_t kPrompt = static_cast<uint8_t>('<');
constexpr uint8_t kCr = static_cast<uint8_t>(ascii::kCr);
constexpr uint8_t kLf = static_cast<uint8_t>(ascii::kLf);
constexpr std::string_view kVersionTag = "#VERSIONA,";
/** 最短有效应答行 "<x\r\n"。 */
constexpr std::size_t kMinPromptBytes = 4U;
constexpr uint32_t kConfigTimeoutMs = 500U;

/** 报文名字（下标 = OutputMsg）：ComNav 只支持其中 4 类 NMEA。 */
constexpr std::array<const char *, kOutputMsgCount> kOutputNames = {
    "GPGGA", "SATSINFOA", "HEADINGA", "GPGST", "GPRMC", "GPGSA", "GPGSV",
};

// <OK 提示符应答（classifyReply）与 #VERSIONA 探测应答（matchProbe）各需一个解码器。
constexpr std::array<const demux::Decoder *, 2> kDecoders{&comnav::kPromptDecoder,
                                                          &demux::unicore_hash::kDecoder};

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

/**
 * @brief 构造“拒绝”结论。
 * @param reason: 拒绝原因（explain 用）。
 * @param offset: 出问题的字节位置。
 * @param byte: 该位置的字节。
 * @retval verdict: step = kReject。
 */
PromptVerdict reject(PromptReason reason, std::size_t offset = 0, uint8_t byte = 0) noexcept
{
    PromptVerdict verdict{};
    verdict.reason = reason;
    verdict.offset = offset;
    verdict.byte = byte;
    return verdict;
}

/**
 * @brief 检查整行：必须以 "\r\n" 结尾，首位为 '<'，中间可打印。
 * @param frame: 已收到的字节（末字节为 '\n'）。
 * @retval std::nullopt: 通过。
 * @retval verdict: 拒绝原因。
 */
std::optional<PromptVerdict> checkLine(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if ((size < kMinPromptBytes) || (frame[size - 2U] != kCr)) {
        return reject(PromptReason::kNoCr, size - 1U, frame[size - 1U]);
    }
    for (size_t i = 1U; (i + 2U) < size; ++i) {
        if (!ascii::isPrintable(frame[i])) {
            return reject(PromptReason::kBody, i, frame[i]);
        }
    }
    return std::nullopt;
}

/**
 * @brief 统一的帧判定入口：explain 与 step 共用，保证诊断结论与实际成帧一致。
 * @param frame: 已收到的字节。
 * @retval verdict: kFrame / kNeedMore / kReject 之一。
 */
PromptVerdict judge(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (size == 0U) {
        return reject(PromptReason::kEmpty);
    }
    if (frame[size - 1U] == kLf) {
        if (const std::optional<PromptVerdict> line = checkLine(frame)) {
            return *line;
        }
        PromptVerdict verdict{};
        verdict.step = demux::Step::kFrame;
        return verdict;
    }
    // 末字节是 '\r' 时只差 '\n'（分块读取常在 CR 后断开），仍属有效前缀。
    if (!ascii::isPrintable(frame[size - 1U]) && (frame[size - 1U] != kCr)) {
        return reject(PromptReason::kBody, size - 1U, frame[size - 1U]);
    }
    PromptVerdict verdict{};
    verdict.step = demux::Step::kNeedMore;
    verdict.need = 1U;
    return verdict;
}

/**
 * @brief 把判定原因写成一句英文（explain 契约）。
 * @param verdict: judge 的结论。
 * @param text: 输出文本写入器。
 */
void explainVerdict(const PromptVerdict &verdict, demux::text_writer::TextWriter &text) noexcept
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

/** @brief 仅在要求改波特率时参与（portBaud == 0 时脚本提前结束）。 */
bool portBaudEnabled(const RoverConfig &cfg) noexcept
{
    return cfg.portBaud != 0U;
}

} // namespace

namespace comnav {

/**
 * @brief 提示符应答行的起始字节判定。
 * @param byte: 待判字节。
 * @retval true: 是 '<'。
 * @retval false: 其他。
 */
bool promptFirstByte(uint8_t byte) noexcept
{
    return byte == kPrompt;
}

/**
 * @brief 提示符应答行的成帧判定（demux 引擎每收到一字节调用，须 ISR 安全）。
 * @param frame: 已收到的字节。
 * @param need: 输出，至少还差多少字节（kNeedMore 时有效）。
 * @retval step: kFrame / kNeedMore / kReject。
 */
demux::Step promptStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    const PromptVerdict verdict = judge(frame);
    need = verdict.need;
    return verdict.step;
}

/**
 * @brief 把拒绝原因写成一句英文（诊断为“not rejected”表示帧本身没问题）。
 * @param frame: 已收到的字节。
 * @param out: 输出缓冲。
 * @retval len: 写入的字符数。
 */
std::size_t explainPrompt(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    demux::text_writer::TextWriter text(out);
    if (frame.empty()) {
        return text.literal("empty prefix").finish();
    }
    const PromptVerdict verdict = judge(frame);
    if (verdict.step == demux::Step::kFrame) {
        return text.literal("not rejected").finish();
    }
    explainVerdict(verdict, text);
    return text.finish();
}

} // namespace comnav

namespace detail {

// NOLINTNEXTLINE(readability-identifier-naming): 外部链接只读单例，命名见 DriverInstances.hpp
constinit const K803Receiver kK803Receiver{};

} // namespace detail

// ---- 命令库：每个函数只把一条完整命令（含 "\r\n"）写进 out，不关心什么时候发 ----

/**
 * @brief 关闭该口全部输出："UNLOGALL COMn"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool ComnavReceiver::cmdUnlogAll(CmdBuf &out, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOGALL COM") && ascii::appendUint(out, port) && out.append("\r\n");
}

/**
 * @brief 切到流动站模式："MODE ROVER"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool ComnavReceiver::cmdModeRover(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("MODE ROVER\r\n");
}

/**
 * @brief 改端口波特率："COM COMn <baud>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号。
 * @param baud: 目标波特率。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool ComnavReceiver::cmdComBaud(CmdBuf &out, uint8_t port, uint32_t baud) noexcept
{
    out.clear();
    return out.append("COM COM") && ascii::appendUint(out, port) && out.append(" ") &&
           ascii::appendUint(out, baud) && out.append("\r\n");
}

/**
 * @brief 以给定周期开启一类报文："LOG COMn <name> ONTIME <period>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGGA"）。
 * @param port: 接收机端口号。
 * @param periodMs: 周期（毫秒），必须是 10 ms 整数倍。
 * @retval true: 已写入完整命令。
 * @retval false: 周期粒度不合法或缓冲空间不足。
 */
bool ComnavReceiver::cmdLogOntime(CmdBuf &out, std::string_view name, uint8_t port,
                                  uint32_t periodMs) noexcept
{
    out.clear();
    return out.append("LOG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           out.append(name) && out.append(" ONTIME ") && ascii::appendPeriod(out, periodMs) &&
           out.append("\r\n");
}

/**
 * @brief 关闭一类报文："UNLOG COMn <name>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGSV"）。
 * @param port: 接收机端口号。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool ComnavReceiver::cmdUnlogMsg(CmdBuf &out, std::string_view name, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           out.append(name) && out.append("\r\n");
}

/**
 * @brief 探测用的版本查询："LOG VERSIONA ONCE"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool ComnavReceiver::cmdVersionQuery(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("LOG VERSIONA ONCE\r\n");
}

// ---- 应答判定：只判“成功/失败”，具体文本不进主流程 ----

/**
 * @brief 是否为确认应答（"<OK"）。
 * @param frame: 整帧字节。
 * @retval true: 以 "<OK" 开头。
 * @retval false: 其他。
 */
bool ComnavReceiver::respIsOk(std::span<const uint8_t> frame) noexcept
{
    return ascii::startsWith(frame, "<OK");
}

/**
 * @brief 是否为拒绝应答（"<ERROR"）。
 * @param frame: 整帧字节。
 * @retval true: 以 "<ERROR" 开头。
 * @retval false: 其他。
 */
bool ComnavReceiver::respIsError(std::span<const uint8_t> frame) noexcept
{
    return ascii::startsWith(frame, "<ERROR");
}

// ---- 驱动成员 ----

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
    return kDecoders;
}

uint32_t ComnavReceiver::caps(Model model) const noexcept
{
    static_cast<void>(model); // 单型号：K803 有 RTCM 输入与基站能力
    return cap::kRtcmIn | cap::kBase;
}

bool ComnavReceiver::buildProbe(CmdBuf &out) const noexcept
{
    return cmdVersionQuery(out);
}

bool ComnavReceiver::matchProbe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                ProbeResult &out) const noexcept
{
    if ((&dec != &demux::unicore_hash::kDecoder) || (frame.size() < kVersionTag.size()) ||
        !ascii::startsWith(frame, kVersionTag)) {
        return false;
    }
    size_t pos = kVersionTag.size();
    std::span<const uint8_t> modelText{};
    if (!ascii::nextQuoted(frame, pos, modelText) || !ascii::quotedStartsWith(modelText, "K803")) {
        return false;
    }
    out = ProbeResult{};
    out.model = Model::kK803;
    return true;
}

bool ComnavReceiver::roverStep(size_t index, Model model, const RoverConfig &cfg,
                               ConfigStep &out) const noexcept
{
    static_cast<void>(model); // 目前只覆盖 K803；报文输出改由输出阶段（buildOutput）下发
    // 表放在成员函数作用域内：步骤回调要调 protected 的 cmd_*，匿名命名空间的自由函数没有访问权。
    static constexpr std::array<ScriptStep, 3> kRoverScript{{
        {[](const RoverConfig &c, ConfigStep &o) noexcept {
             if (!cmdUnlogAll(o.cmd, c.port)) {
                 return false;
             }
             o.expectReply = true;
             o.timeoutMs = kConfigTimeoutMs;
             o.switchBaud = 0;
             return true;
         },
         &always},
        {[](const RoverConfig &c, ConfigStep &o) noexcept {
             static_cast<void>(c); // “MODE ROVER” 与配置无关
             if (!cmdModeRover(o.cmd)) {
                 return false;
             }
             o.expectReply = true;
             o.timeoutMs = kConfigTimeoutMs;
             o.switchBaud = 0;
             return true;
         },
         &always},
        {[](const RoverConfig &c, ConfigStep &o) noexcept {
             if (!cmdComBaud(o.cmd, c.port, c.portBaud)) {
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

Ack ComnavReceiver::classifyReply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  std::span<const uint8_t> sent) const noexcept
{
    static_cast<void>(sent); // NovAtel 应答不回显命令，只判类别
    if (&dec != &comnav::kPromptDecoder) {
        return Ack::kNone;
    }
    if (respIsOk(frame)) {
        return Ack::kAck;
    }
    if (respIsError(frame)) {
        return Ack::kNack;
    }
    return Ack::kNone;
}

uint32_t ComnavReceiver::outputMask(Model model) const noexcept
{
    // K803 只能输出这四类 NMEA；SATSINFOA/HEADINGA/GST 一律 kNotSupported（与旧脚本一致）。
    constexpr uint32_t kGgaBit = 1U << static_cast<uint32_t>(OutputMsg::kGga);
    constexpr uint32_t kRmcBit = 1U << static_cast<uint32_t>(OutputMsg::kRmc);
    constexpr uint32_t kGsaBit = 1U << static_cast<uint32_t>(OutputMsg::kGsa);
    constexpr uint32_t kGsvBit = 1U << static_cast<uint32_t>(OutputMsg::kGsv);
    static_cast<void>(model); // 单型号
    return kGgaBit | kRmcBit | kGsaBit | kGsvBit;
}

fw::Status ComnavReceiver::buildOutput(OutputMsg msg, uint32_t periodMs, Model model,
                                       const OutputPlan &plan, CmdBuf &out) const noexcept
{
    const auto idx = static_cast<size_t>(static_cast<uint8_t>(msg));
    if ((msg == OutputMsg::kNone) || (idx >= kOutputNames.size())) {
        return fw::Status::kInvalidArg;
    }
    if ((outputMask(model) & (1U << static_cast<uint32_t>(msg))) == 0U) {
        return fw::Status::kNotSupported;
    }
    const std::string_view name{kOutputNames[idx]};
    // period 0 = 关闭该类；旧 apply 旁路同样用 "UNLOG COMn <name>"。
    const bool written = (periodMs == 0U) ? cmdUnlogMsg(out, name, plan.port)
                                          : cmdLogOntime(out, name, plan.port, periodMs);
    if (!written) {
        out.clear();
        return fw::Status::kInvalidArg;
    }
    return fw::Status::kOk;
}

} // namespace gnss
