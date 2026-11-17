// AlarmService 实现（ADR-0007 §3.3）：报警相关指令 0x40/0x41/0x42/0x43 与 0x40 主动推送帧。
// 各分支自 command_dispatcher.cpp 逐字节搬迁，应答内容与旧 CommandDispatcher 一致。

#include "core/services/AlarmService.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"
#include "core/TraceCodes.hpp"
#include "proto/AlarmCodec.hpp"
#include "proto/Frame.hpp"

namespace core::services {

namespace {

// 通用结果码（phone_protocol.md §3；沿用旧 CommandDispatcher 的取值）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

/**
 * 组应答帧：sender 固定为 DEV，type 经 responseType 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * 组帧失败时把 reply.len 置 0 并返回 kNoReply（与旧 dispatcher 一致）。
 */
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

/** 结果码应答：§3 规定成功/失败都把同一字节写两遍。 */
inline DispatchResult replyResult(Reply &reply, uint16_t msgId, uint8_t reqType,
                                  uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuild(reply, msgId, reqType, payload);
}

/** 成功应答（0x01 0x01）。 */
inline DispatchResult replyOk(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultOk);
}

/** 失败应答（0x02 0x02）。 */
inline DispatchResult replyFail(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultFail);
}

namespace alarm = proto::alarm;
namespace msg_type = proto::msg_type;

// 0x42 测试报警 payload。
constexpr size_t kTestMsgLen = 3;
constexpr uint8_t kTestActionSet = 0x01;
constexpr uint8_t kTestActionClear = 0x00;

// 0x40 查询应答的 seq 固定为 0（与旧 dispatcher 一致）。
constexpr uint16_t kStatusQuerySeq = 0;

/** 组报警状态结构：状态/等级来自 AlarmSource，手机在线与测试覆盖写入 flags（§5）。 */
alarm::Status makeStatus(const AlarmSource &alarmSource, const LinkManager &link,
                         const DeviceInfo &info, uint16_t seq, uint32_t nowMs) noexcept
{
    alarm::Status status;
    status.seq = seq;
    status.state = alarmSource.state();
    status.level = alarmSource.level();
    status.fixQuality = info.fixQuality;
    status.batteryPct = info.batteryPct;

    uint8_t flags = 0;
    if (link.hasRole(Role::kPhone)) {
        flags = static_cast<uint8_t>(flags | alarm::kFlagPhoneOnline);
    }
    if (alarmSource.isOverride()) {
        flags = static_cast<uint8_t>(flags | alarm::kFlagTestOverride);
    }
    status.flags = flags;
    status.uptimeMs = nowMs;
    // 每次组帧都报告当前报警状态，事件里的 session 无意义（应答/推送由调用方打）。
    traceAlarm(AlarmTrace::kState, 0, static_cast<uint32_t>(status.state),
               static_cast<uint32_t>(status.level));
    return status;
}

DispatchResult handleStatusQuery(const AlarmSource &alarmSource, const LinkManager &link,
                                 const DeviceInfo &info, const proto::FrameView &frame,
                                 uint32_t nowMs, Reply &reply) noexcept
{
    const alarm::Status status = makeStatus(alarmSource, link, info, kStatusQuerySeq, nowMs);
    std::array<uint8_t, alarm::kStatusLen> payload{};
    if (alarm::encodeStatus(status, payload) != payload.size()) {
        return DispatchResult::kNoReply;
    }
    return replyBuild(reply, frame.msgId, frame.type, payload);
}

DispatchResult handleHeartbeat(const proto::FrameView &frame, Reply &reply) noexcept
{
    const std::optional<alarm::Heartbeat> heartbeat = alarm::decodeHeartbeat(frame.payload);
    if (heartbeat.has_value()) {
        reply.heartbeat = heartbeat;
    }
    // 心跳/HELLO 按协议不回复（§5）。
    return DispatchResult::kNoReply;
}

