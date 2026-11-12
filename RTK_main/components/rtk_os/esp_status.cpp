// ESP-IDF 错误码到 rtk::Status 的唯一映射点（CPP-014），以及单调毫秒时钟。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk/os/esp_status.hpp"

#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"

namespace rtk::os {

namespace {

constexpr int64_t kUsPerMs = 1000;

} // namespace

Status from_esp_err(esp_err_t err) noexcept
{
    switch (err) {
    case ESP_OK:
        return Status::kOk;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return Status::kInvalidArg;
    case ESP_ERR_NO_MEM:
        return Status::kNoSpace;
    case ESP_ERR_TIMEOUT:
        return Status::kTimeout;
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_NVS_NOT_FOUND:
        return Status::kNotFound;
    case ESP_ERR_NOT_SUPPORTED:
        return Status::kNotSupported;
    case ESP_ERR_INVALID_STATE:
        return Status::kInvalidState;
    case ESP_ERR_INVALID_CRC:
        return Status::kCorrupt;
    default:
        return Status::kIoError;
    }
}

uint32_t now_ms() noexcept
{
    // esp_timer_get_time() 为 int64 微秒；除以 1000 得毫秒。
    // 毫秒时间按 2^32 取模（约 49.7 天回绕），调用方必须用无符号差值比较。
    return static_cast<uint32_t>(esp_timer_get_time() / kUsPerMs);
}

} // namespace rtk::os
