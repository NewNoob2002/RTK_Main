// 复位原因短名（ADR-0009）：启动横幅与 sys info 共用一份映射，避免两份 switch 漂移。
// 线程：任意任务与启动期可调用；纯函数、无状态，ISR 安全。
#pragma once

#include "esp_system.h"

namespace app {

/**
 * @brief 取得复位原因的静态短名
 * @param reason: IDF 复位原因
 * @retval 已知复位原因短名；未知值返回 unknown
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *resetReasonName(esp_reset_reason_t reason) noexcept;

} // namespace app
