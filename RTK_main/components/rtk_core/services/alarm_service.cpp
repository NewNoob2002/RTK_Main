// AlarmService 实现（ADR-0007 §3.3）：报警相关指令 0x40/0x41/0x42/0x43 与 0x40 主动推送帧。
// 各分支自 command_dispatcher.cpp 逐字节搬迁，应答内容与旧 CommandDispatcher 一致。

#include "rtk/core/services/alarm_service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/core/command_router.hpp"
#include "rtk/core/reply.hpp"
#include "rtk/proto/alarm_codec.hpp"
#include "rtk/proto/frame.hpp"

namespace rtk::core::services {

namespace {

// 通用结果码（phone_protocol.md §3；沿用旧 CommandDispatcher 的取值）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

/**
 * 组应答帧：sender 固定为 DEV，type 经 response_type 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * 组帧失败时把 reply.len 置 0 并返回 kNoReply（与旧 dispatcher 一致）。
 */
inline DispatchResult reply_build(Reply &reply, uint16_t msg_id, uint8_t req_type,
                                  std::span<const uint8_t> payload) noexcept
{
    size_t len = 0;
    const Status status =
        proto::build_frame(reply.buf, msg_id, proto::sender::kDevice, proto::response_type(req_type), payload, len);
    if (!is_ok(status)) {
        reply.len = 0;
        return DispatchResult::kNoReply;
    }
    reply.len = len;
    return DispatchResult::kReply;
}

/** 结果码应答：§3 规定成功/失败都把同一字节写两遍。 */
inline DispatchResult reply_result(Reply &reply, uint16_t msg_id, uint8_t req_type, uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return reply_build(reply, msg_id, req_type, payload);
}

/** 成功应答（0x01 0x01）。 */
inline DispatchResult reply_ok(Reply &reply, uint16_t msg_id, uint8_t req_type) noexcept
{
    return reply_result(reply, msg_id, req_type, kResultOk);
}

/** 失败应答（0x02 0x02）。 */
inline DispatchResult reply_fail(Reply &reply, uint16_t msg_id, uint8_t req_type) noexcept
{
    return reply_result(reply, msg_id, req_type, kResultFail);
}

namespace alarm = rtk::proto::alarm;
namespace msg_type = rtk::proto::msg_type;

// 0x42 测试报警 payload。
constexpr size_t kTestMsgLen = 3;
constexpr uint8_t kTestActionSet = 0x01;
constexpr uint8_t kTestActionClear = 0x00;

// 0x40 查询应答的 seq 固定为 0（与旧 dispatcher 一致）。
constexpr uint16_t kStatusQuerySeq = 0;

/** 组报警状态结构：状态/等级来自 AlarmSource，手机在线与测试覆盖写入 flags（§5）。 */
alarm::Status make_status(const AlarmSource &alarm_source, const LinkManager &link, const DeviceInfo &info,
                          uint16_t seq, uint32_t now_ms) noexcept
{
    alarm::Status status;
    status.seq = seq;
    status.state = alarm_source.state();
    status.level = alarm_source.level();
    status.fix_quality = alarm::kValueUnknown;
    status.battery_pct = info.battery_pct;

    uint8_t flags = 0;
    if (link.has_role(Role::kPhone)) {
        flags = static_cast<uint8_t>(flags | alarm::kFlagPhoneOnline);
    }
    if (alarm_source.is_override()) {
        flags = static_cast<uint8_t>(flags | alarm::kFlagTestOverride);
    }
    status.flags = flags;
    status.uptime_ms = now_ms;
    return status;
}

DispatchResult handle_status_query(const AlarmSource &alarm_source, const LinkManager &link, const DeviceInfo &info,
                                   const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
{
    const alarm::Status status = make_status(alarm_source, link, info, kStatusQuerySeq, now_ms);
    std::array<uint8_t, alarm::kStatusLen> payload{};
    if (alarm::encode_status(status, payload) != payload.size()) {
        return DispatchResult::kNoReply;
    }
    return reply_build(reply, frame.msg_id, frame.type, payload);
}

DispatchResult handle_heartbeat(const proto::FrameView &frame, Reply &reply) noexcept
{
    const std::optional<alarm::Heartbeat> heartbeat = alarm::decode_heartbeat(frame.payload);
    if (heartbeat.has_value()) {
        reply.heartbeat = heartbeat;
    }
    // 心跳/HELLO 按协议不回复（§5）。
    return DispatchResult::kNoReply;
}

DispatchResult handle_test_override(AlarmSource &alarm_source, const proto::FrameView &frame, Reply &reply) noexcept
{
    if ((frame.type != msg_type::kSet) || (frame.payload.size() < kTestMsgLen)) {
        return reply_fail(reply, frame.msg_id, frame.type);
    }

    const uint8_t action = frame.payload[0];
    if (action == kTestActionSet) {
        if (!alarm_source.set_override(frame.payload[1], frame.payload[2])) {
            return reply_fail(reply, frame.msg_id, frame.type);
        }
        return reply_ok(reply, frame.msg_id, frame.type);
    }
    if (action == kTestActionClear) {
        alarm_source.clear_override();
        return reply_ok(reply, frame.msg_id, frame.type);
    }
    return reply_fail(reply, frame.msg_id, frame.type);
}

/** 围栏本阶段预留：查询/设置均回失败（§5）。 */
DispatchResult handle_fence(const proto::FrameView &frame, Reply &reply) noexcept
{
    return reply_fail(reply, frame.msg_id, frame.type);
}

} // namespace

AlarmService::AlarmService(AlarmSource &alarm, const LinkManager &link, const DeviceInfo &info) noexcept
    : alarm_(alarm), link_(link), info_(info)
{}

bool AlarmService::build_status_push(uint16_t seq, uint32_t now_ms, Reply &reply) const noexcept
{
    reply.clear();
    const alarm::Status status = make_status(alarm_, link_, info_, seq, now_ms);
    std::array<uint8_t, alarm::kStatusLen> payload{};
    if (alarm::encode_status(status, payload) != payload.size()) {
        return false;
    }

    size_t len = 0;
    const Status result =
        proto::build_frame(reply.buf, alarm::kMsgStatus, proto::sender::kDevice, msg_type::kPush, payload, len);
    if (!is_ok(result)) {
        return false;
    }
    reply.len = len;
    return true;
}

std::span<const Route> AlarmService::routes() noexcept
{
    // 冻结头只暴露 build_status_push，而 0x40 查询应答需要 AlarmSource/LinkManager/DeviceInfo 三份引用。
    // 局部类定义在成员函数内，按 [class.access] 与成员函数具有相同访问权限，因此 static_cast 之后
    // 可以取到私有引用；每个 self 都来自 table()（RouteTable{this, ...}），类型必然是本服务实例。
    struct Handlers {
        static DispatchResult status_query(void *self, const proto::FrameView &frame, uint32_t now_ms,
                                           Reply &reply) noexcept
        {
            // 旧 dispatch_alarm：0x40 只接受 QUERY，其他 type 按角色无权处理（kDenied）。
            if (frame.type != msg_type::kQuery) {
                return DispatchResult::kDenied;
            }
            const auto *const service = static_cast<const AlarmService *>(self);
            return handle_status_query(service->alarm_, service->link_, service->info_, frame, now_ms, reply);
        }
        static DispatchResult heartbeat(void *self, const proto::FrameView &frame, uint32_t now_ms,
                                        Reply &reply) noexcept
        {
            static_cast<void>(self);
            static_cast<void>(now_ms);
            // 旧 dispatch_alarm：0x41 只接受 PUSH，其他 type 返回 kDenied。
            if (frame.type != msg_type::kPush) {
                return DispatchResult::kDenied;
            }
            return handle_heartbeat(frame, reply);
        }
        static DispatchResult test_override(void *self, const proto::FrameView &frame, uint32_t now_ms,
                                            Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            auto *const service = static_cast<AlarmService *>(self);
            return handle_test_override(service->alarm_, frame, reply);
        }
        static DispatchResult fence(void *self, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            static_cast<void>(self);
            static_cast<void>(now_ms);
            return handle_fence(frame, reply);
        }
    };

    static constexpr std::array<Route, 4> kRoutes = {{
        {alarm::kMsgStatus, kRoleAlarm, &Handlers::status_query},
        {alarm::kMsgHeartbeat, kRoleAlarm, &Handlers::heartbeat},
        {alarm::kMsgTest, kRolePhone, &Handlers::test_override},
        {alarm::kMsgFence, kRolePhone, &Handlers::fence},
    }};
    return kRoutes;
}

} // namespace rtk::core::services