DispatchResult handleTestOverride(AlarmSource &alarmSource, const proto::FrameView &frame,
                                  Reply &reply) noexcept
{
    if ((frame.type != msg_type::kSet) || (frame.payload.size() < kTestMsgLen)) {
        return replyFail(reply, frame.msgId, frame.type);
    }

    const uint8_t action = frame.payload[0];
    if (action == kTestActionSet) {
        if (!alarmSource.setOverride(frame.payload[1], frame.payload[2])) {
            return replyFail(reply, frame.msgId, frame.type);
        }
        traceAlarm(AlarmTrace::kOverride, 0, 1,
                   (static_cast<uint32_t>(frame.payload[1]) << 8U) |
                       static_cast<uint32_t>(frame.payload[2]));
        return replyOk(reply, frame.msgId, frame.type);
    }
    if (action == kTestActionClear) {
        alarmSource.clearOverride();
        traceAlarm(AlarmTrace::kOverride, 0, 0);
        return replyOk(reply, frame.msgId, frame.type);
    }
    return replyFail(reply, frame.msgId, frame.type);
}

/** 围栏本阶段预留：查询/设置均回失败（§5）。 */
DispatchResult handleFence(const proto::FrameView &frame, Reply &reply) noexcept
{
    return replyFail(reply, frame.msgId, frame.type);
}

} // namespace

AlarmService::AlarmService(AlarmSource &alarm, const LinkManager &link,
                           const DeviceInfo &info) noexcept
    : alarm_(alarm), link_(link), info_(info)
{}

bool AlarmService::buildStatusPush(uint16_t seq, uint32_t nowMs, Reply &reply) const noexcept
{
    reply.clear();
    const alarm::Status status = makeStatus(alarm_, link_, info_, seq, nowMs);
    std::array<uint8_t, alarm::kStatusLen> payload{};
    if (alarm::encodeStatus(status, payload) != payload.size()) {
        traceAlarm(AlarmTrace::kPushFail, 0, static_cast<uint32_t>(fw::Status::kInvalidArg));
        return false;
    }

    size_t len = 0;
    const fw::Status result = proto::buildFrame(
        reply.buf, alarm::kMsgStatus, proto::sender::kDevice, msg_type::kPush, payload, len);
    if (!isOk(result)) {
        traceAlarm(AlarmTrace::kPushFail, 0, static_cast<uint32_t>(result));
        return false;
    }
    reply.len = len;
    // 目标会话在写成功处由 LinkService 记录（session 0 表示“已组帧待发送”）。
    traceAlarm(AlarmTrace::kPush, 0, static_cast<uint32_t>(status.state),
               static_cast<uint32_t>(status.level));
    return true;
}

std::span<const Route> AlarmService::routes() noexcept
{
    // 冻结头只暴露 buildStatusPush，而 0x40 查询应答需要 AlarmSource/LinkManager/DeviceInfo 三份引用。
    // 局部类定义在成员函数内，按 [class.access] 与成员函数具有相同访问权限，因此 static_cast 之后
    // 可以取到私有引用；每个 self 都来自 table()（RouteTable{this, ...}），类型必然是本服务实例。
    struct Handlers {
        static DispatchResult statusQuery(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                          Reply &reply) noexcept
        {
            // 旧 dispatch_alarm：0x40 只接受 QUERY，其他 type 按角色无权处理（kDenied）。
            if (frame.type != msg_type::kQuery) {
                return DispatchResult::kDenied;
            }
            const auto *const service = static_cast<const AlarmService *>(self);
            return handleStatusQuery(service->alarm_, service->link_, service->info_, frame, nowMs,
                                     reply);
        }
        static DispatchResult heartbeat(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                        Reply &reply) noexcept
        {
            static_cast<void>(self);
            static_cast<void>(nowMs);
            // 旧 dispatch_alarm：0x41 只接受 PUSH，其他 type 返回 kDenied。
            if (frame.type != msg_type::kPush) {
                return DispatchResult::kDenied;
            }
            return handleHeartbeat(frame, reply);
        }
        static DispatchResult testOverride(void *self, const proto::FrameView &frame,
                                           uint32_t nowMs, Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            auto *const service = static_cast<AlarmService *>(self);
            return handleTestOverride(service->alarm_, frame, reply);
        }
        static DispatchResult fence(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                    Reply &reply) noexcept
        {
            static_cast<void>(self);
            static_cast<void>(nowMs);
            return handleFence(frame, reply);
        }
    };

    static constexpr std::array<Route, 4> kRoutes = {{
        {alarm::kMsgStatus, kRoleAlarm, &Handlers::statusQuery},
        {alarm::kMsgHeartbeat, kRoleAlarm, &Handlers::heartbeat},
        {alarm::kMsgTest, kRolePhone, &Handlers::testOverride},
        {alarm::kMsgFence, kRolePhone, &Handlers::fence},
    }};
    return kRoutes;
}

} // namespace core::services
