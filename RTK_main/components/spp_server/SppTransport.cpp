// core::Transport 的 SPP 适配器（ADR-0003）：无状态，只转调 C 驱动并映射错误码。
// 线程安全：与底层驱动相同，只允许链路任务调用。

#include "spp/SppTransport.hpp"

#include "rtos/EspStatus.hpp"
#include "SppServer.h"

namespace spp {
namespace {
spp_server_tx_token_t tokenFrom(const core::ReplyCredit &credit) noexcept
{
    return {credit.generation, credit.serial, credit.capacity, credit.slot, credit.channel};
}
} // namespace

fw::Status SppTransport::reserveReply(core::SessionHandle handle,
                                      core::ReplyCredit &credit) noexcept
{
    spp_server_tx_token_t token{};
    const fw::Status status = rtos::fromEspErr(spp_server_reply_reserve(handle, &token));
    if (fw::isOk(status)) {
        credit = {token.generation, token.serial, token.capacity, token.slot, token.channel};
    }
    return status;
}

fw::Status SppTransport::commitReply(const core::ReplyCredit &credit,
                                     std::span<const uint8_t> data) noexcept
{
    const auto token = tokenFrom(credit);
    return rtos::fromEspErr(spp_server_tx_commit(&token, data.data(), data.size()));
}

void SppTransport::cancelReply(const core::ReplyCredit &credit) noexcept
{
    const auto token = tokenFrom(credit);
    spp_server_tx_cancel(&token);
}

bool SppTransport::replyReady(core::SessionHandle handle) const noexcept
{
    return spp_server_reply_ready(handle);
}

fw::Status SppTransport::pushLatest(core::SessionHandle handle,
                                    std::span<const uint8_t> data) noexcept
{
    return rtos::fromEspErr(spp_server_push_latest(handle, data.data(), data.size()));
}

fw::Status SppTransport::write(core::SessionHandle handle, std::span<const uint8_t> data,
                               uint32_t timeoutMs) noexcept
{
    /* 空 span 不调用驱动：C 接口要求 data 非 NULL 且 len > 0。 */
    if (data.empty()) {
        return fw::Status::kInvalidArg;
    }
    return rtos::fromEspErr(spp_server_write(handle, data.data(), data.size(), timeoutMs));
}

fw::Status SppTransport::disconnect(core::SessionHandle handle) noexcept
{
    return rtos::fromEspErr(spp_server_disconnect(handle));
}

} // namespace spp
