// esp_err_t → fw::Status 的唯一映射点（CPP-014）。其余 RAII 包装（静态任务、队列、
// StreamBuffer、事件组、portMUX 临界区守卫）由实现方在 rtos 中按 T06 任务卡设计，经验收审查。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "fw/Status.hpp"

namespace rtos {

/**
 * ESP_OK→kOk，ESP_ERR_INVALID_ARG/INVALID_SIZE→kInvalidArg，ESP_ERR_NO_MEM→kNoSpace，
 * ESP_ERR_TIMEOUT→kTimeout，ESP_ERR_NOT_FOUND/ESP_ERR_NVS_NOT_FOUND→kNotFound，
 * ESP_ERR_NOT_SUPPORTED→kNotSupported，ESP_ERR_INVALID_STATE→kInvalidState，
 * ESP_ERR_INVALID_CRC→kCorrupt，其他→kIoError。
 */
[[nodiscard]] fw::Status from_esp_err(esp_err_t err) noexcept;

/** 单调毫秒时间（esp_timer），约 49.7 天回绕；所有比较须用无符号差值。ISR 安全：是。 */
[[nodiscard]] uint32_t now_ms() noexcept;

} // namespace rtos
