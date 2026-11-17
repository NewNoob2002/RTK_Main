// esp_err_t → fw::Status 的唯一映射点（CPP-014）与单调毫秒时钟。
// 纯映射/只读，不阻塞、可在 ISR 调用；不分配堆。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "fw/Status.hpp"

namespace rtos {

/**
 * @brief  把 ESP-IDF 错误码映射为 fw::Status
 * @param  err: ESP-IDF 错误码
 * @retval fw::Status::kOk ESP_OK
 * @retval fw::Status::kInvalidArg ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE
 * @retval fw::Status::kNoSpace ESP_ERR_NO_MEM
 * @retval fw::Status::kTimeout ESP_ERR_TIMEOUT
 * @retval fw::Status::kNotFound ESP_ERR_NOT_FOUND / ESP_ERR_NVS_NOT_FOUND
 * @retval fw::Status::kNotSupported ESP_ERR_NOT_SUPPORTED
 * @retval fw::Status::kInvalidState ESP_ERR_INVALID_STATE
 * @retval fw::Status::kCorrupt ESP_ERR_INVALID_CRC
 * @retval fw::Status::kIoError 其他未映射错误码
 * @note   纯映射，可在 ISR 中调用；不阻塞
 */
[[nodiscard]] fw::Status fromEspErr(esp_err_t err) noexcept;

/**
 * @brief  读取单调毫秒时间（esp_timer）
 * @retval 自启动起的毫秒数，按 2^32 取模（约 49.7 天回绕）
 * @note   所有比较须用无符号差值；可在 ISR 中调用；不阻塞
 */
[[nodiscard]] uint32_t nowMs() noexcept;

} // namespace rtos
