// 报警器扩展消息 0x40/0x41 编解码的 C++ 外观，实现委托给 C 接口 DevProtoAlarm.h。线路格式见
// docs/protocol/phone_protocol.md §5。 本头文件同时供报警器固件复用。线程安全：是（无状态函数）。ISR 安全：是。
// 冻结接口（ADR-0003）。实现方不得修改。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "DevProtoAlarm.h"

namespace proto::alarm {

inline constexpr uint16_t kMsgStatus = 0x0040;
inline constexpr uint16_t kMsgHeartbeat = 0x0041;
inline constexpr uint16_t kMsgTest = 0x0042;
inline constexpr uint16_t kMsgFence = 0x0043;
inline constexpr uint16_t kMsgPair = 0x0044;

inline constexpr size_t kStatusLen = 16;
inline constexpr size_t kHeartbeatLen = 8;
inline constexpr uint32_t kPushPeriodMs = 1000;

// 状态与标志是线路值，保持 uint8_t 常量。
inline constexpr uint8_t kStateNormal = 0x00;
inline constexpr uint8_t kStateAlert = 0x01;
inline constexpr uint8_t kStateNoFix = 0x02;
inline constexpr uint8_t kStateUnknown = 0xFF;
inline constexpr uint8_t kLevelMax = 3;
inline constexpr uint8_t kValueUnknown = 0xFF;
inline constexpr uint8_t kFlagPhoneOnline = 0x01;
inline constexpr uint8_t kFlagTestOverride = 0x02;

/** 0x40 报警状态字段（小端）；纯数据，可平凡拷贝。 */
struct Status {
    uint16_t seq = 0;
    uint8_t state = kStateUnknown;
    uint8_t level = 0;
    uint8_t fix_quality = kValueUnknown;
    uint8_t battery_pct = kValueUnknown;
    uint8_t flags = 0;
    uint32_t uptime_ms = 0;
};

/** 0x41 心跳字段；纯数据，可平凡拷贝。 */
struct Heartbeat {
    uint16_t seq = 0;
    uint8_t battery_pct = kValueUnknown;
    uint8_t local_state = 0;
};

/** 按 §5 布局写 16 字节，返回写入字节数；out 不足 16 字节返回 0 且不写入。 */
[[nodiscard]] size_t encode_status(const Status &status, std::span<uint8_t> out) noexcept;

/** data 不足 8 字节返回 nullopt；多余字节忽略。 */
[[nodiscard]] std::optional<Heartbeat> decode_heartbeat(std::span<const uint8_t> data) noexcept;

/** 报警器侧复用：编码 0x41 心跳 8 字节；out 不足返回 0。 */
[[nodiscard]] size_t encode_heartbeat(const Heartbeat &heartbeat, std::span<uint8_t> out) noexcept;

/** 报警器侧复用：解码 0x40 状态；data 不足 16 字节返回 nullopt。 */
[[nodiscard]] std::optional<Status> decode_status(std::span<const uint8_t> data) noexcept;

static_assert(kStatusLen == DEV_PROTO_ALARM_STATUS_LEN);
static_assert(kHeartbeatLen == DEV_PROTO_ALARM_HEARTBEAT_LEN);
static_assert(kMsgStatus == DEV_PROTO_ALARM_MSG_STATUS);
static_assert(kMsgHeartbeat == DEV_PROTO_ALARM_MSG_HEARTBEAT);
static_assert(kPushPeriodMs == DEV_PROTO_ALARM_PUSH_PERIOD_MS);

} // namespace proto::alarm
