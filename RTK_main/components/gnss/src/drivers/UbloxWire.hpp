// u-blox F9 驱动的 UBX 线格式常量（组件私有头）：帧结构偏移、CFG-VALSET 层与键 ID、MON-VER 布局。
// 线程/ISR：只含 constexpr 常量，无状态，任何 TU 均可包含。
// 堆：不使用。浮点：不使用。
#pragma once

#include <array>
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
inline constexpr uint8_t kClsSec = 0x27;
inline constexpr uint8_t kIdSecUniqId = 0x03;
// 配置层：RAM | BBR（ADR-0010 §2.4 不写模块 flash）
inline constexpr uint8_t kLayerRamBbr = 0x03;
inline constexpr uint8_t kLayerRam = 0x01U;
inline constexpr uint8_t kVersetVersion = 0x00;
inline constexpr uint8_t kByteMask = 0xFFU;
// 小端拼接的移位位数（每字节 8 位）。
inline constexpr uint8_t kShift1Byte = 8U;
inline constexpr uint8_t kShift2Bytes = 16U;
inline constexpr uint8_t kShift3Bytes = 24U;
inline constexpr uint8_t kKeySizeShift = 28U;
inline constexpr uint8_t kKeySizeMask = 0x07U;
inline constexpr uint8_t kKeySizeOneBit = 1U;
inline constexpr uint8_t kKeySizeOneByte = 2U;
inline constexpr uint8_t kKeySizeTwoBytes = 3U;
inline constexpr uint8_t kKeySizeFourBytes = 4U;
inline constexpr uint8_t kKeySizeEightBytes = 5U;
inline constexpr size_t kHeaderBytes = 6U;
inline constexpr size_t kCrcBytes = 2U;
inline constexpr size_t kAckPayloadBytes = 2U;
inline constexpr size_t kSyncBytes = 2U;
inline constexpr size_t kLenOffset = 4U;
inline constexpr size_t kIdOffset = 3U;
inline constexpr size_t kClsOffset = 2U;
inline constexpr size_t kPayloadOffset = 6U;
inline constexpr size_t kMaxPayload = 0xFFFFU;
inline constexpr size_t kValsetHeadBytes = 4U;
inline constexpr size_t kValsetPayloadMax = 80U;
inline constexpr size_t kMsgoutPayloadMax = 16U;
inline constexpr size_t kU1ItemBytes = 5U;
inline constexpr size_t kU2ItemBytes = 6U;
inline constexpr size_t kU4ItemBytes = 8U;
inline constexpr size_t kKeyIdBytes = 4U;
inline constexpr size_t kMonVerSwBytes = 30U;
inline constexpr uint16_t kMonVerMinLen = 40U;
inline constexpr size_t kMonVerExtBytes = 30U;
inline constexpr uint8_t kSecUniqIdVersion = 0x01U;
inline constexpr size_t kSecUniqIdPayloadBytes = 9U;
inline constexpr size_t kSecUniqIdIdOffset = 4U;
inline constexpr size_t kSecUniqIdIdBytes = 5U;
inline constexpr size_t kConfigTimeoutMs = 500U;
inline constexpr uint16_t kF9pSbasMinVersion = 113U; // SparkFun GNSS_ZED.h:36 (HPG 1.13+)
inline constexpr uint16_t kDefaultPeriodMs = 1000U;
inline constexpr uint8_t kMaxRateHz = 20U;
inline constexpr uint8_t kMinRateHz = 1U;
inline constexpr uint32_t kMaxEpochsPerEpoch = 255U;
inline constexpr uint32_t kMsPerSecond = 1000U;
inline constexpr uint32_t kHalfSecondMs = kMsPerSecond / 2U;

