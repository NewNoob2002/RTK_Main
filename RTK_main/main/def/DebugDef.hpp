// 调试模块跨模块共享类型与命令动词定义（ADR-0009：追踪环与 debug 命令组）。
// 只含类型与枚举定义，无运行期状态；由 debug 模块与 debug 命令组共用，线程/ISR 安全。
#pragma once

#include <cstdint>

#include "AppConfig.hpp"

namespace app {

/** debug 命令动词。 */
enum class DebugVerb : uint8_t { kList = 0, kOn, kOff, kReport, kSave };

/**
 * @brief 读取命令动词的数值编号
 * @param value: 待转换或写入的值
 * @retval 与枚举底层值相同的 uint8_t 动词
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t verb(DebugVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

} // namespace app
