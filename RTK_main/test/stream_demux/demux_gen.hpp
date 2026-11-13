// 分流器测试帧生成辅助接口（独立位运算 CRC 实现，黑盒测试用，不共享被测组件查找表）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace demux_test {

// 协议帧上限常量（与 ADR-0004 §4 及冻结头一致）
inline constexpr std::size_t kRtcm3MaxPayload = 1023U;
inline constexpr std::size_t kRtcm3MaxFrame = 1029U;
inline constexpr std::size_t kNmeaMaxFrame = 256U;
inline constexpr std::size_t kUbxMaxFrame = 4096U;
inline constexpr std::size_t kUbxMaxPayload = 4088U;
inline constexpr std::size_t kUnicoreBinMaxFrame = 4096U;
inline constexpr std::size_t kUnicoreBinMaxPayload = 4068U;
inline constexpr std::size_t kUnicoreHashMaxFrame = 4096U;

struct UbxChecksum {
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
};

/** CRC-24Q（多项式 0x1864CFB，初值 0，无取反）；返回低 24 位。线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] uint32_t crc24q(std::span<const uint8_t> data) noexcept;

/** 标准 IEEE CRC-32（反射多项式 0xEDB88320，初值 0xFFFFFFFF，末位取反）。线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] uint32_t crc32_ieee(std::span<const uint8_t> data) noexcept;

/** NovAtel / Unicore CRC-32（反射多项式 0xEDB88320，初值 0，不取反）。线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] uint32_t crc32_novatel(std::span<const uint8_t> data) noexcept;

/** 逐字节异或校验。线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] uint8_t xor_checksum(std::span<const uint8_t> data) noexcept;

/** 8 位 Fletcher 校验（UBX CK_A / CK_B）。线程/ISR 安全：是（纯函数）。 */
[[nodiscard]] UbxChecksum ubx_checksum(std::span<const uint8_t> data) noexcept;

/** 生成合法 RTCM3 帧；payload.size() <= 1023。线程/ISR 安全：是。 */
[[nodiscard]] bool gen_rtcm3(std::span<uint8_t> out, std::span<const uint8_t> payload, std::size_t &out_len) noexcept;

/** 生成合法 NMEA 0183 语句；body 为 '$' 与 '*' 之间的内容（含首字符 [A-Z0-9]）。线程/ISR 安全：是。 */
[[nodiscard]] bool gen_nmea(std::span<uint8_t> out, std::string_view body, bool lowercase_hex,
                            std::size_t &out_len) noexcept;

/** 生成合法 u-blox UBX 帧；payload.size() <= 4088。线程/ISR 安全：是。 */
[[nodiscard]] bool gen_ubx(std::span<uint8_t> out, uint8_t msg_class, uint8_t msg_id, std::span<const uint8_t> payload,
                           std::size_t &out_len) noexcept;

/** 生成合法 Unicore 二进制帧；hdr_rest_16 为空时补 0，否则长度须为 16；payload.size() <= 4068。线程/ISR 安全：是。 */
[[nodiscard]] bool gen_unicore_bin(std::span<uint8_t> out, uint16_t msg_id, uint8_t cpu_idle,
                                   std::span<const uint8_t> hdr_rest_16, std::span<const uint8_t> payload,
                                   std::size_t &out_len) noexcept;

/** 生成合法 Unicore Hash 语句（8 位十六进制 CRC-32）；body 为 '#' 与 '*' 之间的内容。线程/ISR 安全：是。 */
[[nodiscard]] bool gen_unicore_hash_crc32(std::span<uint8_t> out, std::string_view body, bool lowercase_hex,
                                          std::size_t &out_len) noexcept;

/** 生成合法 Unicore Hash 语句（2 位十六进制 XOR）；body 为 '#' 与 '*' 之间的内容。线程/ISR 安全：是。 */
[[nodiscard]] bool gen_unicore_hash_xor(std::span<uint8_t> out, std::string_view body, bool lowercase_hex,
                                        std::size_t &out_len) noexcept;

/** 生成合法产品 SPP 帧（内部调用 proto::build_frame）。线程/ISR 安全：是。 */
[[nodiscard]] bool gen_product(std::span<uint8_t> out, uint16_t msg_id, uint8_t sender, uint8_t type,
                               std::span<const uint8_t> payload, std::size_t &out_len) noexcept;

} // namespace demux_test
