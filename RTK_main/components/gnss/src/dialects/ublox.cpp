// u-blox F9（ZED-F9P）方言实现：UBX-MON-VER 探测、CFG-VALSET 配置、ACK/NAK 匹配（ADR-0010 §2.2/§2.4）。
// 线程/调用约束：纯函数，无全局可变状态，可重入、不阻塞。
// 堆：不使用（命令在 CmdBuf 定长缓冲内构造）。
// 键 ID 出处：u-blox F9 HPG 1.32 Interface Description，CFG-* 配置项键值表（MSGOUT NMEA 段、RATE 段、
// UART1 段）；本仓库无该手册副本，ID 未经实物验证（仅 UM980 有 HIL）。
#include "gnss/dialects/ublox.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoders/ubx.hpp"

namespace gnss::ublox {
namespace {

// UBX 帧头
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

void put_u16(uint8_t *p, uint16_t v) noexcept
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void put_u32(uint8_t *p, uint32_t v) noexcept
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

void fletcher(uint8_t byte, uint8_t &ck_a, uint8_t &ck_b) noexcept
{
    ck_a = static_cast<uint8_t>(ck_a + byte);
    ck_b = static_cast<uint8_t>(ck_b + ck_a);
}

// 组装一帧 UBX：B5 62 | class | id | len(LE) | payload | CK_A CK_B（覆盖 class..payload）。
bool build_frame(CmdBuf &out, uint8_t cls, uint8_t id, std::span<const uint8_t> payload) noexcept
{
    if (payload.size() > 0xFFFFU) {
        return false;
    }
    const std::array<uint8_t, 6> header{kSync1,
                                        kSync2,
                                        cls,
                                        id,
                                        static_cast<uint8_t>(payload.size() & 0xFFU),
                                        static_cast<uint8_t>((payload.size() >> 8) & 0xFFU)};
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    for (size_t i = 2; i < header.size(); ++i) {
        fletcher(header[i], ck_a, ck_b);
    }
    for (const uint8_t b : payload) {
        fletcher(b, ck_a, ck_b);
    }
    const std::array<uint8_t, 2> checksum{ck_a, ck_b};
    out.clear();
    return out.append(header) && out.append(payload) && out.append(checksum);
}

// VALSET 载荷公共前缀：version=0，layer=RAM|BBR。
size_t begin_valset(uint8_t *payload) noexcept
{
    payload[0] = 0x00;
    payload[1] = kLayerRamBbr;
    payload[2] = 0x00;
    return 3;
}

size_t add_u2_item(uint8_t *payload, size_t pos, uint32_t key, uint16_t value) noexcept
{
    put_u32(&payload[pos], key);
    put_u16(&payload[pos + 4], value);
    return pos + 6;
}

size_t add_u1_item(uint8_t *payload, size_t pos, uint32_t key, uint8_t value) noexcept
{
    put_u32(&payload[pos], key);
    payload[pos + 4] = value;
    return pos + 5;
}

size_t add_u4_item(uint8_t *payload, size_t pos, uint32_t key, uint32_t value) noexcept
{
    put_u32(&payload[pos], key);
    put_u32(&payload[pos + 4], value);
    return pos + 8;
}

bool text_starts_with(std::span<const uint8_t> text, const char *prefix) noexcept
{
    size_t len = 0;
    while (prefix[len] != '\0') {
        if (len >= text.size() || text[len] != static_cast<uint8_t>(prefix[len])) {
            return false;
        }
        ++len;
    }
    return true;
}

// MON-VER 的 30 字节扩展项（"MOD=..." / "FWVER=..."，NUL 填充）。
void copy_text(std::span<const uint8_t> text, std::array<char, kVersionLen> &out) noexcept
{
    size_t len = 0;
    while (len < text.size() && text[len] != 0) {
        ++len;
    }
    const size_t copy = len < (kVersionLen - 1) ? len : (kVersionLen - 1);
    for (size_t i = 0; i < copy; ++i) {
        out[i] = static_cast<char>(text[i]);
    }
    out[copy] = '\0';
}

} // namespace

bool build_probe(CmdBuf &out) noexcept
{
    out.clear();
    return out.append(std::array<uint8_t, 8>{kSync1, kSync2, kClsMon, kIdMonVer, 0x00, 0x00, 0x0E, 0x34});
}

bool match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame, ProbeResult &out) noexcept
{
    if (&dec != &demux::ubx::kDecoder || frame.size() < 8) {
        return false;
    }
    if (frame[2] != kClsMon || frame[3] != kIdMonVer) {
        return false;
    }
    const size_t len = static_cast<size_t>(frame[4]) | (static_cast<size_t>(frame[5]) << 8);
    if (len < 40 || frame.size() < 8 + len) {
        return false;
    }
    const std::span<const uint8_t> payload = frame.subspan(6, len);

    bool have_model = false;
    ProbeResult result{};
    for (size_t off = 40; off + 30 <= len; off += 30) {
        const std::span<const uint8_t> ext = payload.subspan(off, 30);
        if (text_starts_with(ext, "MOD=")) {
            if (!text_starts_with(ext.subspan(4), "ZED-F9P")) {
                return false; // 同一 UBX 家族的其他型号（如 F9R/F9H）不认作本方言
            }
            have_model = true;
        } else if (text_starts_with(ext, "FWVER=")) {
            copy_text(ext.subspan(6), result.fw_version);
        }
    }
    if (!have_model) {
        return false;
    }
    result.model = Model::kZedF9p;
    out = result;
    return true;
}

