// 复位原因短名（R8a）：启动横幅与 sys info 共用一份映射，避免两份 switch 漂移。
// 线程：任意任务与启动期可调用；纯函数、无状态，ISR 安全。
#pragma once

#include "esp_system.h"

namespace app {

/** 复位原因短名（小写连字符）；未识别取值返回 "unknown"。 */
[[nodiscard]] const char *reset_reason_name(esp_reset_reason_t reason) noexcept;

} // namespace app
