// u-blox F9（ZED-F9P）驱动实现：UBX-MON-VER 探测、CFG-VALSET 配置、ACK/NAK 匹配（ADR-0010 §2.2/§2.4）。
// 结构（P3s）：命令生成（cmd_*）与应答判定（resp_*）是 UbloxReceiver 的 protected static，子类可直接复用；
// 脚本步骤的回调写在 roverStep 内（要取 protected 成员的地址，匿名命名空间的自由函数没有访问权）。
// 载荷写入器（Writer）与 CFG-VALSET 载荷拼接留在本文件内：Writer 是 .cpp 私有类型，不能出现在类声明里。
// 线程/调用约束：纯函数，无全局可变状态，可重入、不阻塞。
// 堆：不使用（命令在 CmdBuf 定长缓冲内构造）。
// 键 ID 出处：u-blox F9 HPG 1.32 Interface Description，CFG-* 配置项键值表（MSGOUT NMEA 段、RATE 段、
// UART1 段）；本仓库无该手册副本，ID 未经实物验证（仅 UM980 有 HIL）。
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

namespace gnss {
namespace {

// UBX 帧头与消息 ID
constexpr uint8_t kSync1 = 0xB5;
constexpr uint8_t kSync2 = 0x62;
constexpr uint8_t kClsCfg = 0x06;
constexpr uint8_t kIdCfgValset = 0x8A;
constexpr uint8_t kClsAck = 0x05;
constexpr uint8_t kIdAck = 0x01;
constexpr uint8_t kIdNak = 0x00;
constexpr uint8_t kClsMon = 0x0A;
constexpr uint8_t kIdMonVer = 0x04;
// 配置层：RAM | BBR（ADR-0010 §2.4 不写模块 flash）
constexpr uint8_t kLayerRamBbr = 0x03;
constexpr uint8_t kVersetVersion = 0x00;
constexpr uint8_t kByteMask = 0xFFU;
// 小端拼接的移位位数（每字节 8 位）。
constexpr uint8_t kShift1Byte = 8U;
constexpr uint8_t kShift2Bytes = 16U;
constexpr uint8_t kShift3Bytes = 24U;
constexpr size_t kHeaderBytes = 6U;
constexpr size_t kCrcBytes = 2U;
constexpr size_t kAckPayloadBytes = 2U;
constexpr size_t kSyncBytes = 2U;
constexpr size_t kLenOffset = 4U;
constexpr size_t kIdOffset = 3U;
constexpr size_t kClsOffset = 2U;
constexpr size_t kPayloadOffset = 6U;
constexpr size_t kMaxPayload = 0xFFFFU;
constexpr size_t kValsetHeadBytes = 3U;
constexpr size_t kValsetPayloadMax = 64U;
constexpr size_t kMsgoutPayloadMax = 16U;
constexpr size_t kU1ItemBytes = 5U;
constexpr size_t kU2ItemBytes = 6U;
constexpr size_t kU4ItemBytes = 8U;
constexpr uint16_t kMonVerMinLen = 40U;
constexpr size_t kMonVerExtBytes = 30U;
constexpr size_t kConfigTimeoutMs = 500U;
constexpr uint16_t kDefaultPeriodMs = 1000U;
constexpr uint8_t kMaxRateHz = 20U;
constexpr uint8_t kMinRateHz = 1U;
constexpr uint32_t kMaxEpochsPerEpoch = 255U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint32_t kHalfSecondMs = kMsPerSecond / 2U;

// 键 ID（见文件头出处）
constexpr uint32_t kKeyRateMeas = 0x30210001U;          // CFG-RATE-MEAS, U2
constexpr uint32_t kKeyUart1InprotRtcm3x = 0x10730004U; // CFG-UART1INPROT-RTCM3X, L
constexpr uint32_t kKeyUart1OutprotNmea = 0x10740002U;  // CFG-UART1OUTPROT-NMEA, L
constexpr uint32_t kKeyMsgoutGgaUart1 = 0x209100BBU;    // CFG-MSGOUT-NMEA_ID_GGA_UART1, U1
constexpr uint32_t kKeyMsgoutRmcUart1 = 0x209100ACU;    // CFG-MSGOUT-NMEA_ID_RMC_UART1, U1
constexpr uint32_t kKeyMsgoutGsaUart1 = 0x209100BFU;    // CFG-MSGOUT-NMEA_ID_GSA_UART1, U1
constexpr uint32_t kKeyMsgoutGsvUart1 = 0x209100C3U;    // CFG-MSGOUT-NMEA_ID_GSV_UART1, U1
constexpr uint32_t kKeyMsgoutGstUart1 = 0x209100D4U;    // CFG-MSGOUT-NMEA_ID_GST_UART1, U1
constexpr uint32_t kKeyUart1Baudrate = 0x40520001U;     // CFG-UART1-BAUDRATE, U4

/** MON-VER 轮询帧（空载荷）已算好的 Fletcher 校验：载荷为空，校验只覆盖 class/id/len。 */
constexpr uint8_t kMonVerPollCkA = 0x0E;
constexpr uint8_t kMonVerPollCkB = 0x34;
constexpr size_t kMonVerPollBytes = 8U;

// MON-VER 探测应答与 ACK/NAK 都是 UBX 帧（matchProbe / classifyReply 均按 ubx 解码器判定）。
constexpr std::array<const demux::Decoder *, 1> kDecoders{&demux::ubx::kDecoder};

/** 载荷写入器：定长缓冲 + 写点下标（不用指针算术，越界即失败）。 */
struct Writer {
    std::span<uint8_t> buf{};
    size_t pos = 0;

