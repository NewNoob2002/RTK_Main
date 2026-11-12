// 报警器扩展消息 0x40/0x41 的 C++ 外观（ADR-0003）。
// 协议逻辑全部在 rtk_proto_alarm.c；本文件只做结构体逐字段转换与 span 包装。
// 线程安全：是（无状态函数）。ISR 安全：是。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk/proto/alarm_codec.hpp"

namespace rtk::proto::alarm {

namespace {

rtk_proto_alarm_status_t to_c(const Status &status) noexcept
{
    rtk_proto_alarm_status_t c_status{};
    c_status.seq = status.seq;
    c_status.state = status.state;
    c_status.level = status.level;
    c_status.fix_quality = status.fix_quality;
    c_status.battery_pct = status.battery_pct;
    c_status.flags = status.flags;
    c_status.uptime_ms = status.uptime_ms;
    return c_status;
}

Status from_c(const rtk_proto_alarm_status_t &c_status) noexcept
{
    Status status;
    status.seq = c_status.seq;
    status.state = c_status.state;
    status.level = c_status.level;
    status.fix_quality = c_status.fix_quality;
    status.battery_pct = c_status.battery_pct;
    status.flags = c_status.flags;
    status.uptime_ms = c_status.uptime_ms;
    return status;
}

rtk_proto_alarm_heartbeat_t to_c(const Heartbeat &heartbeat) noexcept
{
    rtk_proto_alarm_heartbeat_t c_heartbeat{};
    c_heartbeat.seq = heartbeat.seq;
    c_heartbeat.battery_pct = heartbeat.battery_pct;
    c_heartbeat.local_state = heartbeat.local_state;
    return c_heartbeat;
}

Heartbeat from_c(const rtk_proto_alarm_heartbeat_t &c_heartbeat) noexcept
{
    Heartbeat heartbeat;
    heartbeat.seq = c_heartbeat.seq;
    heartbeat.battery_pct = c_heartbeat.battery_pct;
    heartbeat.local_state = c_heartbeat.local_state;
    return heartbeat;
}

} // namespace

size_t encode_status(const Status &status, std::span<uint8_t> out) noexcept
{
    const rtk_proto_alarm_status_t c_status = to_c(status);
    return rtk_proto_alarm_encode_status(&c_status, out.data(), out.size());
}

std::optional<Status> decode_status(std::span<const uint8_t> data) noexcept
{
    rtk_proto_alarm_status_t c_status{};
    if (!rtk_proto_alarm_decode_status(data.data(), data.size(), &c_status)) {
        return std::nullopt;
    }
    return from_c(c_status);
}

size_t encode_heartbeat(const Heartbeat &heartbeat, std::span<uint8_t> out) noexcept
{
    const rtk_proto_alarm_heartbeat_t c_heartbeat = to_c(heartbeat);
    return rtk_proto_alarm_encode_heartbeat(&c_heartbeat, out.data(), out.size());
}

std::optional<Heartbeat> decode_heartbeat(std::span<const uint8_t> data) noexcept
{
    rtk_proto_alarm_heartbeat_t c_heartbeat{};
    if (!rtk_proto_alarm_decode_heartbeat(data.data(), data.size(), &c_heartbeat)) {
        return std::nullopt;
    }
    return from_c(c_heartbeat);
}

} // namespace rtk::proto::alarm