// 键 ID（UBX-22008968 F9 HPG 1.32 Interface Description §6.9）
inline constexpr uint32_t kKeyRateMeas = 0x30210001U;           // CFG-RATE-MEAS, U2
inline constexpr uint32_t kKeyUart1InprotRtcm3x = 0x10730004U;  // CFG-UART1INPROT-RTCM3X, L
inline constexpr uint32_t kKeyUart1OutprotNmea = 0x10740002U;   // CFG-UART1OUTPROT-NMEA, L
inline constexpr uint32_t kKeyMsgoutDtmUart1 = 0x209100A7U;     // CFG-MSGOUT-NMEA_ID_DTM_UART1, U1
inline constexpr uint32_t kKeyMsgoutRmcUart1 = 0x209100ACU;     // CFG-MSGOUT-NMEA_ID_RMC_UART1, U1
inline constexpr uint32_t kKeyMsgoutVtgUart1 = 0x209100B1U;     // CFG-MSGOUT-NMEA_ID_VTG_UART1, U1
inline constexpr uint32_t kKeyMsgoutGnsUart1 = 0x209100B6U;     // CFG-MSGOUT-NMEA_ID_GNS_UART1, U1
inline constexpr uint32_t kKeyMsgoutGgaUart1 = 0x209100BBU;     // CFG-MSGOUT-NMEA_ID_GGA_UART1, U1
inline constexpr uint32_t kKeyMsgoutGsaUart1 = 0x209100C0U;     // CFG-MSGOUT-NMEA_ID_GSA_UART1, U1
inline constexpr uint32_t kKeyMsgoutGsvUart1 = 0x209100C5U;     // CFG-MSGOUT-NMEA_ID_GSV_UART1, U1
inline constexpr uint32_t kKeyMsgoutGllUart1 = 0x209100CAU;     // CFG-MSGOUT-NMEA_ID_GLL_UART1, U1
inline constexpr uint32_t kKeyMsgoutGrsUart1 = 0x209100CFU;     // CFG-MSGOUT-NMEA_ID_GRS_UART1, U1
inline constexpr uint32_t kKeyMsgoutGstUart1 = 0x209100D4U;     // CFG-MSGOUT-NMEA_ID_GST_UART1, U1
inline constexpr uint32_t kKeyMsgoutZdaUart1 = 0x209100D9U;     // CFG-MSGOUT-NMEA_ID_ZDA_UART1, U1
inline constexpr uint32_t kKeyMsgoutGbsUart1 = 0x209100DEU;     // CFG-MSGOUT-NMEA_ID_GBS_UART1, U1
inline constexpr uint32_t kKeyUart1Baudrate = 0x40520001U;      // CFG-UART1-BAUDRATE, U4
inline constexpr uint32_t kKeyUart1OutprotRtcm3x = 0x10740004U; // CFG-UART1OUTPROT-RTCM3X, L
inline constexpr uint32_t kKeyUart2OutprotRtcm3x = 0x10760004U; // CFG-UART2OUTPROT-RTCM3X, L

// 流动站启动静默的 12 条标准 NMEA 语句（HPG 1.12+ 全部支持；UART2 = UART1 + 1，UBX-22008968 §6.9.14）
inline constexpr std::array<uint32_t, 12U> kNmeaSilenceUart1Keys{
    kKeyMsgoutGgaUart1, kKeyMsgoutGllUart1, kKeyMsgoutGsaUart1, kKeyMsgoutGsvUart1,
    kKeyMsgoutRmcUart1, kKeyMsgoutVtgUart1, kKeyMsgoutGstUart1, kKeyMsgoutZdaUart1,
    kKeyMsgoutGnsUart1, kKeyMsgoutDtmUart1, kKeyMsgoutGrsUart1, kKeyMsgoutGbsUart1,
};

// 星座与截止角键 ID（UBX-22008968 §6.9.16 CFG-NAVSPG、§6.9.22 CFG-SIGNAL；
// 信号键与固件版本表交叉核对：SparkFun GNSS_ZED.h:28-37, GNSS_ZED.ino:2012-2031）
inline constexpr uint32_t kKeyNavspgInfilMinelev =
    0x201100A4U;                                               // CFG-NAVSPG-INFIL_MINELEV, I1 (deg)
