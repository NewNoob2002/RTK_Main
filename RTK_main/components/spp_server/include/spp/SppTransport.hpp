// C SPP 驱动（SppServer.h）到 core::Transport 的 C++ 适配器，供 LinkService 使用。
// 只做两件事：转调 C 函数；用 rtos::fromEspErr 映射错误码（CPP-014）。不持有状态。
// 线程安全：与底层驱动相同，只允许链路任务调用。
// 冻结接口（ADR-0003）。实现方不得修改。
#pragma once

#include <cstdint>
#include <span>

#include "core/Transport.hpp"
#include "core/Types.hpp"
#include "fw/Status.hpp"

namespace spp {

/** C SPP 驱动的 Transport 适配器：无状态、只持有驱动句柄；只允许链路任务调用，非 ISR。 */
class SppTransport final : public core::Transport {
public:
    constexpr SppTransport() noexcept = default;

    [[nodiscard]] fw::Status write(core::SessionHandle handle, std::span<const uint8_t> data,
                                   uint32_t timeoutMs) noexcept override;
    [[nodiscard]] bool supportsCredit() const noexcept override
    {
        return true;
    }
    [[nodiscard]] fw::Status reserveReply(core::SessionHandle handle,
                                          core::ReplyCredit &credit) noexcept override;
    [[nodiscard]] fw::Status commitReply(const core::ReplyCredit &credit,
                                         std::span<const uint8_t> data) noexcept override;
    void cancelReply(const core::ReplyCredit &credit) noexcept override;
    [[nodiscard]] bool replyReady(core::SessionHandle handle) const noexcept override;
    [[nodiscard]] fw::Status pushLatest(core::SessionHandle handle,
                                        std::span<const uint8_t> data) noexcept override;
    [[nodiscard]] fw::Status disconnect(core::SessionHandle handle) noexcept override;
};

} // namespace spp
