// 按角色分发手机/报警器命令并生成应答帧（phone_protocol.md §3–§5）。
// 所有应答 payload 在栈上构造，再交给 proto::build_frame；不使用堆。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk/core/command_dispatcher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "rtk/proto/bytes.hpp"

namespace rtk::core {

namespace {

namespace proto = rtk::proto;
namespace alarm = rtk::proto::alarm;
namespace msg_type = rtk::proto::msg_type;

// §4 命令 ID。
constexpr uint16_t kIdDevInfo = 0x0001;
constexpr uint16_t kIdDevState = 0x0002;
constexpr uint16_t kIdBattery = 0x0003;
constexpr uint16_t kIdGga = 0x0004;
constexpr uint16_t kIdSat = 0x0005;
constexpr uint16_t kIdSatTrack = 0x0006;
constexpr uint16_t kIdWorkMode = 0x0007;
constexpr uint16_t kIdCom = 0x000D;
constexpr uint16_t kIdRadio = 0x0014;
constexpr uint16_t kIdLog = 0x0015;

// 通用结果码（§3）。
constexpr uint8_t kResultOk = 0x01;
constexpr uint8_t kResultFail = 0x02;

// 0x42 测试报警 payload。
constexpr size_t kTestMsgLen = 3;
constexpr uint8_t kTestActionSet = 0x01;
constexpr uint8_t kTestActionClear = 0x00;

// 0x0D COM 口布局（§4）。
constexpr size_t kComOfsBaud = 4;
constexpr size_t kComOfsFlagHigh = 27;
constexpr uint16_t kComParam218 = 218;

// 0x03 电池/存储布局（§4）。
constexpr size_t kBatteryOfsTemp = 12;

// 0x01 设备信息：fw_ver 与 board 之间的保留区终点（§4）。
constexpr size_t kDevInfoBoardOfs = 56;

// 0x44 查询应答：1 字节数量 + n×6 字节 MAC。
constexpr size_t kPairOfsCount = 0;
constexpr size_t kPairOfsFirst = 1;
constexpr size_t kPairQueryMax = kPairOfsFirst + (kMaxAlarms * kAddrLen);

// 0x02 设备状态常量（§4）。
constexpr std::array<uint8_t, kDevStateLen> kDevStatePayload = {
    0x03, 0x01, 0x81, 0x83, 0x81, 0x00, 0x00, 0x00, 0x00, 0x81, 0x81, 0x81, 0x00, 0x01,
};

// 以指针传递上下文：clang-tidy 禁止引用数据成员，且这些对象生命周期由 LinkService 保证。
struct Context {
    AlarmSource *alarm;
    LinkManager *link;
    DeviceInfo *info;
    std::span<uint8_t> work_mode;
    std::span<uint8_t> radio_cfg;
    std::span<uint8_t> log_cfg;
};

DispatchResult reply_build(Reply &reply, uint16_t msg_id, uint8_t req_type, std::span<const uint8_t> payload) noexcept
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

DispatchResult reply_result(Reply &reply, uint16_t msg_id, uint8_t req_type, uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return reply_build(reply, msg_id, req_type, payload);
}

DispatchResult reply_ok(Reply &reply, uint16_t msg_id, uint8_t req_type) noexcept
{
    return reply_result(reply, msg_id, req_type, kResultOk);
}

DispatchResult reply_fail(Reply &reply, uint16_t msg_id, uint8_t req_type) noexcept
{
    return reply_result(reply, msg_id, req_type, kResultFail);
}

DispatchResult handle_dev_info(const DeviceInfo &info, const proto::FrameView &frame, Reply &reply) noexcept
{
    std::array<uint8_t, kDevInfoLen> payload{};
    proto::ByteWriter writer(payload);
    writer.fixed_str(info.product.view(), info.product.capacity());
    writer.fixed_str(info.sn.view(), info.sn.capacity());
    writer.fixed_str(info.hw_ver.view(), info.hw_ver.capacity());
    writer.fixed_str(info.fw_ver.view(), info.fw_ver.capacity());
    // §4：fw_ver 与 board 之间有 8 B 保留区。
    writer.zeros(kDevInfoBoardOfs - writer.size());
    writer.fixed_str(info.board.view(), info.board.capacity());
    writer.fixed_str(info.board_sn.view(), info.board_sn.capacity());
    writer.fixed_str(info.board_fw.view(), info.board_fw.capacity());
    writer.fixed_str(info.radio_type.view(), info.radio_type.capacity());
    writer.fixed_str(info.radio_fw.view(), info.radio_fw.capacity());
    // 字段总宽 120 B，剩余 16 B 保持 0（§4）。
    return reply_build(reply, frame.msg_id, frame.type, payload);
}

DispatchResult handle_dev_state(const proto::FrameView &frame, Reply &reply) noexcept
{
    return reply_build(reply, frame.msg_id, frame.type, kDevStatePayload);
}

DispatchResult handle_battery(const DeviceInfo &info, const proto::FrameView &frame, Reply &reply) noexcept
{
    std::array<uint8_t, kBatteryLen> payload{};
    proto::ByteWriter writer(payload);
    writer.u8(info.on_battery ? 1U : 0U);
    writer.u8(0);
    writer.u8(info.battery_pct);
    writer.zeros(kBatteryOfsTemp - writer.size());
    // int8_t 的补码位模式按协议直接写入（-5 → 0xFB）。
    writer.u8(static_cast<uint8_t>(info.battery_temp_c));
    writer.zeros(kBatteryLen - writer.size());
    return reply_build(reply, frame.msg_id, frame.type, payload);
}

DispatchResult handle_ok(const proto::FrameView &frame, Reply &reply) noexcept
{
    return reply_ok(reply, frame.msg_id, frame.type);
}

DispatchResult handle_sat_track(const proto::FrameView &frame, Reply &reply) noexcept
{
    if (frame.type != msg_type::kQuery) {
        return reply_ok(reply, frame.msg_id, frame.type);
    }
    const std::array<uint8_t, kSatTrackLen> payload{};
    return reply_build(reply, frame.msg_id, frame.type, payload);
}

DispatchResult handle_work_mode(std::span<uint8_t> cfg, const proto::FrameView &frame, Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.size() < kWorkModeSetLen) {
            return reply_fail(reply, frame.msg_id, frame.type);
        }
        for (size_t i = 0; i < kWorkModeSetLen; ++i) {
            cfg[i] = frame.payload[i];
        }
        return reply_ok(reply, frame.msg_id, frame.type);
    }
    return reply_build(reply, frame.msg_id, frame.type, cfg);
}

