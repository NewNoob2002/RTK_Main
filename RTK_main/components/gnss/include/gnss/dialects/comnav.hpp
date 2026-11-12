// 司南（ComNav）K8 系列方言（K803 等）：NovAtel 风格 ASCII 命令，实验性。
// 当前依据为公开资料的 NovAtel 兼容语法（LOG/UNLOGALL/COM/SAVECONFIG，应答 "<OK" 行），
// 未取得 K803 官方命令手册，未经实物验证；拿到手册后必须先修订 ADR-0010 §2.2 的 ComNav 小节再改实现。
// 构建开关：IDF 下 CONFIG_RTK_GNSS_DIALECT_COMNAV（默认关），主机 CMake 下 RTK_GNSS_DIALECT_COMNAV。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoder.hpp"
#include "gnss/dialect.hpp"

namespace gnss::comnav {

/** "<OK\r\n" / "<ERROR:...\r\n" 等以 '<' 开头的应答行解码器（可打印 ASCII 直到 \r\n，无校验）。 */
inline constexpr std::size_t kPromptMaxFrame = 128;
[[nodiscard]] bool prompt_first_byte(uint8_t byte) noexcept;
[[nodiscard]] demux::Step prompt_step(std::span<const uint8_t> frame, std::size_t &need) noexcept;
inline constexpr demux::Decoder kPromptDecoder{"ComnavPrompt", kPromptMaxFrame, &prompt_first_byte, &prompt_step};

/** ComNav 方言描述符（定义在 src/dialects/comnav.cpp）。 */
extern const Dialect kDialect;

} // namespace gnss::comnav
