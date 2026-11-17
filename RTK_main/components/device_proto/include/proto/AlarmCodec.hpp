// 报警器扩展消息 0x40/0x41 编解码的 C++ 外观（冻结，ADR-0003），实现委托给 DevProtoAlarm.h，
// 同时供报警器固件复用。无状态函数，任意任务可调用；无阻塞、无分配，ISR 安全。
// 不使用堆；线路格式见 docs/protocol/phone_protocol.md §5。
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
    uint8_t fixQuality = kValueUnknown;
    uint8_t batteryPct = kValueUnknown;
    uint8_t flags = 0;
    uint32_t uptimeMs = 0;
};

/** 0x41 心跳字段；纯数据，可平凡拷贝。 */
struct Heartbeat {
    uint16_t seq = 0;
    uint8_t batteryPct = kValueUnknown;
    uint8_t localState = 0;
};

/**
 * @brief  按 §5 布局编码 0x40 状态（16 字节，多字节字段小端）
 * @param  status: 待编码的状态字段
 * @param[out] out: 输出缓冲
 * @retval 16 成功，写入 16 字节
 * @retval 0  out 不足 16 字节，此时不写入
 * @note   无阻塞、无分配、ISR 安全
 */
[[nodiscard]] size_t encodeStatus(const Status &status, std::span<uint8_t> out) noexcept;

/**
 * @brief  按 §5 布局解码 0x41 心跳
 * @param  data: 收到的 payload（只读）
 * @retval 心跳字段；data 不足 8 字节时返回 std::nullopt
 * @note   多余字节忽略；无阻塞、无分配、ISR 安全
 */
[[nodiscard]] std::optional<Heartbeat> decodeHeartbeat(std::span<const uint8_t> data) noexcept;

/**
 * @brief  按 §5 布局编码 0x41 心跳（8 字节，报警器侧复用）
 * @param  heartbeat: 待编码的心跳字段
 * @param[out] out: 输出缓冲
 * @retval 8 成功，写入 8 字节
 * @retval 0 out 不足 8 字节，此时不写入
 * @note   无阻塞、无分配、ISR 安全
 */
[[nodiscard]] size_t encodeHeartbeat(const Heartbeat &heartbeat, std::span<uint8_t> out) noexcept;

/**
 * @brief  按 §5 布局解码 0x40 状态（报警器侧复用）
 * @param  data: 收到的 payload（只读）
 * @retval 状态字段；data 不足 16 字节时返回 std::nullopt
 * @note   多余字节忽略；无阻塞、无分配、ISR 安全
 */
[[nodiscard]] std::optional<Status> decodeStatus(std::span<const uint8_t> data) noexcept;

static_assert(kStatusLen == DEV_PROTO_ALARM_STATUS_LEN);
static_assert(kHeartbeatLen == DEV_PROTO_ALARM_HEARTBEAT_LEN);
static_assert(kMsgStatus == DEV_PROTO_ALARM_MSG_STATUS);
static_assert(kMsgHeartbeat == DEV_PROTO_ALARM_MSG_HEARTBEAT);
static_assert(kPushPeriodMs == DEV_PROTO_ALARM_PUSH_PERIOD_MS);

} // namespace proto::alarm