bool rover_step(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) noexcept
{
    (void)model; // ZED-F9P 单型号，脚本与型号无关
    const uint8_t rate = cfg.rate_hz == 0 ? 1 : (cfg.rate_hz > 20 ? 20 : cfg.rate_hz);
    const uint16_t period_ms = static_cast<uint16_t>(1000U / rate);
    std::array<uint8_t, 64> payload{};

    size_t pos = 0;
    bool expect_reply = true;
    uint32_t switch_baud = 0;
    if (index == 0) {
        pos = begin_valset(payload.data());
        pos = add_u2_item(payload.data(), pos, kKeyRateMeas, period_ms);
        pos = add_u1_item(payload.data(), pos, kKeyUart1InprotRtcm3x, 1);
        pos = add_u1_item(payload.data(), pos, kKeyUart1OutprotNmea, 1);
    } else if (index == 1) {
        pos = begin_valset(payload.data());
        pos = add_u1_item(payload.data(), pos, kKeyMsgoutGgaUart1, 1);
        pos = add_u1_item(payload.data(), pos, kKeyMsgoutRmcUart1, 1);
        pos = add_u1_item(payload.data(), pos, kKeyMsgoutGsaUart1, 1);
        pos = add_u1_item(payload.data(), pos, kKeyMsgoutGstUart1, 1);
        pos = add_u1_item(payload.data(), pos, kKeyMsgoutGsvUart1, cfg.gsv ? 1 : 0);
    } else if (index == 2) {
        if (cfg.port_baud == 0) {
            return false; // 未要求改波特率，脚本结束
        }
        pos = begin_valset(payload.data());
        pos = add_u4_item(payload.data(), pos, kKeyUart1Baudrate, cfg.port_baud);
        expect_reply = false; // 改完波特率旧速率不再有应答，直接切换主机 UART
        switch_baud = cfg.port_baud;
    } else {
        return false;
    }

    ConfigStep step{};
    if (!build_frame(step.cmd, kClsCfg, kIdCfgValset, std::span<const uint8_t>(payload.data(), pos))) {
        return false;
    }
    step.expect_reply = expect_reply;
    step.timeout_ms = 500;
    step.switch_baud = switch_baud;
    out = step;
    return true;
}

Reply match_reply(const demux::Decoder &dec, std::span<const uint8_t> frame, std::span<const uint8_t>) noexcept
{
    if (&dec != &demux::ubx::kDecoder || frame.size() < 10) {
        return Reply::kNone;
    }
    if (frame[2] != kClsAck) {
        return Reply::kNone;
    }
    const uint8_t id = frame[3];
    if (id != kIdAck && id != kIdNak) {
        return Reply::kNone;
    }
    const size_t len = static_cast<size_t>(frame[4]) | (static_cast<size_t>(frame[5]) << 8);
    if (len != 2 || frame.size() != 8 + len) { // 6 字节头 + 载荷 + 2 字节校验
        return Reply::kNone;
    }
    // ACK/NAK 载荷是被应答帧的 class/id；只认 CFG-VALSET（06 8A）。
    if (frame[6] != kClsCfg || frame[7] != kIdCfgValset) {
        return Reply::kNone;
    }
    return id == kIdAck ? Reply::kAck : Reply::kNack;
}

uint32_t caps(Model model) noexcept
{
    if (model == Model::kZedF9p) {
        return cap::kRtcmIn | cap::kBase | cap::kRate20Hz;
    }
    return cap::kRtcmIn | cap::kBase;
}

// MON-VER 探测应答与 ACK/NAK 都是 UBX 帧（match_probe / match_reply 均按 ubx 解码器判定）。
constexpr std::array<const demux::Decoder *, 1> kDecoders{&demux::ubx::kDecoder};

const Dialect kDialect{"u-blox",     Family::kUblox, kDecoders,    &build_probe,
                       &match_probe, &rover_step,    &match_reply, &caps};

} // namespace gnss::ublox