inline constexpr uint32_t kKeySignalGpsL1caEna = 0x10310001U;  // CFG-SIGNAL-GPS_L1CA_ENA, L
inline constexpr uint32_t kKeySignalQzssL1caEna = 0x10310012U; // CFG-SIGNAL-QZSS_L1CA_ENA, L
inline constexpr uint32_t kKeySignalGpsEna = 0x1031001FU;      // CFG-SIGNAL-GPS_ENA, L
inline constexpr uint32_t kKeySignalSbasEna = 0x10310020U;     // CFG-SIGNAL-SBAS_ENA, L
inline constexpr uint32_t kKeySignalGalEna = 0x10310021U;      // CFG-SIGNAL-GAL_ENA, L
inline constexpr uint32_t kKeySignalBdsEna = 0x10310022U;      // CFG-SIGNAL-BDS_ENA, L
inline constexpr uint32_t kKeySignalQzssEna = 0x10310024U;     // CFG-SIGNAL-QZSS_ENA, L
inline constexpr uint32_t kKeySignalGloEna = 0x10310025U;      // CFG-SIGNAL-GLO_ENA, L

// TMODE3 / Time Mode 键 ID（UBX-22008968 §6.9.25）
inline constexpr uint32_t kKeyTmodeMode = 0x20030001U;         // E1: 0=Disabled,1=SurveyIn,2=Fixed
inline constexpr uint32_t kKeyTmodePosType = 0x20030002U;      // E1: 0=ECEF,1=LLH
inline constexpr uint32_t kKeyTmodeLat = 0x40030009U;          // I4, 1e-7 deg
inline constexpr uint32_t kKeyTmodeLon = 0x4003000AU;          // I4, 1e-7 deg
inline constexpr uint32_t kKeyTmodeHeight = 0x4003000BU;       // I4, cm
inline constexpr uint32_t kKeyTmodeLatHp = 0x2003000CU;        // I1, 1e-9 deg
inline constexpr uint32_t kKeyTmodeLonHp = 0x2003000DU;        // I1, 1e-9 deg
inline constexpr uint32_t kKeyTmodeHeightHp = 0x2003000EU;     // I1, 0.1 mm
inline constexpr uint32_t kKeyTmodeFixedPosAcc = 0x4003000FU;  // U4, 0.1 mm
inline constexpr uint32_t kKeyTmodeSvinMinDur = 0x40030010U;   // U4, s
inline constexpr uint32_t kKeyTmodeSvinAccLimit = 0x40030011U; // U4, 0.1 mm

// 基站 RTCM3 MSGOUT UART1 键（UART2 = UART1 + 1；UBX-22008968 §6.9.14）
inline constexpr uint32_t kKeyMsgoutRtcm1005Uart1 = 0x209102BEU;
inline constexpr uint32_t kKeyMsgoutRtcm1230Uart1 = 0x20910304U;
inline constexpr uint32_t kKeyMsgoutRtcm1074Uart1 = 0x2091035FU;
inline constexpr uint32_t kKeyMsgoutRtcm1084Uart1 = 0x20910364U;
inline constexpr uint32_t kKeyMsgoutRtcm1094Uart1 = 0x20910369U;
inline constexpr uint32_t kKeyMsgoutRtcm1124Uart1 = 0x2091036EU;
inline constexpr uint32_t kKeyMsgoutRtcm1077Uart1 = 0x209102CDU;
inline constexpr uint32_t kKeyMsgoutRtcm1087Uart1 = 0x209102D2U;
inline constexpr uint32_t kKeyMsgoutRtcm1097Uart1 = 0x2091030EU;
inline constexpr uint32_t kKeyMsgoutRtcm1127Uart1 = 0x2091034AU;

/** MON-VER 轮询帧（空载荷）已算好的 Fletcher 校验：载荷为空，校验只覆盖 class/id/len。 */
inline constexpr uint8_t kMonVerPollCkA = 0x0E;
inline constexpr uint8_t kMonVerPollCkB = 0x34;
inline constexpr size_t kMonVerPollBytes = 8U;
/** UBX-SEC-UNIQID 轮询帧（空载荷）的 Fletcher 校验：覆盖 0x27 0x03 0x00 0x00。 */
inline constexpr uint8_t kSecUniqIdPollCkA = 0x2AU;
inline constexpr uint8_t kSecUniqIdPollCkB = 0xA5U;
inline constexpr size_t kSecUniqIdPollBytes = 8U;

} // namespace gnss::ublox