    /**
     * @brief 追加 1 字节。
     * @param v: 待写值。
     * @retval true: 写入成功。
     * @retval false: 空间不足。
     */
    bool u8(uint8_t v) noexcept
    {
        if (pos >= buf.size()) {
            return false;
        }
        buf[pos] = v;
        ++pos;
        return true;
    }

    /**
     * @brief 追加 2 字节（小端）。
     * @param v: 待写值。
     * @retval true: 写入成功。
     * @retval false: 空间不足。
     */
    bool u16(uint16_t v) noexcept
    {
        const auto value = static_cast<uint32_t>(v);
        return u8(static_cast<uint8_t>(value & kByteMask)) &&
               u8(static_cast<uint8_t>((value >> kShift1Byte) & kByteMask));
    }

    /**
     * @brief 追加 4 字节（小端）。
     * @param v: 待写值。
     * @retval true: 写入成功。
     * @retval false: 空间不足。
     */
    bool u32(uint32_t v) noexcept
    {
        return u8(static_cast<uint8_t>(v & kByteMask)) &&
               u8(static_cast<uint8_t>((v >> kShift1Byte) & kByteMask)) &&
               u8(static_cast<uint8_t>((v >> kShift2Bytes) & kByteMask)) &&
               u8(static_cast<uint8_t>((v >> kShift3Bytes) & kByteMask));
    }

