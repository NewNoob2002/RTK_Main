// u-blox F9P 共享 UBX/VALSET 编码器、固件能力判定与在途 VALSET 跟踪实现。
// 线程安全：工具函数纯函数可重入，ValsetTracker 由单任务独占。ISR 安全：纯函数是，跟踪器否；均零堆不阻塞。
#include "UbloxExt.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "drivers/UbloxWire.hpp"

namespace gnss::ublox {
namespace {

constexpr size_t kHexDigitsPerByte = 2U;
constexpr size_t kHexCharsPerKey = 10U; // "0x12345678"
constexpr uint8_t kNibbleShift = 4U;
constexpr uint8_t kNibbleMask = 0x0FU;
constexpr std::string_view kHexDigits = "0123456789ABCDEF";
constexpr uint8_t kMinPort = 1U;
constexpr uint8_t kMaxPort = 2U;
constexpr size_t kReservedOffset1 = 2U;
constexpr size_t kReservedOffset2 = 3U;
constexpr size_t kHexByte2Offset = 2U;
constexpr size_t kHexByte3Offset = 4U;
constexpr size_t kHexByte4Offset = 6U;
constexpr uint16_t kVersionMajorScale = 100U;
constexpr uint16_t kDecimalBase = 10U;
constexpr uint16_t kMaxHpgMajor = 99U;
constexpr size_t kHpgMinorDigits = 2U;

/**
 * @brief 比较单调时间是否已到达截止点（有符号差值支持 uint32 回绕）。
 * @param nowMs: 当前单调时钟（ms）。
 * @param deadlineMs: 截止时钟（ms）。
 * @retval true 已到达或越过截止点；false 未到达。
 */
bool timeReached(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

/**
 * @brief 截断视图中首个 NUL 及之后的尾字节。
 * @param text: 原始文本视图。
 * @retval 去除尾 NUL 后的有效前缀。
 */
std::string_view trimAtNul(std::string_view text) noexcept
{
    const size_t nulPos = text.find('\0');
    return (nulPos == std::string_view::npos) ? text : text.substr(0U, nulPos);
}

/**
 * @brief 按 UBX 配置键 bit[30:28] 尺寸编码返回对应值字节数（UBX-22008968 §6.9.1）。
 * @param key: 32 位 UBX 配置键 ID。
 * @retval 值字节数（1/2/4/8）；未知编码返回 0。
 */
size_t keyValueBytes(uint32_t key) noexcept
{
    const uint32_t code = (key >> kKeySizeShift) & kKeySizeMask;
    switch (code) {
    case kKeySizeOneBit:
    case kKeySizeOneByte:
        return 1U;
    case kKeySizeTwoBytes:
        return 2U;
    case kKeySizeFourBytes:
        return 4U;
    case kKeySizeEightBytes:
        return 8U;
    default:
        return 0U;
    }
}

/**
 * @brief 读取 UBX 帧头里的小端 16 位载荷长度。
 * @param frame: 长度至少 kHeaderBytes 的帧视图。
 * @retval 载荷字节数。
 */
size_t readPayloadLen(std::span<const uint8_t> frame) noexcept
{
    return static_cast<size_t>(frame[kLenOffset]) |
           (static_cast<size_t>(frame[kLenOffset + 1U]) << kShift1Byte);
}

/**
 * @brief 将单字节写入两位大写十六进制字符。
 * @param byte: 输入字节。
 * @param[out] out: 目标字符缓冲。
 * @param pos: 起始下标（调用方保证余量 >= 2）。
 */
void writeHexByte(uint8_t byte, std::span<char> out, size_t pos) noexcept
{
    const auto high =
        static_cast<size_t>((static_cast<uint32_t>(byte) >> kNibbleShift) & kNibbleMask);
    const auto low = static_cast<size_t>(static_cast<uint32_t>(byte) & kNibbleMask);
    out[pos] = kHexDigits[high];
    out[pos + 1U] = kHexDigits[low];
}

/**
 * @brief 判断帧是否为针对 VALSET 的 ACK-ACK 或 ACK-NAK。
 * @param frame: 完整 UBX 帧字节。
 * @param expectedId: kIdAck 或 kIdNak。
 * @retval true 匹配且校验和有效；false 否。
 */
bool isValsetReply(std::span<const uint8_t> frame, uint8_t expectedId) noexcept
{
    if ((frame.size() != (kHeaderBytes + kAckPayloadBytes + kCrcBytes)) ||
        !validUbxChecksum(frame)) {
        return false;
    }
    return (frame[kClsOffset] == kClsAck) && (frame[kIdOffset] == expectedId) &&
           (frame[kPayloadOffset] == kClsCfg) && (frame[kPayloadOffset + 1U] == kIdCfgValset);
}

} // namespace

bool ValsetWriter::u8(uint8_t value) noexcept
{
    if (pos >= buf.size()) {
        return false;
    }
    buf[pos] = value;
    ++pos;
    return true;
}

bool ValsetWriter::u16(uint16_t value) noexcept
{
    const auto wide = static_cast<uint32_t>(value);
    return u8(static_cast<uint8_t>(wide & kByteMask)) &&
           u8(static_cast<uint8_t>((wide >> kShift1Byte) & kByteMask));
}

bool ValsetWriter::u32(uint32_t value) noexcept
{
    return u8(static_cast<uint8_t>(value & kByteMask)) &&
           u8(static_cast<uint8_t>((value >> kShift1Byte) & kByteMask)) &&
           u8(static_cast<uint8_t>((value >> kShift2Bytes) & kByteMask)) &&
           u8(static_cast<uint8_t>((value >> kShift3Bytes) & kByteMask));
}

bool ValsetWriter::begin(uint8_t layers) noexcept
{
    return (pos == 0U) && u8(kVersetVersion) && u8(layers) && u16(0U) && (pos == kValsetHeadBytes);
}

bool ValsetWriter::addU1(uint32_t key, uint8_t value) noexcept
{
    const size_t start = pos;
    return u32(key) && u8(value) && ((pos - start) == kU1ItemBytes);
}

bool ValsetWriter::addU2(uint32_t key, uint16_t value) noexcept
{
    const size_t start = pos;
    return u32(key) && u16(value) && ((pos - start) == kU2ItemBytes);
}

bool ValsetWriter::addU4(uint32_t key, uint32_t value) noexcept
{
    const size_t start = pos;
    return u32(key) && u32(value) && ((pos - start) == kU4ItemBytes);
}

bool ValsetWriter::addI1(uint32_t key, int8_t value) noexcept
{
    return addU1(key, static_cast<uint8_t>(value));
}

bool ValsetWriter::addI4(uint32_t key, int32_t value) noexcept
{
    return addU4(key, static_cast<uint32_t>(value));
}

void stepFletcher(uint8_t byte, uint8_t &ckA, uint8_t &ckB) noexcept
{
    ckA = static_cast<uint8_t>(ckA + byte);
    ckB = static_cast<uint8_t>(ckB + ckA);
}

bool encodeUbxFrame(CmdBuf &out, uint8_t cls, uint8_t id, std::span<const uint8_t> payload) noexcept
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
    uint8_t ckA = 0U;
    uint8_t ckB = 0U;
    for (size_t idx = kSyncBytes; idx < header.size(); ++idx) {
        stepFletcher(header[idx], ckA, ckB);
    }
    for (const uint8_t byte : payload) {
        stepFletcher(byte, ckA, ckB);
    }
    const std::array<uint8_t, kCrcBytes> checksum{ckA, ckB};
    out.clear();
    return out.append(header) && out.append(payload) && out.append(checksum);
}

bool validUbxChecksum(std::span<const uint8_t> frame) noexcept
{
    if (frame.size() < (kHeaderBytes + kCrcBytes)) {
        return false;
    }
    if ((frame[0] != kSync1) || (frame[1] != kSync2)) {
        return false;
    }
    const size_t payloadLen = readPayloadLen(frame);
    if (frame.size() != (kHeaderBytes + payloadLen + kCrcBytes)) {
        return false;
    }
    uint8_t ckA = 0U;
    uint8_t ckB = 0U;
    const size_t ckEnd = frame.size() - kCrcBytes;
    for (size_t i = kSyncBytes; i < ckEnd; ++i) {
        stepFletcher(frame[i], ckA, ckB);
    }
    return (frame[ckEnd] == ckA) && (frame[ckEnd + 1U] == ckB);
}

bool buildSilenceNmeaValset(CmdBuf &out, uint8_t layer, uint8_t port) noexcept
{
    if ((port < kMinPort) || (port > kMaxPort)) {
        return false;
    }
    std::array<uint8_t, kValsetPayloadMax> payload{};
    ValsetWriter writer{payload};
    if (!writer.begin(layer)) {
        return false;
    }
    const uint32_t portOffset = (port == kMaxPort) ? 1U : 0U;
    for (const uint32_t baseKey : kNmeaSilenceUart1Keys) {
        if (!writer.addU1(baseKey + portOffset, 0U)) {
            return false;
        }
    }
    return encodeUbxFrame(out, kClsCfg, kIdCfgValset, writer.view());
}

bool isSilenceNmeaValset(std::span<const uint8_t> frame) noexcept
{
    std::array<uint32_t, kMaxExtractedValsetKeys> keys{};
    const size_t count = extractValsetKeys(frame, keys);
    if (count != kNmeaSilenceUart1Keys.size()) {
        return false;
    }
    const uint32_t firstKey = keys[0U];
    uint32_t portOffset = 0U;
    if (firstKey == (kNmeaSilenceUart1Keys[0U] + 1U)) {
        portOffset = 1U;
    } else if (firstKey != kNmeaSilenceUart1Keys[0U]) {
        return false;
    }
    for (size_t idx = 0U; idx < kNmeaSilenceUart1Keys.size(); ++idx) {
        if (keys[idx] != (kNmeaSilenceUart1Keys[idx] + portOffset)) {
            return false;
        }
    }
    return true;
}

bool parseF9pHpgVersion(std::string_view fwVersion, uint16_t &versionOut) noexcept
{
    const std::string_view clean = trimAtNul(fwVersion);
    constexpr std::string_view kPrefix = "HPG ";
    if (!clean.starts_with(kPrefix)) {
        return false;
    }
    std::string_view rest = clean.substr(kPrefix.size());
    uint16_t major = 0U;
    size_t majorDigits = 0U;
    while ((majorDigits < rest.size()) && (rest[majorDigits] >= '0') &&
           (rest[majorDigits] <= '9')) {
        major = static_cast<uint16_t>((major * kDecimalBase) +
                                      static_cast<uint16_t>(rest[majorDigits] - '0'));
        if (major > kMaxHpgMajor) {
            return false;
        }
        ++majorDigits;
    }
    if ((majorDigits == 0U) || (majorDigits >= rest.size()) || (rest[majorDigits] != '.')) {
        return false;
    }
    rest.remove_prefix(majorDigits + 1U);
    if (rest.size() < kHpgMinorDigits) {
        return false;
    }
    uint16_t minor = 0U;
    for (size_t idx = 0U; idx < kHpgMinorDigits; ++idx) {
        if ((rest[idx] < '0') || (rest[idx] > '9')) {
            return false;
        }
        minor =
            static_cast<uint16_t>((minor * kDecimalBase) + static_cast<uint16_t>(rest[idx] - '0'));
    }
    if ((rest.size() > kHpgMinorDigits) && (rest[kHpgMinorDigits] >= '0') &&
        (rest[kHpgMinorDigits] <= '9')) {
        return false;
    }
    versionOut = static_cast<uint16_t>((major * kVersionMajorScale) + minor);
    return true;
}

bool f9pSupportsSbas(std::string_view fwVersion) noexcept
{
    uint16_t version = 0U;
    return parseF9pHpgVersion(fwVersion, version) && (version >= kF9pSbasMinVersion);
}

bool buildSatTrackValset(CmdBuf &out, const SatMaskConfig &cfg, bool includeSbas) noexcept
{
    if ((cfg.cutoffDeg > SatMaskConfig::kMaxCutoffDeg) || !cfg.gps) {
        return false;
    }
    std::array<uint8_t, kValsetPayloadMax> payload{};
    ValsetWriter writer{payload};
    // u-blox F9P 原子 VALSET：同帧写入 MINELEV + 全部 CFG-SIGNAL 键；GPS 与 GPS L1C/A 强制启用，
    // QZSS L1C/A 随 QZSS 显式写入（SparkFun GNSS_ZED.h:35, GNSS_ZED.ino:2024-2028），SBAS 按固件版本门控。
    bool ok = writer.begin(kLayerRamBbr) &&
              writer.addI1(kKeyNavspgInfilMinelev, static_cast<int8_t>(cfg.cutoffDeg)) &&
              writer.addU1(kKeySignalGpsEna, 1U) && writer.addU1(kKeySignalGpsL1caEna, 1U) &&
              writer.addU1(kKeySignalQzssEna, 1U) && writer.addU1(kKeySignalQzssL1caEna, 1U) &&
              writer.addU1(kKeySignalBdsEna, cfg.bds ? 1U : 0U) &&
              writer.addU1(kKeySignalGloEna, cfg.glo ? 1U : 0U) &&
              writer.addU1(kKeySignalGalEna, cfg.gal ? 1U : 0U);
    if (ok && includeSbas) {
        ok = writer.addU1(kKeySignalSbasEna, cfg.sbas ? 1U : 0U);
    }
    if (!ok) {
        return false;
    }
    return encodeUbxFrame(out, kClsCfg, kIdCfgValset, writer.view());
}

bool buildPollSecUniqId(CmdBuf &out) noexcept
{
    out.clear();
    return out.append(std::array<uint8_t, kSecUniqIdPollBytes>{
        kSync1, kSync2, kClsSec, kIdSecUniqId, 0x00U, 0x00U, kSecUniqIdPollCkA, kSecUniqIdPollCkB});
}

size_t parseSecUniqId(std::span<const uint8_t> frame, std::span<char> out) noexcept
{
    if (!out.empty()) {
        out[0] = '\0';
    }
    if ((out.size() < (kSecUniqIdHexChars + 1U)) || !validUbxChecksum(frame)) {
        return 0U;
    }
    if ((frame[kClsOffset] != kClsSec) || (frame[kIdOffset] != kIdSecUniqId)) {
        return 0U;
    }
    if (readPayloadLen(frame) != kSecUniqIdPayloadBytes) {
        return 0U;
    }
    if (frame[kPayloadOffset] != kSecUniqIdVersion) {
        return 0U;
    }
    for (size_t i = 0U; i < kSecUniqIdIdBytes; ++i) {
        writeHexByte(frame[kPayloadOffset + kSecUniqIdIdOffset + i], out, i * kHexDigitsPerByte);
    }
    out[kSecUniqIdHexChars] = '\0';
    return kSecUniqIdHexChars;
}

bool isValsetFrame(std::span<const uint8_t> frame) noexcept
{
    if (!validUbxChecksum(frame)) {
        return false;
    }
    if ((frame[kClsOffset] != kClsCfg) || (frame[kIdOffset] != kIdCfgValset)) {
        return false;
    }
    const size_t payloadLen = readPayloadLen(frame);
    if (payloadLen < kValsetHeadBytes) {
        return false;
    }
    return (frame[kPayloadOffset] == kVersetVersion) &&
           (frame[kPayloadOffset + kReservedOffset1] == 0U) &&
           (frame[kPayloadOffset + kReservedOffset2] == 0U);
}

size_t extractValsetKeys(std::span<const uint8_t> frame, std::span<uint32_t> keysOut) noexcept
{
    if (!isValsetFrame(frame) || keysOut.empty()) {
        return 0U;
    }
    const size_t end = frame.size() - kCrcBytes;
    size_t pos = kPayloadOffset + kValsetHeadBytes;
    size_t count = 0U;
    while (pos < end) {
        if ((pos + kKeyIdBytes) > end) {
            return 0U;
        }
        const uint32_t key =
            static_cast<uint32_t>(frame[pos]) |
            (static_cast<uint32_t>(frame[pos + 1U]) << kShift1Byte) |
            (static_cast<uint32_t>(frame[pos + kReservedOffset1]) << kShift2Bytes) |
            (static_cast<uint32_t>(frame[pos + kReservedOffset2]) << kShift3Bytes);
        const size_t valueLen = keyValueBytes(key);
        if ((valueLen == 0U) || ((pos + kKeyIdBytes + valueLen) > end)) {
            return 0U;
        }
        if (count < keysOut.size()) {
            keysOut[count] = key;
            ++count;
        }
        pos += kKeyIdBytes + valueLen;
    }
    return count;
}

size_t formatValsetKeys(std::span<const uint32_t> keys, std::span<char> out) noexcept
{
    if (out.empty()) {
        return 0U;
    }
    if (keys.empty()) {
        constexpr std::string_view kNone = "none";
        const size_t copy = (kNone.size() < (out.size() - 1U)) ? kNone.size() : (out.size() - 1U);
        for (size_t i = 0U; i < copy; ++i) {
            out[i] = kNone[i];
        }
        out[copy] = '\0';
        return copy;
    }
    size_t pos = 0U;
    for (size_t idx = 0U; idx < keys.size(); ++idx) {
        const size_t need = (idx == 0U ? 0U : 1U) + kHexCharsPerKey;
        if ((pos + need + 1U) > out.size()) {
            break;
        }
        if (idx > 0U) {
            out[pos] = ',';
            ++pos;
        }
        out[pos] = '0';
        out[pos + 1U] = 'x';
        pos += kHexDigitsPerByte;
        const uint32_t key = keys[idx];
        writeHexByte(static_cast<uint8_t>((key >> kShift3Bytes) & kByteMask), out, pos);
        writeHexByte(static_cast<uint8_t>((key >> kShift2Bytes) & kByteMask), out,
                     pos + kHexByte2Offset);
        writeHexByte(static_cast<uint8_t>((key >> kShift1Byte) & kByteMask), out,
                     pos + kHexByte3Offset);
        writeHexByte(static_cast<uint8_t>(key & kByteMask), out, pos + kHexByte4Offset);
        pos += 8U;
    }
    out[pos] = '\0';
    return pos;
}

bool isValsetAck(std::span<const uint8_t> frame) noexcept
{
    return isValsetReply(frame, kIdAck);
}

bool isValsetNak(std::span<const uint8_t> frame) noexcept
{
    return isValsetReply(frame, kIdNak);
}

void ValsetTracker::reset() noexcept
{
    key_count_ = 0U;
    in_flight_ = false;
    resyncing_ = false;
    deadline_ms_ = 0U;
    resync_until_ms_ = 0U;
}

void ValsetTracker::onTx(std::span<const uint8_t> frame, uint32_t nowMs, uint32_t timeoutMs,
                         bool expectReply) noexcept
{
    if (!isValsetFrame(frame)) {
        return;
    }
    if (!expectReply) {
        in_flight_ = false;
        key_count_ = 0U;
        return;
    }
    key_count_ = extractValsetKeys(frame, keys_);
    in_flight_ = true;
    resyncing_ = false;
    deadline_ms_ = nowMs + timeoutMs;
}

void ValsetTracker::enterResync(uint32_t nowMs) noexcept
{
    in_flight_ = false;
    resyncing_ = true;
    resync_until_ms_ = nowMs + kValsetResyncWindowMs;
}

/**
 * @brief 查询是否仍在 300 ms 重同步窗口内。
 * @param nowMs: 当前单调时钟（ms）。
 * @retval true 仍在重同步窗口内；false 已结束或未处于重同步。
 */
bool ValsetTracker::resyncing(uint32_t nowMs) const noexcept
{
    return resyncing_ && !timeReached(nowMs, resync_until_ms_);
}

bool ValsetTracker::pollTimeout(uint32_t nowMs, std::span<const uint32_t> &keysOut) noexcept
{
    keysOut = {};
    if (resyncing_ && timeReached(nowMs, resync_until_ms_)) {
        resyncing_ = false;
        key_count_ = 0U;
    }
    if (in_flight_ && timeReached(nowMs, deadline_ms_)) {
        keysOut = std::span<const uint32_t>{keys_.data(), key_count_};
        enterResync(nowMs);
        return true;
    }
    return false;
}

/**
 * @brief 处理收到的一帧 UBX，判定是否匹配当前在途 VALSET。
 * @param frame: 已校验的完整 UBX 帧。
 * @param nowMs: 当前单调时钟（ms）。
 * @param[out] keysOut: 返回 kAck/kNak 时指向被匹配的在途键表，其余情况为空。
 * @retval kNone 非 VALSET ACK/NAK；kAck/kNak 匹配在途命令；kUnmatched 空闲或重同步期迟到/重复应答。
 */
ValsetReplyKind ValsetTracker::onFrame(std::span<const uint8_t> frame, uint32_t nowMs,
                                       std::span<const uint32_t> &keysOut) noexcept
{
    keysOut = {};
    if (resyncing_ && timeReached(nowMs, resync_until_ms_)) {
        resyncing_ = false;
        key_count_ = 0U;
    }
    const bool isAck = isValsetAck(frame);
    const bool isNak = !isAck && isValsetNak(frame);
    if (!isAck && !isNak) {
        return ValsetReplyKind::kNone;
    }
    if (resyncing_ || !in_flight_) {
        return ValsetReplyKind::kUnmatched;
    }
    in_flight_ = false;
    keysOut = std::span<const uint32_t>{keys_.data(), key_count_};
    return isNak ? ValsetReplyKind::kNak : ValsetReplyKind::kAck;
}

} // namespace gnss::ublox
