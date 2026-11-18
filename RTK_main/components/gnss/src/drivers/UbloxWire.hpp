// u-blox F9 驱动的 UBX 线格式常量（组件私有头）：帧结构偏移、CFG-VALSET 层与键 ID、MON-VER 布局。
// 线程/ISR：只含 constexpr 常量，无状态，任何 TU 均可包含。
// 堆：不使用。浮点：不使用。
#pragma once

#include <cstddef>
#include <cstdint>

namespace gnss::ublox {

// UBX 帧头与消息 ID
inline constexpr uint8_t kSync1 = 0xB5;
inline constexpr uint8_t kSync2 = 0x62;
inline constexpr uint8_t kClsCfg = 0x06;
inline constexpr uint8_t kIdCfgCfg = 0x09;
inline constexpr uint8_t kIdCfgValset = 0x8A;
inline constexpr uint8_t kClsAck = 0x05;
inline constexpr uint8_t kIdAck = 0x01;
inline constexpr uint8_t kIdNak = 0x00;
inline constexpr uint8_t kClsMon = 0x0A;
inline constexpr uint8_t kIdMonVer = 0x04;
// 配置层：RAM | BBR（ADR-0010 §2.4 不写模块 flash）
inline constexpr uint8_t kLayerRamBbr = 0x03;
inline constexpr uint8_t kLayerRam = 0x01U;
inline constexpr uint8_t kVersetVersion = 0x00;
inline constexpr uint8_t kByteMask = 0xFFU;
// 小端拼接的移位位数（每字节 8 位）。
inline constexpr uint8_t kShift1Byte = 8U;
inline constexpr uint8_t kShift2Bytes = 16U;
inline constexpr uint8_t kShift3Bytes = 24U;
inline constexpr size_t kHeaderBytes = 6U;
inline constexpr size_t kCrcBytes = 2U;
inline constexpr size_t kAckPayloadBytes = 2U;
inline constexpr size_t kSyncBytes = 2U;
inline constexpr size_t kLenOffset = 4U;
inline constexpr size_t kIdOffset = 3U;
inline constexpr size_t kClsOffset = 2U;
inline constexpr size_t kPayloadOffset = 6U;
inline constexpr size_t kMaxPayload = 0xFFFFU;
inline constexpr size_t kValsetHeadBytes = 3U;
inline constexpr size_t kValsetPayloadMax = 64U;
inline constexpr size_t kMsgoutPayloadMax = 16U;
inline constexpr size_t kU1ItemBytes = 5U;
inline constexpr size_t kU2ItemBytes = 6U;
inline constexpr size_t kU4ItemBytes = 8U;
inline constexpr uint16_t kMonVerMinLen = 40U;
inline constexpr size_t kMonVerExtBytes = 30U;
inline constexpr size_t kConfigTimeoutMs = 500U;
inline constexpr uint16_t kDefaultPeriodMs = 1000U;
inline constexpr uint8_t kMaxRateHz = 20U;
inline constexpr uint8_t kMinRateHz = 1U;
inline constexpr uint32_t kMaxEpochsPerEpoch = 255U;
inline constexpr uint32_t kMsPerSecond = 1000U;
inline constexpr uint32_t kHalfSecondMs = kMsPerSecond / 2U;

// 键 ID（见文件头出处）
inline constexpr uint32_t kKeyRateMeas = 0x30210001U;          // CFG-RATE-MEAS, U2
inline constexpr uint32_t kKeyUart1InprotRtcm3x = 0x10730004U; // CFG-UART1INPROT-RTCM3X, L
inline constexpr uint32_t kKeyUart1OutprotNmea = 0x10740002U;  // CFG-UART1OUTPROT-NMEA, L
inline constexpr uint32_t kKeyMsgoutGgaUart1 = 0x209100BBU;    // CFG-MSGOUT-NMEA_ID_GGA_UART1, U1
inline constexpr uint32_t kKeyMsgoutRmcUart1 = 0x209100ACU;    // CFG-MSGOUT-NMEA_ID_RMC_UART1, U1
inline constexpr uint32_t kKeyMsgoutGsaUart1 = 0x209100BFU;    // CFG-MSGOUT-NMEA_ID_GSA_UART1, U1
inline constexpr uint32_t kKeyMsgoutGsvUart1 = 0x209100C3U;    // CFG-MSGOUT-NMEA_ID_GSV_UART1, U1
inline constexpr uint32_t kKeyMsgoutGstUart1 = 0x209100D4U;    // CFG-MSGOUT-NMEA_ID_GST_UART1, U1
inline constexpr uint32_t kKeyUart1Baudrate = 0x40520001U;     // CFG-UART1-BAUDRATE, U4

/** MON-VER 轮询帧（空载荷）已算好的 Fletcher 校验：载荷为空，校验只覆盖 class/id/len。 */
inline constexpr uint8_t kMonVerPollCkA = 0x0E;
inline constexpr uint8_t kMonVerPollCkB = 0x34;
inline constexpr size_t kMonVerPollBytes = 8U;

} // namespace gnss::ublox
