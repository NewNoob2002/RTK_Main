// rtk::core::Transport 的 SPP 适配器（ADR-0003）：无状态，只转调 C 驱动并映射错误码。
// 线程安全：与底层驱动相同，只允许链路任务调用。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk/bt/spp_transport.hpp"

#include "rtk/os/esp_status.hpp"
#include "rtk_bt_spp.h"

namespace rtk::bt {

Status SppTransport::write(core::SessionHandle handle, std::span<const uint8_t> data, uint32_t timeout_ms) noexcept
{
    /* 空 span 不调用驱动：C 接口要求 data 非 NULL 且 len > 0。 */
    if (data.empty()) {
        return Status::kInvalidArg;
    }
    return os::from_esp_err(rtk_bt_spp_write(handle, data.data(), data.size(), timeout_ms));
}

Status SppTransport::disconnect(core::SessionHandle handle) noexcept
{
    return os::from_esp_err(rtk_bt_spp_disconnect(handle));
}

} // namespace rtk::bt
