// 报警器扩展消息 0x40/0x41 的 C++ 外观（ADR-0003）；协议逻辑在 DevProtoAlarm.c，本文件只做结构体转换。
// 无状态函数，任意任务可调用，非阻塞、无分配，ISR 安全。
// 不使用堆。

#include "proto/AlarmCodec.hpp"

namespace proto::alarm {

namespace {

/**
 * @brief  把 C++ Status 逐字段转成 C 结构
 * @param  status: 源字段
 * @retval 对应的 C 结构（未列出的字段由值初始化置 0）
 * @note   纯字段拷贝、无阻塞、ISR 安全
 */
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

/**
 * @brief  把 C 状态结构逐字段转成 C++ Status
 * @param  cStatus: 源结构
 * @retval 对应的 Status
 * @note   纯字段拷贝、无阻塞、ISR 安全
 */
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

/**
 * @brief  把 C++ Heartbeat 逐字段转成 C 结构
 * @param  heartbeat: 源字段
 * @retval 对应的 C 结构（未列出的字段由值初始化置 0）
 * @note   纯字段拷贝、无阻塞、ISR 安全
 */
dev_proto_alarm_heartbeat_t toC(const Heartbeat &heartbeat) noexcept
{
    dev_proto_alarm_heartbeat_t cHeartbeat{};
    cHeartbeat.seq = heartbeat.seq;
    cHeartbeat.battery_pct = heartbeat.batteryPct;
    cHeartbeat.local_state = heartbeat.localState;
    return cHeartbeat;
}

/**
 * @brief  把 C 心跳结构逐字段转成 C++ Heartbeat
 * @param  cHeartbeat: 源结构
 * @retval 对应的 Heartbeat
 * @note   纯字段拷贝、无阻塞、ISR 安全
 */
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
