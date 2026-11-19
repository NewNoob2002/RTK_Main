// GnssService 实现（ADR-0011 §6）：PHONE 侧 0x04/0x05/0x1F/0x21/0x22/0x23 订阅与最新帧查询。
// 线程安全：否（仅由链路任务单线程访问）。ISR 安全：否。堆：不分配（查询负载用栈上缓冲）。

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

/** @brief 按指定应答 type 组帧，sender 固定为 DEV。
 *  @param[out] reply 复用缓冲；成功时写 buf/len，失败时 len 置 0。
 *  @param msgId 请求的 msg_id。
 *  @param resType 应答帧的 type 字节。
 *  @param payload 应答负载（0..proto::kMaxPayload 字节）。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 负载超过 kMaxPayload 或编码失败。
 *  @note 不分配、不加锁；非 ISR 上下文调用。 */
inline DispatchResult replyBuildType(Reply &reply, uint16_t msgId, uint8_t resType,
                                     std::span<const uint8_t> payload) noexcept
{
    size_t len = 0;
    const fw::Status status =
        proto::buildFrame(reply.buf, msgId, proto::sender::kDevice, resType, payload, len);
    if (!isOk(status)) {
        reply.len = 0;
        return DispatchResult::kNoReply;
    }
    reply.len = len;
    return DispatchResult::kReply;
}

/** @brief 组应答帧：sender 固定为 DEV，type 经 responseType 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 *  @param[out] reply 复用缓冲；成功时写 buf/len，失败时 len 置 0。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @param payload 应答负载（0..proto::kMaxPayload 字节）。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 负载超过 kMaxPayload 或编码失败。
 *  @note 不分配、不加锁；非 ISR 上下文调用。 */
inline DispatchResult replyBuild(Reply &reply, uint16_t msgId, uint8_t reqType,
                                 std::span<const uint8_t> payload) noexcept
{
    return replyBuildType(reply, msgId, proto::responseType(reqType), payload);
}

/** @brief 按指定应答 type 组装两字节结果码应答。
 *  @param[out] reply 复用缓冲。
 *  @param msgId 请求的 msg_id。
 *  @param resType 应答帧的 type 字节。
 *  @param value 结果码（kResultOk/kResultFail）。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyResultType(Reply &reply, uint16_t msgId, uint8_t resType,
                                      uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuildType(reply, msgId, resType, payload);
}

/** @brief 结果码应答：§3 规定成功/失败都把同一字节写两遍。
 *  @param[out] reply 复用缓冲，语义同 replyBuild。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @param value 结果码（kResultOk/kResultFail）。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyResult(Reply &reply, uint16_t msgId, uint8_t reqType,
                                  uint8_t value) noexcept
{
    return replyResultType(reply, msgId, proto::responseType(reqType), value);
}

/** @brief 判断该 msgId 在指定 AppKind 下是否一律回复 set-res 结果码。
 *  @param msgId 请求消息号。
 *  @param appKind 手机 App 类型。
 *  @retval true Survey 的 0x04/0x21/0x22 或两类 App 的 0x23。
 *  @retval false 走原有 Nav/HEADING 查询或订阅应答逻辑。 */
constexpr bool usesSetResultReply(uint16_t msgId, AppKind appKind) noexcept
{
    if (msgId == kIdGsa) {
        return true;
    }
    return (appKind == AppKind::kSurvey) &&
           ((msgId == kIdGga) || (msgId == kIdGst) || (msgId == kIdRmc));
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
    const bool supported = (supported_mask_ & bit) != 0U;
    const AppKind appKind = appKindFromSender(frame.sender);

    // 协议第 19 字节按有符号 char 解释：先保位模式转为 int8_t，供 intervalToPeriodMs 解析。
    const auto interval = static_cast<int8_t>(frame.interval);
    const uint32_t prevMask = active_mask_;
    if (interval == 0) {
        active_mask_ &= ~bit;
        periods_ms_[idx] = 0U;
    } else if (supported) {
        periods_ms_[idx] = intervalToPeriodMs(interval);
        active_mask_ |= bit;
    }
    if ((active_mask_ != prevMask) && (mask_fn_ != nullptr)) {
        mask_fn_(mask_ctx_, active_mask_);
    }

    if (frame.msgId == kIdSat) {
        // P1 Rev A：0x05 按 App 类型回 2 B 全 0（Survey→set-res，Nav→query-res），订阅逻辑不变。
        constexpr std::array<uint8_t, 2U> kZero{};
        const uint8_t resType =
            (appKind == AppKind::kSurvey) ? proto::msg_type::kSetRes : proto::msg_type::kQueryRes;
        return replyBuildType(reply, frame.msgId, resType, kZero);
    }
    if (usesSetResultReply(frame.msgId, appKind)) {
        return replyResultType(reply, frame.msgId, proto::msg_type::kSetRes,
                               supported ? kResultOk : kResultFail);
    }
    if (!supported || (interval != 0)) {
        return replyResult(reply, frame.msgId, frame.type, supported ? kResultOk : kResultFail);
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
    return replyBuild(reply, frame.msgId, frame.type, std::span<const uint8_t>{buf.data(), q.len});
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
