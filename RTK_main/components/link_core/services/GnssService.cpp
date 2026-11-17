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
    uint16_t msgId;
    uint32_t defaultPeriodMs;
};

constexpr std::array<MsgSpec, kGnssMsgCount> kMsgSpecs = {{
    {kIdGga, 1000U},
    {kIdSat, 5000U},
    {kIdHeading, 1000U},
    {kIdGst, 1000U},
    {kIdRmc, 1000U},
    {kIdGsa, 3000U},
}};

inline DispatchResult replyBuild(Reply &reply, uint16_t msgId, uint8_t reqType,
                                 std::span<const uint8_t> payload) noexcept
{
    size_t len = 0;
    const fw::Status status = proto::buildFrame(reply.buf, msgId, proto::sender::kDevice,
                                                proto::responseType(reqType), payload, len);
    if (!isOk(status)) {
        reply.len = 0;
        return DispatchResult::kNoReply;
    }
    reply.len = len;
    return DispatchResult::kReply;
}

inline DispatchResult replyResult(Reply &reply, uint16_t msgId, uint8_t reqType,
                                  uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuild(reply, msgId, reqType, payload);
}

} // namespace

void GnssService::setQueryHook(GnssQueryFn fn, void *ctx) noexcept
{
    query_fn_ = fn;
    query_ctx_ = ctx;
}

void GnssService::setMaskHook(GnssMaskChangedFn fn, void *ctx) noexcept
{
    mask_fn_ = fn;
    mask_ctx_ = ctx;
}

void GnssService::setSupportedMask(uint32_t mask) noexcept
{
    supported_mask_ = mask & ((1U << kGnssMsgCount) - 1U);
}

uint32_t GnssService::supportedMask() const noexcept
{
    return supported_mask_;
}

void GnssService::resetSession() noexcept
{
    const bool changed = (active_mask_ != 0U);
    active_mask_ = 0U;
    periods_ms_.fill(0U);
    if (changed && (mask_fn_ != nullptr)) {
        mask_fn_(mask_ctx_, active_mask_);
    }
}

uint32_t GnssService::activeMask() const noexcept
{
    return active_mask_;
}

uint32_t GnssService::periodMs(size_t typeIdx) const noexcept
{
    if (typeIdx >= kGnssMsgCount) {
        return 0U;
    }
    return periods_ms_[typeIdx];
}

uint32_t GnssService::queryOversizeCount() const noexcept
{
    return query_oversize_;
}

uint32_t GnssService::intervalToPeriodMs(int8_t interval) noexcept
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

std::optional<size_t> GnssService::typeIndexOf(uint16_t msgId) noexcept
{
    for (size_t i = 0; i < kMsgSpecs.size(); ++i) {
        if (kMsgSpecs[i].msgId == msgId) {
            return i;
        }
    }
    return std::nullopt;
}

uint16_t GnssService::msgIdAt(size_t typeIdx) noexcept
{
    if (typeIdx >= kMsgSpecs.size()) {
        return 0U;
    }
    return kMsgSpecs[typeIdx].msgId;
}

uint32_t GnssService::defaultPeriodMs(size_t typeIdx) noexcept
{
    if (typeIdx >= kMsgSpecs.size()) {
        return 0U;
    }
    return kMsgSpecs[typeIdx].defaultPeriodMs;
}

DispatchResult GnssService::handleFrame(const proto::FrameView &frame, Reply &reply) noexcept
{
    if ((frame.type != proto::msg_type::kQuery) && (frame.type != proto::msg_type::kSet)) {
        return replyResult(reply, frame.msgId, frame.type, kResultFail);
    }

    const std::optional<size_t> idxOpt = typeIndexOf(frame.msgId);
    if (!idxOpt.has_value()) {
        return DispatchResult::kNoReply;
    }
    const size_t idx = *idxOpt;
    const uint32_t bit = 1U << idx;

    // 协议第 19 字节按有符号 char 解释：先保位模式转为 int8_t，供 intervalToPeriodMs 解析。
    const auto interval = static_cast<int8_t>(frame.interval);
    if (interval == 0) {
        const uint32_t nextMask = active_mask_ & ~bit;
        const bool changed = (nextMask != active_mask_);
        active_mask_ = nextMask;
        periods_ms_[idx] = 0U;
        if (changed && (mask_fn_ != nullptr)) {
            mask_fn_(mask_ctx_, active_mask_);
        }

        if ((supported_mask_ & bit) == 0U) {
            return replyResult(reply, frame.msgId, frame.type, kResultFail);
        }
        if (query_fn_ == nullptr) {
            return replyResult(reply, frame.msgId, frame.type, kResultFail);
        }

        std::array<uint8_t, proto::kMaxPayload> buf{};
        const GnssQueryResult q = query_fn_(query_ctx_, frame.msgId, buf);
        if (q.oversize) {
            ++query_oversize_;
            return replyResult(reply, frame.msgId, frame.type, kResultFail);
        }
        if (!q.ok || (q.len == 0U) || (q.len > buf.size())) {
            return replyResult(reply, frame.msgId, frame.type, kResultFail);
        }
        return replyBuild(reply, frame.msgId, frame.type,
                          std::span<const uint8_t>{buf.data(), q.len});
    }

    if ((supported_mask_ & bit) == 0U) {
        return replyResult(reply, frame.msgId, frame.type, kResultFail);
    }

    periods_ms_[idx] = intervalToPeriodMs(interval);
    const uint32_t nextMask = active_mask_ | bit;
    const bool changed = (nextMask != active_mask_);
    active_mask_ = nextMask;
    if (changed && (mask_fn_ != nullptr)) {
        mask_fn_(mask_ctx_, active_mask_);
    }
    return replyResult(reply, frame.msgId, frame.type, kResultOk);
}

std::span<const Route> GnssService::routes() noexcept
{
    struct Handlers {
        static DispatchResult handle(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                     Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            return static_cast<GnssService *>(self)->handleFrame(frame, reply);
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
