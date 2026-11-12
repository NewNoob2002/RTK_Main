// u-blox F9 代方言（ZED-F9P）：UBX 二进制配置。
// 探测：UBX-MON-VER 轮询（B5 62 0A 04 00 00 0E 34）→ MON-VER 应答，扩展字段 "MOD=ZED-F9P" 定型号。
// 配置：UBX-CFG-VALSET（layer RAM|BBR）逐组键值 → UBX-ACK-ACK / ACK-NAK（class/id 匹配 06 8A）。
//   键：CFG-RATE-MEAS、CFG-MSGOUT-NMEA_ID_{GGA,RMC,GSA,GSV}_UART1、CFG-UART1INPROT-RTCM3X=1、CFG-UART1-BAUDRATE。
// 依据：u-blox F9 HPG 1.32 Interface Description（未在实物上验证，HIL 只有 UM980）。
// 构建开关：IDF 下 CONFIG_RTK_GNSS_DIALECT_UBLOX，主机 CMake 下 RTK_GNSS_DIALECT_UBLOX。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include "gnss/dialect.hpp"

namespace gnss::ublox {

/** u-blox 方言描述符（定义在 src/dialects/ublox.cpp）；应答解码器使用 demux::ubx::kDecoder。 */
extern const Dialect kDialect;

} // namespace gnss::ublox
