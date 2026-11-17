// ESP-IDF 错误码到 fw::Status 的唯一映射点（CPP-014），以及单调毫秒时钟。
// 由各组件在 IDF 边界调用；from_esp_err 是纯映射，now_ms 只读 esp_timer，均不阻塞且 ISR 安全。

#include "rtos/EspStatus.hpp"

#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"

namespace rtos {

namespace {

constexpr int64_t kUsPerMs = 1000;

} // namespace

fw::Status from_esp_err(esp_err_t err) noexcept
{
    switch (err) {
    case ESP_OK:
        return fw::Status::kOk;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return fw::Status::kInvalidArg;
    case ESP_ERR_NO_MEM:
        return fw::Status::kNoSpace;
    case ESP_ERR_TIMEOUT:
        return fw::Status::kTimeout;
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_NVS_NOT_FOUND:
        return fw::Status::kNotFound;
    case ESP_ERR_NOT_SUPPORTED:
        return fw::Status::kNotSupported;
    case ESP_ERR_INVALID_STATE:
        return fw::Status::kInvalidState;
    case ESP_ERR_INVALID_CRC:
        return fw::Status::kCorrupt;
    default:
        return fw::Status::kIoError;
    }
}

uint32_t now_ms() noexcept
{
    // esp_timer_get_time() 为 int64 微秒；除以 1000 得毫秒。
    // 毫秒时间按 2^32 取模（约 49.7 天回绕），调用方必须用无符号差值比较。
    return static_cast<uint32_t>(esp_timer_get_time() / kUsPerMs);
}

} // namespace rtos
