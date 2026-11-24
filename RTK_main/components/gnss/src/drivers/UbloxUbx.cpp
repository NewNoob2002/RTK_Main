// u-blox F9 的 UBX 帧层：帧组装（同步字/长度/Fletcher 校验）、CFG-VALSET 载荷拼接、ACK/NAK 判定（ADR-0010 §2.2/§2.4）。
// 线程安全：是（纯函数，无全局可变状态，可重入）。ISR 安全：是（无锁、不阻塞）。
// 堆与阻塞：不使用堆（帧在 CmdBuf 定长缓冲内构造）、不阻塞；键 ID 出自 F9 HPG 1.32 的 CFG-* 键值表。
#include "gnss/drivers/UbloxReceiver.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "UbloxExt.hpp"
#include "UbloxWire.hpp"

namespace gnss {
namespace {

constexpr uint8_t kTmodeDisabled = 0U;
constexpr uint8_t kTmodeSurveyIn = 1U;
constexpr uint8_t kTmodeFixed = 2U;
constexpr uint8_t kTmodePosLlh = 1U;
constexpr uint32_t kCmTo01Mm = 100U;
constexpr uint32_t kFixedPosAcc01Mm = 100U; // 10 mm
constexpr int64_t kE9PerE7 = 100;
constexpr int32_t kMmPerCm = 10;
constexpr int32_t k01MmPerMm = 10;

// 与 kBaseRtcmIds 1:1 对应的 F9P UART1 MSGOUT 键（0 = F9P 不支持）。
constexpr std::array<uint32_t, kBaseRtcmCount> kUbloxRtcmUart1Keys{
    ublox::kKeyMsgoutRtcm1005Uart1, // 1005
    0U,                             // 1006
    0U,                             // 1033
    ublox::kKeyMsgoutRtcm1230Uart1, // 1230
    ublox::kKeyMsgoutRtcm1074Uart1, // 1074
    ublox::kKeyMsgoutRtcm1084Uart1, // 1084
    ublox::kKeyMsgoutRtcm1094Uart1, // 1094
    ublox::kKeyMsgoutRtcm1124Uart1, // 1124
    ublox::kKeyMsgoutRtcm1077Uart1, // 1077
    ublox::kKeyMsgoutRtcm1087Uart1, // 1087
    ublox::kKeyMsgoutRtcm1097Uart1, // 1097
    ublox::kKeyMsgoutRtcm1127Uart1, // 1127
};

/**
 * @brief 将基站 RTCM 周期（毫秒）换算为 F9P 每历元发送间隔。
 * @param periodMs: 周期（0 表示关闭）。
 * @param navRateHz: 导航率（Hz）。
 * @retval epochs: 0..255，每 epochs 个导航历元发送 1 次。
 */
uint8_t rtcmPeriodToEpochs(uint32_t periodMs, uint8_t navRateHz) noexcept
{
    if (periodMs == 0U) {
        return 0U;
    }
    const uint32_t rate = (navRateHz == 0U) ? ublox::kMinRateHz : static_cast<uint32_t>(navRateHz);
    const uint64_t scaled = (static_cast<uint64_t>(periodMs) * rate) + ublox::kHalfSecondMs;
    uint64_t epochs = scaled / ublox::kMsPerSecond;
    if (epochs == 0ULL) {
        epochs = 1ULL;
    }
    if (epochs > ublox::kMaxEpochsPerEpoch) {
        epochs = ublox::kMaxEpochsPerEpoch;
    }
    return static_cast<uint8_t>(epochs);
}
} // namespace

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
    return ublox::encodeUbxFrame(out, cls, id, payload);
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
    std::array<uint8_t, ublox::kValsetPayloadMax> payload{};
    ublox::ValsetWriter writer{payload};
    if (!writer.begin(ublox::kLayerRamBbr) || !writer.addU2(ublox::kKeyRateMeas, periodMs) ||
        !writer.addU1(ublox::kKeyUart1InprotRtcm3x, 1U) ||
        !writer.addU1(ublox::kKeyUart1OutprotNmea, 1U)) {
        return false;
    }
    return cmdBuildFrame(out, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
}

/**
 * @brief 设置导航周期、打开接线口 RTCM3 输入与 NMEA 输出，并启用 NMEA 高精度与扩展卫星编号。
 * @param out: 目标命令缓冲。
 * @param periodMs: 导航周期，毫秒。
 * @param port: 接线口号，UART1 或 UART2。
 * @retval true 已写入完整帧。
 * @retval false 命令缓冲不足或端口非法。
 * @note 纯编码，可重入，ISR 安全。键见 UBX-22008968 §6.9.15、§6.9.35–36。
 */
bool UbloxReceiver::cmdConfigurePort(CmdBuf &out, uint16_t periodMs, uint8_t port) noexcept
{
    constexpr uint32_t kUart2RtcmInput = 0x10750004U;
    constexpr uint32_t kUart2NmeaOutput = 0x10760002U;
    if (port != 1U && port != 2U) {
        return false;
    }
    const uint32_t rtcmKey = port == 2U ? kUart2RtcmInput : ublox::kKeyUart1InprotRtcm3x;
    const uint32_t nmeaKey = port == 2U ? kUart2NmeaOutput : ublox::kKeyUart1OutprotNmea;
    std::array<uint8_t, ublox::kValsetPayloadMax> payload{};
    ublox::ValsetWriter writer{payload};
    return writer.begin(ublox::kLayerRam) && writer.addU2(ublox::kKeyRateMeas, periodMs) &&
           writer.addU1(rtcmKey, 1U) && writer.addU1(nmeaKey, 1U) &&
           writer.addU1(ublox::kKeyNmeaHighprec, 1U) &&
           writer.addU1(ublox::kKeyNmeaSvNumbering, 1U) &&
           cmdBuildFrame(out, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
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
    std::array<uint8_t, ublox::kValsetPayloadMax> payload{};
    ublox::ValsetWriter writer{payload};
    if (!writer.begin(ublox::kLayerRamBbr) || !writer.addU4(ublox::kKeyUart1Baudrate, baud)) {
        return false;
    }
    return cmdBuildFrame(out, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
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
    return cmdValsetU1(out, ublox::kLayerRamBbr, key, epochs);
}

/**
 * @brief 编码带四字节头的单键 U1/逻辑值 CFG-VALSET。
 * @param out: 目标命令缓冲。
 * @param layer: 目标配置层位图。
 * @param key: 配置键。
 * @param value: 一字节值。
 * @retval true 已编码；false 缓冲不足。
 */
bool UbloxReceiver::cmdValsetU1(CmdBuf &out, uint8_t layer, uint32_t key, uint8_t value) noexcept
{
    std::array<uint8_t, ublox::kMsgoutPayloadMax> payload{};
    ublox::ValsetWriter writer{payload};
    return writer.begin(layer) && writer.addU1(key, value) &&
           cmdBuildFrame(out, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
}

// ---- 应答判定：只判帧结构与 ACK 类别，不看具体配置内容 ----

/**
 * @brief UBX 帧的载荷长度（长度字段，小端）。
 * @param frame: 整帧字节。
 * @retval 长度: 载荷字节数；帧头不足时返回 0。
 */
size_t UbloxReceiver::respPayloadLen(std::span<const uint8_t> frame) noexcept
{
    if (frame.size() < (ublox::kLenOffset + 2U)) {
        return 0U;
    }
    return static_cast<size_t>(frame[ublox::kLenOffset]) |
           (static_cast<size_t>(frame[ublox::kLenOffset + 1U]) << ublox::kShift1Byte);
}

/**
 * @brief 是否为结构完整的 UBX-ACK-ACK / UBX-ACK-NAK。
 * @param frame: 整帧字节。
 * @retval true: class 0x05、id 0x00/0x01、载荷 2 字节、总长正好。
 * @retval false: 其他帧。
 */
bool UbloxReceiver::respHasValidAck(std::span<const uint8_t> frame) noexcept
{
    if ((frame.size() != (ublox::kHeaderBytes + ublox::kAckPayloadBytes + ublox::kCrcBytes)) ||
        (frame[ublox::kClsOffset] != ublox::kClsAck)) {
        return false;
    }
    if ((frame[ublox::kIdOffset] != ublox::kIdAck) && (frame[ublox::kIdOffset] != ublox::kIdNak)) {
        return false;
    }
    return respPayloadLen(frame) == ublox::kAckPayloadBytes;
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
    return (frame[ublox::kPayloadOffset] == cls) && (frame[ublox::kPayloadOffset + 1U] == id);
}

bool UbloxReceiver::saveStep(ConfigStep &out) const noexcept
{
    constexpr uint16_t kSaveTimeoutMs = 1800U;
    constexpr uint8_t kBbrAndFlash = 0x03U;
    constexpr size_t kCfgBytes = 13U;
    // F9 HPG 1.32 §3.10.2：任一 saveMask 位保存完整 RAM 配置。
    constexpr std::array<uint8_t, kCfgBytes> kSave{0, 0, 0, 0, 1, 0,           0,
                                                   0, 0, 0, 0, 0, kBbrAndFlash};
    out = ConfigStep{.timeoutMs = kSaveTimeoutMs};
    return cmdBuildFrame(out.cmd, ublox::kClsCfg, ublox::kIdCfgCfg, kSave);
}

/**
 * @brief 判断出厂缺省可在 UART2 接收 UBX 的 F9P 固件版本。
 * @param detected: MON-VER 解析结果。
 * @retval true HPG 1.32 或更新的 HPG 1.x；false 未确认。
 * @note UBX-18010802 §3.1.3 给出 1.32 的门槛。纯判断，可重入，ISR 安全。
 */
static bool uart2FactoryUbxInput(const ProbeResult &detected) noexcept
{
    const std::string_view version{detected.fwVersion.data(), detected.fwVersion.size()};
    constexpr std::string_view kHpgOne = "HPG 1.";
    if (!version.starts_with(kHpgOne)) {
        return false;
    }
    const auto minor = version.substr(kHpgOne.size(), 2U);
    if (minor.size() != 2U) {
        return false;
    }
    constexpr unsigned kFirstUbxInputMinor = 32U;
    unsigned value = 0U;
    // 两个字符都属于固件版本的定长缓冲区。
    const char *const begin = minor.data();
    const char *const end =
        begin + minor.size(); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end && value >= kFirstUbxInputMinor;
}

bool UbloxReceiver::factoryResetStep(uint8_t port, const ProbeResult &detected,
                                     ConfigStep &out) const noexcept
{
    if (port != 1U && (port != 2U || !uart2FactoryUbxInput(detected))) {
        return false;
    }
    constexpr uint32_t kFactoryBaud = 38400U;
    constexpr uint8_t kBbrAndFlash = 0x03U;
    constexpr size_t kCfgBytes = 13U;
    constexpr std::array<uint8_t, kCfgBytes> kClearLoad{1, 0, 0, 0, 0, 0,           0,
                                                        0, 1, 0, 0, 0, kBbrAndFlash};
    out = ConfigStep{.expectReply = false, .switchBaud = kFactoryBaud};
    return cmdBuildFrame(out.cmd, ublox::kClsCfg, ublox::kIdCfgCfg, kClearLoad);
}

bool UbloxReceiver::buildFactoryResetProbe(uint8_t port, CmdBuf &out) const noexcept
{
    if (port == 1U) {
        return buildProbe(out);
    }
    // UART2 缺省关闭 UBX 输出，先在 RAM|BBR 恢复输出协议，再轮询 MON-VER。
    constexpr uint32_t kUart2UbxOutput = 0x10760001U;
    CmdBuf probe{};
    return port == 2U && cmdValsetU1(out, ublox::kLayerRamBbr, kUart2UbxOutput, 1U) &&
           buildProbe(probe) && out.append(probe.view());
}

bool UbloxReceiver::baudStep(uint8_t port, uint32_t baud, ConfigStep &out) const noexcept
{
    constexpr uint32_t kUart2Baud = 0x40530001U;
    if (port != 1U && port != 2U) {
        return false;
    }
    out = ConfigStep{.expectReply = false, .switchBaud = baud};
    std::array<uint8_t, ublox::kValsetPayloadMax> payload{};
    ublox::ValsetWriter writer{payload};
    const uint32_t key = port == 1U ? ublox::kKeyUart1Baudrate : kUart2Baud;
    // 切速只改 RAM 层，接收机持久化由手动保存触发。
    return writer.begin(ublox::kLayerRam) && writer.addU4(key, baud) &&
           cmdBuildFrame(out.cmd, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
}

/**
 * @brief 向 CFG-VALSET 载荷写入 RTCM 输出协议开关与各基站 RTCM MSGOUT 频率。
 * @param writer: 载荷写入器。
 * @param req: 工作模式请求。
 * @param port: 目标端口（1 或 2）。
 * @param enable: true 启用并按请求写周期，false 全关。
 * @retval true: 写入成功。
 * @retval false: 载荷空间不足。
 */
static bool fillRtcmMsgoutPayload(ublox::ValsetWriter &writer, const ModeRequest &req, uint8_t port,
                                  bool enable) noexcept
{
    const uint32_t outprotKey =
        (port == 2U) ? ublox::kKeyUart2OutprotRtcm3x : ublox::kKeyUart1OutprotRtcm3x;
    if (!writer.begin(ublox::kLayerRam) || !writer.addU1(outprotKey, enable ? 1U : 0U)) {
        return false;
    }
    for (size_t i = 0U; i < kBaseRtcmCount; ++i) {
        uint32_t key = kUbloxRtcmUart1Keys[i];
        if (key == 0U) {
            continue;
        }
        if (port == 2U) {
            ++key;
        }
        const uint8_t epochs = enable ? rtcmPeriodToEpochs(req.rtcmPeriodMs[i], req.navRateHz) : 0U;
        if (!writer.addU1(key, epochs)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 向 CFG-VALSET 载荷写入固定坐标基站（TMODE=2）的 LLH 高精度字段。
 * @param writer: 载荷写入器。
 * @param req: 工作模式请求。
 * @retval true: 写入成功。
 * @retval false: 载荷空间不足。
 */
static bool fillTmodeFixedPayload(ublox::ValsetWriter &writer, const ModeRequest &req) noexcept
{
    const auto latE7 = static_cast<int32_t>(req.fixLatE9 / kE9PerE7);
    const auto latHp = static_cast<int8_t>(req.fixLatE9 % kE9PerE7);
    const auto lonE7 = static_cast<int32_t>(req.fixLonE9 / kE9PerE7);
    const auto lonHp = static_cast<int8_t>(req.fixLonE9 % kE9PerE7);
    const int32_t heightCm = req.fixHeightMm / kMmPerCm;
    const auto heightHp = static_cast<int8_t>((req.fixHeightMm % kMmPerCm) * k01MmPerMm);
    return writer.begin(ublox::kLayerRam) && writer.addU1(ublox::kKeyTmodeMode, kTmodeFixed) &&
           writer.addU1(ublox::kKeyTmodePosType, kTmodePosLlh) &&
           writer.addI4(ublox::kKeyTmodeLat, latE7) && writer.addI1(ublox::kKeyTmodeLatHp, latHp) &&
           writer.addI4(ublox::kKeyTmodeLon, lonE7) && writer.addI1(ublox::kKeyTmodeLonHp, lonHp) &&
           writer.addI4(ublox::kKeyTmodeHeight, heightCm) &&
           writer.addI1(ublox::kKeyTmodeHeightHp, heightHp) &&
           writer.addU4(ublox::kKeyTmodeFixedPosAcc, kFixedPosAcc01Mm);
}

bool UbloxReceiver::modeStep(const ModeRequest &req, uint8_t idx, ConfigStep &out) const noexcept
{
    const uint8_t port = req.useConfiguredPort ? req.port : 1U;
    if ((port != 1U) && (port != 2U)) {
        return false;
    }
    std::array<uint8_t, ublox::kValsetPayloadMax> payload{};
    ublox::ValsetWriter writer{payload};
    bool ok = false;
    switch (req.action) {
    case ModeAction::kRover:
    case ModeAction::kBasePrep:
        if (idx == 0U) {
            ok = writer.begin(ublox::kLayerRam) &&
                 writer.addU1(ublox::kKeyTmodeMode, kTmodeDisabled);
        } else if (idx == 1U) {
            ok = fillRtcmMsgoutPayload(writer, req, port, req.action == ModeAction::kBasePrep);
        }
        break;
    case ModeAction::kBaseSurvey:
        if ((idx == 0U) && (req.surveySeconds != 0U)) {
            const uint32_t acc01Mm = static_cast<uint32_t>(req.surveyAccCm) * kCmTo01Mm;
            ok = writer.begin(ublox::kLayerRam) &&
                 writer.addU1(ublox::kKeyTmodeMode, kTmodeSurveyIn) &&
                 writer.addU4(ublox::kKeyTmodeSvinMinDur, req.surveySeconds) &&
                 writer.addU4(ublox::kKeyTmodeSvinAccLimit, acc01Mm);
        }
        break;
    case ModeAction::kBaseFixed:
        if (idx == 0U) {
            ok = fillRtcmMsgoutPayload(writer, req, port, true);
        } else if (idx == 1U) {
            ok = fillTmodeFixedPayload(writer, req);
        }
        break;
    default:
        break;
    }
    if (!ok) {
        return false;
    }
    out = ConfigStep{.timeoutMs = ublox::kConfigTimeoutMs};
    return cmdBuildFrame(out.cmd, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
}

} // namespace gnss
