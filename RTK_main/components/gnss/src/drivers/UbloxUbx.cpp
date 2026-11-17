// u-blox F9 的 UBX 帧层：帧组装（同步字/长度/Fletcher 校验）、CFG-VALSET 载荷拼接、ACK/NAK 判定。
// 结构：命令生成（cmd_*）与应答判定（resp_*）都是 UbloxReceiver 的 protected static，子类可直接复用。
// 线程/调用约束：纯函数，无全局可变状态，可重入、不阻塞。
// 堆：不使用（帧在 CmdBuf 定长缓冲内构造）。
// 键 ID 出处：u-blox F9 HPG 1.32 Interface Description（CFG-* 键值表）。
#include "gnss/drivers/UbloxReceiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "UbloxWire.hpp"

namespace gnss {
namespace {

using ublox::kAckPayloadBytes;
using ublox::kByteMask;
using ublox::kClsAck;
using ublox::kClsCfg;
using ublox::kClsOffset;
using ublox::kCrcBytes;
using ublox::kHeaderBytes;
using ublox::kIdAck;
using ublox::kIdCfgValset;
using ublox::kIdNak;
using ublox::kIdOffset;
using ublox::kKeyRateMeas;
using ublox::kKeyUart1Baudrate;
using ublox::kKeyUart1InprotRtcm3x;
using ublox::kKeyUart1OutprotNmea;
using ublox::kLayerRamBbr;
using ublox::kLenOffset;
using ublox::kMaxPayload;
using ublox::kMsgoutPayloadMax;
using ublox::kPayloadOffset;
using ublox::kShift1Byte;
using ublox::kShift2Bytes;
using ublox::kShift3Bytes;
using ublox::kSync1;
using ublox::kSync2;
using ublox::kSyncBytes;
using ublox::kU1ItemBytes;
using ublox::kU2ItemBytes;
using ublox::kU4ItemBytes;
using ublox::kValsetHeadBytes;
using ublox::kValsetPayloadMax;
using ublox::kVersetVersion;

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

    /**
     * @brief 已写入的字节视图。
     * @retval view: 指向内部缓冲前 pos 字节的只读视图（缓冲失效前有效）。
     */
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

} // namespace gnss
