// u-blox F9 的 UBX 帧层：帧组装（同步字/长度/Fletcher 校验）、CFG-VALSET 载荷拼接、ACK/NAK 判定（ADR-0010 §2.2/§2.4）。
// 线程安全：是（纯函数，无全局可变状态，可重入）。ISR 安全：是（无锁、不阻塞）。
// 堆与阻塞：不使用堆（帧在 CmdBuf 定长缓冲内构造）、不阻塞；键 ID 出自 F9 HPG 1.32 的 CFG-* 键值表。
#include "gnss/drivers/UbloxReceiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "UbloxWire.hpp"

namespace gnss {
namespace {

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
        return u8(static_cast<uint8_t>(value & ublox::kByteMask)) &&
               u8(static_cast<uint8_t>((value >> ublox::kShift1Byte) & ublox::kByteMask));
    }

    /**
     * @brief 追加 4 字节（小端）。
     * @param v: 待写值。
     * @retval true: 写入成功。
     * @retval false: 空间不足。
     */
    bool u32(uint32_t v) noexcept
    {
        return u8(static_cast<uint8_t>(v & ublox::kByteMask)) &&
               u8(static_cast<uint8_t>((v >> ublox::kShift1Byte) & ublox::kByteMask)) &&
               u8(static_cast<uint8_t>((v >> ublox::kShift2Bytes) & ublox::kByteMask)) &&
               u8(static_cast<uint8_t>((v >> ublox::kShift3Bytes) & ublox::kByteMask));
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
    return writer.u8(ublox::kVersetVersion) && writer.u8(ublox::kLayerRamBbr) && writer.u8(0x00) &&
           (writer.pos == ublox::kValsetHeadBytes);
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
    return writer.u32(key) && writer.u8(value) && ((writer.pos - start) == ublox::kU1ItemBytes);
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
    return writer.u32(key) && writer.u16(value) && ((writer.pos - start) == ublox::kU2ItemBytes);
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
    return writer.u32(key) && writer.u32(value) && ((writer.pos - start) == ublox::kU4ItemBytes);
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
    if (payload.size() > ublox::kMaxPayload) {
        return false;
    }
    const std::array<uint8_t, ublox::kHeaderBytes> header{
        ublox::kSync1,
        ublox::kSync2,
        cls,
        id,
        static_cast<uint8_t>(payload.size() & ublox::kByteMask),
        static_cast<uint8_t>((payload.size() >> ublox::kShift1Byte) & ublox::kByteMask)};
    uint8_t ckA = 0;
    uint8_t ckB = 0;
    for (size_t i = ublox::kSyncBytes; i < header.size(); ++i) {
        fletcher(header[i], ckA, ckB);
    }
    for (const uint8_t b : payload) {
        fletcher(b, ckA, ckB);
    }
    const std::array<uint8_t, ublox::kCrcBytes> checksum{ckA, ckB};
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
    std::array<uint8_t, ublox::kValsetPayloadMax> payload{};
    Writer writer{payload};
    if (!beginValset(writer) || !addU2Item(writer, ublox::kKeyRateMeas, periodMs) ||
        !addU1Item(writer, ublox::kKeyUart1InprotRtcm3x, 1U) ||
        !addU1Item(writer, ublox::kKeyUart1OutprotNmea, 1U)) {
        return false;
    }
    return cmdBuildFrame(out, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
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
    Writer writer{payload};
    if (!beginValset(writer) || !addU4Item(writer, ublox::kKeyUart1Baudrate, baud)) {
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
    std::array<uint8_t, ublox::kMsgoutPayloadMax> payload{};
    Writer writer{payload};
    if (!beginValset(writer) || !addU1Item(writer, key, epochs)) {
        return false;
    }
    return cmdBuildFrame(out, ublox::kClsCfg, ublox::kIdCfgValset, writer.view());
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

} // namespace gnss
