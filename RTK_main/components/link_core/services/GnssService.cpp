// GnssService 实现（ADR-0011 §6）：PHONE 侧 0x04/0x05/0x1F/0x21/0x22/0x23 订阅与最新帧查询。
// 线程安全：否（仅由链路任务单线程访问）。ISR 安全：否。

#include "core/services/GnssService.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"
#include "proto/Frame.hpp"

namespace core::services {

namespace {

inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

inline constexpr uint16_t kIdGga = 0x0004;
inline constexpr uint16_t kIdSat = 0x0005;
inline constexpr uint16_t kIdHeading = 0x001F;
inline constexpr uint16_t kIdGst = 0x0021;
inline constexpr uint16_t kIdRmc = 0x0022;
inline constexpr uint16_t kIdGsa = 0x0023;

inline constexpr uint32_t kMinPositivePeriodMs = 50;
inline constexpr uint32_t kMsPerSecond = 1000;

struct MsgSpec {
    uint16_t msg_id;
    uint32_t default_period_ms;
};

constexpr std::array<MsgSpec, kGnssMsgCount> kMsgSpecs = {{
    {kIdGga, 1000U},
    {kIdSat, 5000U},
    {kIdHeading, 1000U},
    {kIdGst, 1000U},
    {kIdRmc, 1000U},
    {kIdGsa, 3000U},
}};

inline DispatchResult reply_build(Reply &reply, uint16_t msg_id, uint8_t req_type,
                                  std::span<const uint8_t> payload) noexcept
{
    size_t len = 0;
    const fw::Status status =
        proto::build_frame(reply.buf, msg_id, proto::sender::kDevice, proto::response_type(req_type), payload, len);
    if (!is_ok(status)) {
        reply.len = 0;
        return DispatchResult::kNoReply;
    }
    reply.len = len;
    return DispatchResult::kReply;
}

inline DispatchResult reply_result(Reply &reply, uint16_t msg_id, uint8_t req_type, uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return reply_build(reply, msg_id, req_type, payload);
}

} // namespace

void GnssService::set_query_hook(GnssQueryFn fn, void *ctx) noexcept
{
    query_fn_ = fn;
    query_ctx_ = ctx;
}

void GnssService::set_mask_hook(GnssMaskChangedFn fn, void *ctx) noexcept
{
    mask_fn_ = fn;
    mask_ctx_ = ctx;
}

void GnssService::set_supported_mask(uint32_t mask) noexcept
{
    supported_mask_ = mask & ((1U << kGnssMsgCount) - 1U);
}

uint32_t GnssService::supported_mask() const noexcept
{
    return supported_mask_;
}

void GnssService::reset_session() noexcept
{
    const bool changed = (active_mask_ != 0U);
    active_mask_ = 0U;
    periods_ms_.fill(0U);
    if (changed && (mask_fn_ != nullptr)) {
        mask_fn_(mask_ctx_, active_mask_);
    }
}

uint32_t GnssService::active_mask() const noexcept
{
    return active_mask_;
}

uint32_t GnssService::period_ms(size_t type_idx) const noexcept
{
    if (type_idx >= kGnssMsgCount) {
        return 0U;
    }
    return periods_ms_[type_idx];
}

uint32_t GnssService::query_oversize_count() const noexcept
{
    return query_oversize_;
}

uint32_t GnssService::interval_to_period_ms(int8_t interval) noexcept
{
    if (interval == 0) {
        return 0U;
    }
    if (interval < 0) {
        // 先扩到 int32_t 再取反，避免 -128（INT8_MIN）在有符号 8 位上取负溢出。
        const int32_t sec = -static_cast<int32_t>(interval);
        return static_cast<uint32_t>(sec) * kMsPerSecond;
    }
    const auto hz = static_cast<uint32_t>(static_cast<uint8_t>(interval));
    return std::max(kMinPositivePeriodMs, kMsPerSecond / hz);
}

std::optional<size_t> GnssService::type_index_of(uint16_t msg_id) noexcept
{
    for (size_t i = 0; i < kMsgSpecs.size(); ++i) {
        if (kMsgSpecs[i].msg_id == msg_id) {
            return i;
        }
    }
    return std::nullopt;
}

uint16_t GnssService::msg_id_at(size_t type_idx) noexcept
{
    if (type_idx >= kMsgSpecs.size()) {
        return 0U;
    }
    return kMsgSpecs[type_idx].msg_id;
}

uint32_t GnssService::default_period_ms(size_t type_idx) noexcept
{
    if (type_idx >= kMsgSpecs.size()) {
        return 0U;
    }
    return kMsgSpecs[type_idx].default_period_ms;
}

DispatchResult GnssService::handle_frame(const proto::FrameView &frame, Reply &reply) noexcept
{
    if ((frame.type != proto::msg_type::kQuery) && (frame.type != proto::msg_type::kSet)) {
        return reply_result(reply, frame.msg_id, frame.type, kResultFail);
    }

    const std::optional<size_t> idx_opt = type_index_of(frame.msg_id);
    if (!idx_opt.has_value()) {
        return DispatchResult::kNoReply;
    }
    const size_t idx = *idx_opt;
    const uint32_t bit = 1U << idx;

    // 协议第 19 字节按有符号 char 解释：先保位模式转为 int8_t，供 interval_to_period_ms 解析。
    const auto interval = static_cast<int8_t>(frame.interval);
    if (interval == 0) {
        const uint32_t next_mask = active_mask_ & ~bit;
        const bool changed = (next_mask != active_mask_);
        active_mask_ = next_mask;
        periods_ms_[idx] = 0U;
        if (changed && (mask_fn_ != nullptr)) {
            mask_fn_(mask_ctx_, active_mask_);
        }

        if ((supported_mask_ & bit) == 0U) {
            return reply_result(reply, frame.msg_id, frame.type, kResultFail);
        }
        if (query_fn_ == nullptr) {
            return reply_result(reply, frame.msg_id, frame.type, kResultFail);
        }

        std::array<uint8_t, proto::kMaxPayload> buf{};
        const GnssQueryResult q = query_fn_(query_ctx_, frame.msg_id, buf);
        if (q.oversize) {
            ++query_oversize_;
            return reply_result(reply, frame.msg_id, frame.type, kResultFail);
        }
        if (!q.ok || (q.len == 0U) || (q.len > buf.size())) {
            return reply_result(reply, frame.msg_id, frame.type, kResultFail);
        }
        return reply_build(reply, frame.msg_id, frame.type, std::span<const uint8_t>{buf.data(), q.len});
    }

    if ((supported_mask_ & bit) == 0U) {
        return reply_result(reply, frame.msg_id, frame.type, kResultFail);
    }

    periods_ms_[idx] = interval_to_period_ms(interval);
    const uint32_t next_mask = active_mask_ | bit;
    const bool changed = (next_mask != active_mask_);
    active_mask_ = next_mask;
    if (changed && (mask_fn_ != nullptr)) {
        mask_fn_(mask_ctx_, active_mask_);
    }
    return reply_result(reply, frame.msg_id, frame.type, kResultOk);
}

std::span<const Route> GnssService::routes() noexcept
{
    struct Handlers {
        static DispatchResult handle(void *self, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            return static_cast<GnssService *>(self)->handle_frame(frame, reply);
        }
    };

    static constexpr std::array<Route, kGnssMsgCount> kRoutes = {{
        {kIdGga, kRolePhone, &Handlers::handle},
        {kIdSat, kRolePhone, &Handlers::handle},
        {kIdHeading, kRolePhone, &Handlers::handle},
        {kIdGst, kRolePhone, &Handlers::handle},
        {kIdRmc, kRolePhone, &Handlers::handle},
        {kIdGsa, kRolePhone, &Handlers::handle},
    }};
    return kRoutes;
}

} // namespace core::services
