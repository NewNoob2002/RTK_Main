// core::Transport 的 SPP 适配器（ADR-0003）：无状态，只转调 C 驱动并映射错误码。
// 线程安全：与底层驱动相同，只允许链路任务调用。

#include "spp/spp_transport.hpp"

#include "rtos/esp_status.hpp"
#include "spp_server.h"

namespace spp {

fw::Status SppTransport::write(core::SessionHandle handle, std::span<const uint8_t> data, uint32_t timeout_ms) noexcept
{
    /* 空 span 不调用驱动：C 接口要求 data 非 NULL 且 len > 0。 */
    if (data.empty()) {
        return fw::Status::kInvalidArg;
    }
    return rtos::from_esp_err(spp_server_write(handle, data.data(), data.size(), timeout_ms));
}

fw::Status SppTransport::disconnect(core::SessionHandle handle) noexcept
{
    return rtos::from_esp_err(spp_server_disconnect(handle));
}

} // namespace spp
