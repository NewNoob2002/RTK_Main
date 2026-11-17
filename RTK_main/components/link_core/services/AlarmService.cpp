// AlarmService 实现（ADR-0007 §3.3）：报警相关指令 0x40/0x41/0x42/0x43 与 0x40 主动推送帧。
// 线程安全：否，只由链路任务串行调用；非 ISR、不阻塞。
// 堆：不分配（应答复用调用方的 Reply 缓冲，状态帧用栈上定长数组）。

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

// 通用结果码（phone_protocol.md §3：0x01 成功，0x02 失败）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

/**
 * @brief 组应答帧：sender 固定为 DEV，type 经 responseType 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * @param[out] reply 复用缓冲；成功时写入帧并设置 len，失败时 len 置 0。
 * @param msgId 请求的 msg_id，原样回填。
 * @param reqType 请求的 type，用于映射应答 type。
 * @param payload 应答负载。
 * @retval DispatchResult::kReply 已组帧。
 * @retval DispatchResult::kNoReply 组帧失败（缓冲不足等），reply.len 为 0。
 * @note 不分配、不加锁；非 ISR 可调（纯缓冲写入）。
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
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuild(reply, msgId, reqType, payload);
}

/** @brief 成功应答（结果码 0x01 0x01）的快捷封装。
 *  @param[out] reply 复用缓冲。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyOk(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultOk);
}

/** @brief 失败应答（结果码 0x02 0x02）的快捷封装。
 *  @param[out] reply 复用缓冲。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
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

// 0x40 查询应答的 seq 固定为 0（查询不入推送序号；推送用递增 seq）。
constexpr uint16_t kStatusQuerySeq = 0;

/** @brief 组报警状态结构：状态/等级来自 AlarmSource，手机在线与测试覆盖写入 flags（§5）。
 *  @param alarmSource 报警状态来源（覆盖优先）。
 *  @param link 用于判断 PHONE 角色是否在线。
 *  @param info 设备信息，取定位质量与电量。
 *  @param seq 帧序号；查询应答固定为 0，推送用递增序号。
 *  @param nowMs 当前时间（ms），写入 uptimeMs。
 *  @return 填充好的状态结构（按值返回，不分配）。 */
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

/** @brief 处理 0x40 查询：组当前状态并按查询的 msgId/type 回一帧。
 *  @param alarmSource 报警状态来源。
 *  @param link 会话状态机（手机在线标志）。
 *  @param info 设备信息。
 *  @param frame 已校验的请求帧（ALARM、type=QUERY）。
 *  @param nowMs 当前时间（ms）。
 *  @param[out] reply 应答缓冲。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 状态编码失败（负载长度不符）。 */
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

/** @brief 处理 0x41 心跳：解码并写入 reply.heartbeat，供链路层记录。
 *  @param frame 请求帧（ALARM、type=PUSH）。
 *  @param[out] reply 应答缓冲；解码成功时写入心跳，其余字段不动。
 *  @return 恒为 DispatchResult::kNoReply（协议规定心跳不回复）。
 *  @note 负载解码失败时静默忽略，不影响连接。 */
DispatchResult handleHeartbeat(const proto::FrameView &frame, Reply &reply) noexcept
{
    const std::optional<alarm::Heartbeat> heartbeat = alarm::decodeHeartbeat(frame.payload);
    if (heartbeat.has_value()) {
        reply.heartbeat = heartbeat;
    }
    // 心跳/HELLO 按协议不回复（§5）。
    return DispatchResult::kNoReply;
}

/** @brief 处理 0x42 测试报警：action=set 设置覆盖，action=clear 清除，其他回失败。
 *  @param[out] alarmSource 写入测试覆盖。
 *  @param frame 请求帧（PHONE、type=SET，负载至少 3 字节）。
 *  @param[out] reply 应答缓冲。
 *  @retval DispatchResult::kReply 已组成功或失败应答（type/长度/参数非法时回失败码）。
 *  @retval DispatchResult::kNoReply 组帧失败。
 *  @note setOverride 拒绝非法状态/等级时按协议回失败，不修改现有覆盖。 */
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

/** @brief 电子围栏尚未接入：查询/设置一律回失败（§5）。
 *  @param frame 请求帧，仅取 msgId/type 用于组应答。
 *  @param[out] reply 应答缓冲。
 *  @retval DispatchResult::kReply 已组失败应答。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
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
            // 0x40 只接受 QUERY，其他 type 按角色无权处理（kDenied）。
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
            // 0x41 只接受 PUSH，其他 type 返回 kDenied。
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
