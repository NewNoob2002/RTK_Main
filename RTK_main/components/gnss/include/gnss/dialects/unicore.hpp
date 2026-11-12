// Unicore N4 平台方言（UM980/UM981/UM982）：ASCII 命令以 \r\n 结尾；
// 命令应答 "$command,<原命令>,response: OK*hh\r\n"（失败为 PARSING/其他文本），XOR 校验同 NMEA；
// 版本查询 "VERSIONA\r\n" → "#VERSIONA,...;\"UM980\",\"<fw>\",...*crc32\r\n"（demux::unicore_hash 成帧）。
// 依据：Unicore N4 高精度产品命令手册 V2 R1.6 §7（GPGGA COMx rate、UNLOG、CONFIG COMx baud、MODE ROVER、VERSIONA）。
// 构建开关：IDF 下 CONFIG_RTK_GNSS_DIALECT_UNICORE，主机 CMake 下 RTK_GNSS_DIALECT_UNICORE。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoder.hpp"
#include "gnss/dialect.hpp"

namespace gnss::unicore {

/**
 * "$command,...*hh\r\n" 应答解码器：$ | "command," | 可打印 ASCII（不含 $ *）| * 两位十六进制 XOR | \r\n。
 * 与 demux::nmea 首字节相同（$），由 nmea 的第 2 字节 [A-Z0-9] 规则让出（'c' 小写），故两者可同时挂载。
 */
inline constexpr std::size_t kResponseMaxFrame = 256;
[[nodiscard]] bool response_first_byte(uint8_t byte) noexcept;
[[nodiscard]] demux::Step response_step(std::span<const uint8_t> frame, std::size_t &need) noexcept;
inline constexpr demux::Decoder kResponseDecoder{"UnicoreResp", kResponseMaxFrame, &response_first_byte,
                                                 &response_step};

/** Unicore 方言描述符（定义在 src/dialects/unicore.cpp）。 */
extern const Dialect kDialect;

} // namespace gnss::unicore
