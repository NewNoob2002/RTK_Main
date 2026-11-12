// 冻结接口（ADR-0003）。实现方不得修改。
// C SPP 驱动（rtk_bt_spp.h）到 rtk::core::Transport 的 C++ 适配器，供 LinkService 使用。
// 只做两件事：转调 C 函数；用 rtk::os::from_esp_err 映射错误码（CPP-014）。不持有状态。
// 线程安全：与底层驱动相同，只允许链路任务调用。
#pragma once

#include <cstdint>
#include <span>

#include "rtk/core/transport.hpp"
#include "rtk/core/types.hpp"
#include "rtk/status.hpp"

namespace rtk::bt {

class SppTransport final : public core::Transport {
public:
    constexpr SppTransport() noexcept = default;

    [[nodiscard]] Status write(core::SessionHandle handle, std::span<const uint8_t> data,
                               uint32_t timeout_ms) noexcept override;
    [[nodiscard]] Status disconnect(core::SessionHandle handle) noexcept override;
};

} // namespace rtk::bt