DispatchResult handle_com(const DeviceInfo &info, const proto::FrameView &frame, Reply &reply) noexcept
{
    // 5.4 行为：0x0D 设置不回复（§4）。
    if (frame.type != msg_type::kQuery) {
        return DispatchResult::kNoReply;
    }

    std::array<uint8_t, kComLen> payload{};
    proto::ByteWriter writer(payload);
    writer.u8(kResultOk);
    writer.u8(kResultOk);
    writer.zeros(kComOfsBaud - writer.size());
    writer.u32(info.com_baud);
    writer.u8(kResultOk);
    writer.zeros(kComOfsFlagHigh - writer.size());
    writer.u8(kResultOk);
    writer.u16(kComParam218);
    writer.zeros(kComLen - writer.size());
    return reply_build(reply, frame.msg_id, frame.type, payload);
}

DispatchResult handle_radio(std::span<uint8_t> cfg, const proto::FrameView &frame, Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.size() < kRadioSetMinLen) {
            return reply_fail(reply, frame.msg_id, frame.type);
        }
        const size_t count = (frame.payload.size() < kRadioLen) ? frame.payload.size() : kRadioLen;
        for (size_t i = 0; i < count; ++i) {
            cfg[i] = frame.payload[i];
        }
        // payload 未覆盖 [17] 时保持原值（§4）。
        return reply_ok(reply, frame.msg_id, frame.type);
    }
    return reply_build(reply, frame.msg_id, frame.type, cfg);
}

DispatchResult handle_log(std::span<uint8_t> cfg, const proto::FrameView &frame, Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.size() < kLogLen) {
            return reply_fail(reply, frame.msg_id, frame.type);
        }
        for (size_t i = 0; i < kLogLen; ++i) {
            cfg[i] = frame.payload[i];
        }
        return reply_ok(reply, frame.msg_id, frame.type);
    }
    return reply_build(reply, frame.msg_id, frame.type, cfg);
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

DispatchResult handle_fence(const proto::FrameView &frame, Reply &reply) noexcept
{
    // 围栏本阶段预留：查询/设置均回失败（§5）。
    return reply_fail(reply, frame.msg_id, frame.type);
}

DispatchResult handle_pair(LinkManager &link, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.empty()) {
            return reply_fail(reply, frame.msg_id, frame.type);
        }
        const uint8_t action = frame.payload[0];
        if (action == kTestActionSet) {
            link.pair_open(now_ms);
            return reply_ok(reply, frame.msg_id, frame.type);
        }
        if (action == kTestActionClear) {
            link.whitelist_clear();
            reply.whitelist_changed = true;
            return reply_ok(reply, frame.msg_id, frame.type);
        }
        return reply_fail(reply, frame.msg_id, frame.type);
    }

    std::array<uint8_t, kPairQueryMax> payload{};
    const std::span<const BtAddr> list = link.whitelist();
    const size_t count = (list.size() < kMaxAlarms) ? list.size() : kMaxAlarms;
    // count ≤ kMaxAlarms(4)，uint8_t 截断安全。
    payload[kPairOfsCount] = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < kAddrLen; ++j) {
            payload[kPairOfsFirst + (i * kAddrLen) + j] = list[i].bytes[j];
        }
    }
    const size_t len = kPairOfsFirst + (count * kAddrLen);
    return reply_build(reply, frame.msg_id, frame.type, std::span<const uint8_t>(payload).first(len));
}

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