    /** @brief 已写入的字节视图。 */
    std::span<const uint8_t> view() const noexcept
    {
        return buf.first(pos);
    }
};

/**
 * @brief 累积 UBX Fletcher 校验（命令库与应答判定共用，放在文件内）。
 * @param byte: 新字节。
 * @param ckA: 入参与输出，校验字节 A。
 * @param ckB: 入参与输出，校验字节 B。
 */
void fletcher(uint8_t byte, uint8_t &ckA, uint8_t &ckB) noexcept
{
    ckA = static_cast<uint8_t>(ckA + byte);
    ckB = static_cast<uint8_t>(ckB + ckA);
}

/**
 * @brief 写 CFG-VALSET 公共前缀（version=0，layer=RAM|BBR）。
 * @param writer: 载荷写入器。
 * @retval true: 写入成功。
 * @retval false: 空间不足。
 */
bool beginValset(Writer &writer) noexcept
{
    return writer.u8(kVersetVersion) && writer.u8(kLayerRamBbr) && writer.u8(0x00) &&
           (writer.pos == kValsetHeadBytes);
}

/**
 * @brief 追加一个 U1 键值项（键 4 字节 + 值 1 字节）。
 * @param writer: 载荷写入器。
 * @param key: 配置键。
 * @param value: 值。
 * @retval true: 写入成功。
 * @retval false: 空间不足。
 */
bool addU1Item(Writer &writer, uint32_t key, uint8_t value) noexcept
{
    const size_t start = writer.pos;
    return writer.u32(key) && writer.u8(value) && ((writer.pos - start) == kU1ItemBytes);
}

/**
 * @brief 追加一个 U2 键值项（键 4 字节 + 值 2 字节）。
 * @param writer: 载荷写入器。
 * @param key: 配置键。
 * @param value: 值。
 * @retval true: 写入成功。
 * @retval false: 空间不足。
 */
bool addU2Item(Writer &writer, uint32_t key, uint16_t value) noexcept
{
    const size_t start = writer.pos;
    return writer.u32(key) && writer.u16(value) && ((writer.pos - start) == kU2ItemBytes);
}

/**
 * @brief 追加一个 U4 键值项（键 4 字节 + 值 4 字节）。
 * @param writer: 载荷写入器。
 * @param key: 配置键。
 * @param value: 值。
 * @retval true: 写入成功。
 * @retval false: 空间不足。
 */
bool addU4Item(Writer &writer, uint32_t key, uint32_t value) noexcept
{
    const size_t start = writer.pos;
    return writer.u32(key) && writer.u32(value) && ((writer.pos - start) == kU4ItemBytes);
}

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

// ---- 命令库：每个函数只组一条完整 UBX 帧（同步字 + class/id + 长度 + 载荷 + Fletcher 校验） ----

/**
 * @brief 组装一帧 UBX：B5 62 | class | id | len(LE) | payload | CK_A CK_B（覆盖 class..payload）。
 * @param out: 目标命令缓冲（先清空）。
 * @param cls: 消息 class。
 * @param id: 消息 id。
 * @param payload: 载荷字节。
 * @retval true: 已写入完整帧。
 * @retval false: 载荷超长或缓冲不足。
 */
bool UbloxReceiver::cmdBuildFrame(CmdBuf &out, uint8_t cls, uint8_t id,
                                  std::span<const uint8_t> payload) noexcept
{
    if (payload.size() > kMaxPayload) {
        return false;
    }
    const std::array<uint8_t, kHeaderBytes> header{
        kSync1,
        kSync2,
        cls,
        id,
        static_cast<uint8_t>(payload.size() & kByteMask),
        static_cast<uint8_t>((payload.size() >> kShift1Byte) & kByteMask)};
    uint8_t ckA = 0;
    uint8_t ckB = 0;
    for (size_t i = kSyncBytes; i < header.size(); ++i) {
        fletcher(header[i], ckA, ckB);
    }
    for (const uint8_t b : payload) {
        fletcher(b, ckA, ckB);
    }
    const std::array<uint8_t, kCrcBytes> checksum{ckA, ckB};
    out.clear();
    return out.append(header) && out.append(payload) && out.append(checksum);
}

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

/**
 * @brief 基础配置：CFG-RATE-MEAS + 打开 RTCM3X 输入协议 + 打开 NMEA 输出协议。
 * @param out: 目标命令缓冲。
 * @param periodMs: 导航周期（毫秒）。
 * @retval true: 已写入完整帧。
 * @retval false: 载荷或缓冲空间不足。
 */
bool UbloxReceiver::cmdConfigureReceiver(CmdBuf &out, uint16_t periodMs) noexcept
{
    std::array<uint8_t, kValsetPayloadMax> payload{};
    Writer writer{payload};
    if (!beginValset(writer) || !addU2Item(writer, kKeyRateMeas, periodMs) ||
        !addU1Item(writer, kKeyUart1InprotRtcm3x, 1U) ||
        !addU1Item(writer, kKeyUart1OutprotNmea, 1U)) {
        return false;
    }
    return cmdBuildFrame(out, kClsCfg, kIdCfgValset, writer.view());
}

/**
 * @brief 改 UART1 波特率（单键 VALSET）。
 * @param out: 目标命令缓冲。
 * @param baud: 目标波特率。
 * @retval true: 已写入完整帧。
 * @retval false: 载荷或缓冲空间不足。
 */
bool UbloxReceiver::cmdSetBaudrate(CmdBuf &out, uint32_t baud) noexcept
{
    std::array<uint8_t, kValsetPayloadMax> payload{};
    Writer writer{payload};
    if (!beginValset(writer) || !addU4Item(writer, kKeyUart1Baudrate, baud)) {
        return false;
    }
    return cmdBuildFrame(out, kClsCfg, kIdCfgValset, writer.view());
}

/**
 * @brief 单键 CFG-MSGOUT VALSET：值 = 每历元输出次数（0 关闭）。
 * @param out: 目标命令缓冲。
 * @param key: CFG-MSGOUT-*_UART1 的键。
 * @param epochs: 每历元次数。
 * @retval true: 已写入完整帧。
 * @retval false: 载荷或缓冲空间不足。
 */
bool UbloxReceiver::cmdValsetMsgout(CmdBuf &out, uint32_t key, uint8_t epochs) noexcept
{
    std::array<uint8_t, kMsgoutPayloadMax> payload{};
    Writer writer{payload};
    if (!beginValset(writer) || !addU1Item(writer, key, epochs)) {
        return false;
    }
    return cmdBuildFrame(out, kClsCfg, kIdCfgValset, writer.view());
}

// ---- 应答判定：只判帧结构与 ACK 类别，不看具体配置内容 ----

/**
 * @brief UBX 帧的载荷长度（长度字段，小端）。
 * @param frame: 整帧字节。
 * @retval 长度: 载荷字节数；帧头不足时返回 0。
 */
size_t UbloxReceiver::respPayloadLen(std::span<const uint8_t> frame) noexcept
{
    if (frame.size() < (kLenOffset + 2U)) {
        return 0U;
    }
    return static_cast<size_t>(frame[kLenOffset]) |
           (static_cast<size_t>(frame[kLenOffset + 1U]) << kShift1Byte);
}

/**
 * @brief 是否为结构完整的 UBX-ACK-ACK / UBX-ACK-NAK。
 * @param frame: 整帧字节。
 * @retval true: class 0x05、id 0x00/0x01、载荷 2 字节、总长正好。
 * @retval false: 其他帧。
 */
bool UbloxReceiver::respHasValidAck(std::span<const uint8_t> frame) noexcept
{
    if ((frame.size() != (kHeaderBytes + kAckPayloadBytes + kCrcBytes)) ||
        (frame[kClsOffset] != kClsAck)) {
        return false;
    }
    if ((frame[kIdOffset] != kIdAck) && (frame[kIdOffset] != kIdNak)) {
        return false;
    }
    return respPayloadLen(frame) == kAckPayloadBytes;
}

/**
 * @brief ACK/NAK 载荷是否指向给定的 class/id（即“被 ACK 的是哪条命令”）。
 * @param frame: 已通过 respHasValidAck 的帧。
 * @param cls: 期望被应答的 class。
 * @param id: 期望被应答的 id。
 * @retval true: 载荷等于 cls/id。
 * @retval false: 其他命令的应答。
 */
bool UbloxReceiver::respAckClassId(std::span<const uint8_t> frame, uint8_t cls, uint8_t id) noexcept
{
    return (frame[kPayloadOffset] == cls) && (frame[kPayloadOffset + 1U] == id);
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

/** @brief ZED-F9P 叶子单例。 @return constinit 实例的引用。 @note 无运行期状态，ISR 安全。 */
const Receiver &ubloxReceiver() noexcept
{
    return detail::kZedF9pReceiver;
}

} // namespace gnss
