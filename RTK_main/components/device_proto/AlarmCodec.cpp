// 报警器扩展消息 0x40/0x41 的 C++ 外观（ADR-0003）。
// 协议逻辑全部在 DevProtoAlarm.c；本文件只做结构体逐字段转换与 span 包装。
// 线程安全：是（无状态函数）。ISR 安全：是。

#include "proto/AlarmCodec.hpp"

namespace proto::alarm {

namespace {

dev_proto_alarm_status_t toC(const Status &status) noexcept
{
    dev_proto_alarm_status_t cStatus{};
    cStatus.seq = status.seq;
    cStatus.state = status.state;
    cStatus.level = status.level;
    cStatus.fix_quality = status.fixQuality;
    cStatus.battery_pct = status.batteryPct;
    cStatus.flags = status.flags;
    cStatus.uptime_ms = status.uptimeMs;
    return cStatus;
}

Status fromC(const dev_proto_alarm_status_t &cStatus) noexcept
{
    Status status;
    status.seq = cStatus.seq;
    status.state = cStatus.state;
    status.level = cStatus.level;
    status.fixQuality = cStatus.fix_quality;
    status.batteryPct = cStatus.battery_pct;
    status.flags = cStatus.flags;
    status.uptimeMs = cStatus.uptime_ms;
    return status;
}

dev_proto_alarm_heartbeat_t toC(const Heartbeat &heartbeat) noexcept
{
    dev_proto_alarm_heartbeat_t cHeartbeat{};
    cHeartbeat.seq = heartbeat.seq;
    cHeartbeat.battery_pct = heartbeat.batteryPct;
    cHeartbeat.local_state = heartbeat.localState;
    return cHeartbeat;
}

Heartbeat fromC(const dev_proto_alarm_heartbeat_t &cHeartbeat) noexcept
{
    Heartbeat heartbeat;
    heartbeat.seq = cHeartbeat.seq;
    heartbeat.batteryPct = cHeartbeat.battery_pct;
    heartbeat.localState = cHeartbeat.local_state;
    return heartbeat;
}

} // namespace

size_t encodeStatus(const Status &status, std::span<uint8_t> out) noexcept
{
    const dev_proto_alarm_status_t cStatus = toC(status);
    return dev_proto_alarm_encode_status(&cStatus, out.data(), out.size());
}

std::optional<Status> decodeStatus(std::span<const uint8_t> data) noexcept
{
    dev_proto_alarm_status_t cStatus{};
    if (!dev_proto_alarm_decode_status(data.data(), data.size(), &cStatus)) {
        return std::nullopt;
    }
    return fromC(cStatus);
}

size_t encodeHeartbeat(const Heartbeat &heartbeat, std::span<uint8_t> out) noexcept
{
    const dev_proto_alarm_heartbeat_t cHeartbeat = toC(heartbeat);
    return dev_proto_alarm_encode_heartbeat(&cHeartbeat, out.data(), out.size());
}

std::optional<Heartbeat> decodeHeartbeat(std::span<const uint8_t> data) noexcept
{
    dev_proto_alarm_heartbeat_t cHeartbeat{};
    if (!dev_proto_alarm_decode_heartbeat(data.data(), data.size(), &cHeartbeat)) {
        return std::nullopt;
    }
    return fromC(cHeartbeat);
}

} // namespace proto::alarm
