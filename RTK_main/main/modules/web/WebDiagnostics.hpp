// WebSocket 堆及任务栈诊断的 ESP-IDF 采样接口。
// 仅 kLink 任务，非 ISR，无动态分配。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rtos {
class BootArena;
}

namespace app::web_cfg {

struct DevWebStatus;

/** @brief 补充 status.dev 的能力堆字段。 @param dev: 待补充的设备状态。 @note 仅 kLink，非 ISR。 */
void sampleHeapStatus(DevWebStatus &dev) noexcept;

/**
 * @brief 编码按需诊断的一帧，读取已封存的启动台账或任务栈。
 * @param id: 请求 ID。
 * @param name: 请求名 diag 或 diag.tasks。
 * @param arena: 已封存的 BootArena。
 * @param[out] out: 发送帧缓冲。
 * @retval 正值 JSON 字节数。
 * @retval 0 编码失败。
 * @note 仅 kLink，非 ISR，无动态分配。
 */
[[nodiscard]] size_t buildDiagFrame(uint16_t id, std::string_view name,
                                    const rtos::BootArena &arena, std::span<char> out) noexcept;

} // namespace app::web_cfg