DispatchResult handle_status_query(const Context &ctx, const proto::FrameView &frame, uint32_t now_ms,
                                   Reply &reply) noexcept
{
    const alarm::Status status = make_status(*ctx.alarm, *ctx.link, *ctx.info, 0, now_ms);
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

bool is_known_id(uint16_t msg_id) noexcept
{
    switch (msg_id) {
    case kIdDevInfo:
    case kIdDevState:
    case kIdBattery:
    case kIdGga:
    case kIdSat:
    case kIdSatTrack:
    case kIdWorkMode:
    case kIdCom:
    case kIdRadio:
    case kIdLog:
    case alarm::kMsgStatus:
    case alarm::kMsgHeartbeat:
    case alarm::kMsgTest:
    case alarm::kMsgFence:
    case alarm::kMsgPair:
        return true;
    default:
        return false;
    }
}

bool is_phone_command(uint16_t msg_id) noexcept
{
    switch (msg_id) {
    case kIdDevInfo:
    case kIdDevState:
    case kIdBattery:
    case kIdGga:
    case kIdSat:
    case kIdSatTrack:
    case kIdWorkMode:
    case kIdCom:
    case kIdRadio:
    case kIdLog:
    case alarm::kMsgTest:
    case alarm::kMsgFence:
    case alarm::kMsgPair:
        return true;
    default:
        return false;
    }
}

DispatchResult dispatch_phone(const Context &ctx, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
{
    switch (frame.msg_id) {
    case kIdDevInfo:
        return handle_dev_info(*ctx.info, frame, reply);
    case kIdDevState:
        return handle_dev_state(frame, reply);
    case kIdBattery:
        return handle_battery(*ctx.info, frame, reply);
    case kIdGga:
    case kIdSat:
        return handle_ok(frame, reply);
    case kIdSatTrack:
        return handle_sat_track(frame, reply);
    case kIdWorkMode:
        return handle_work_mode(ctx.work_mode, frame, reply);
    case kIdCom:
        return handle_com(*ctx.info, frame, reply);
    case kIdRadio:
        return handle_radio(ctx.radio_cfg, frame, reply);
    case kIdLog:
        return handle_log(ctx.log_cfg, frame, reply);
    case alarm::kMsgTest:
        return handle_test_override(*ctx.alarm, frame, reply);
    case alarm::kMsgFence:
        return handle_fence(frame, reply);
    case alarm::kMsgPair:
        return handle_pair(*ctx.link, frame, now_ms, reply);
    default:
        return DispatchResult::kNoReply;
    }
}

DispatchResult dispatch_alarm(const Context &ctx, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
{
    if ((frame.msg_id == alarm::kMsgHeartbeat) && (frame.type == msg_type::kPush)) {
        return handle_heartbeat(frame, reply);
    }
    if ((frame.msg_id == alarm::kMsgStatus) && (frame.type == msg_type::kQuery)) {
        return handle_status_query(ctx, frame, now_ms, reply);
    }
    return DispatchResult::kDenied;
}

} // namespace

CommandDispatcher::CommandDispatcher(AlarmSource &alarm, LinkManager &link) noexcept : alarm_(alarm), link_(link)
{
    info_.product.assign("E1-Lite");
    info_.hw_ver.assign("V1.1.0");
    info_.board.assign("UM980");
    info_.radio_type.assign("5002D");
    work_mode_.fill(0);
    radio_cfg_.fill(0);
    radio_cfg_[kRadioModeOffset] = kRadioModeDefault;
    log_cfg_.fill(0);
}

DispatchResult CommandDispatcher::handle(Role role, const proto::FrameView &frame, uint32_t now_ms,
                                         Reply &reply) noexcept
{
    reply.clear();
    // 未知 ID 与 5.4 保留 ID 一律不回复，先于角色检查。
    if (!is_known_id(frame.msg_id)) {
        return DispatchResult::kNoReply;
    }

    const Context ctx{&alarm_, &link_, &info_, work_mode_, radio_cfg_, log_cfg_};
    switch (role) {
    case Role::kPhone:
        if (!is_phone_command(frame.msg_id)) {
            return DispatchResult::kDenied;
        }
        return dispatch_phone(ctx, frame, now_ms, reply);
    case Role::kAlarm:
        return dispatch_alarm(ctx, frame, now_ms, reply);
    default:
        return DispatchResult::kDenied;
    }
}

bool CommandDispatcher::build_status_push(uint16_t seq, uint32_t now_ms, Reply &reply) const noexcept
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

} // namespace rtk::core
