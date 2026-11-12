// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rtk::core {

using SessionHandle = uint32_t;

inline constexpr size_t kMaxSessions = 2; // 与 BR/EDR ACL 上限一致（CONFIG_BT_ACL_CONNECTIONS=2）
inline constexpr size_t kMaxAlarms = 4;   // 报警器白名单容量
inline constexpr size_t kAddrLen = 6;

struct BtAddr {
    std::array<uint8_t, kAddrLen> bytes{};
    friend constexpr bool operator==(const BtAddr &, const BtAddr &) = default;
};

enum class Role : uint8_t {
    kNone = 0, // 句柄不存在 / 被拒绝
    kPhone,
    kAlarm,
    kPending, // 已有手机时新接入的未知设备，等待 HELLO
};

enum class Policy : uint8_t {
    kConcurrent = 0, // 手机与报警器可同时在线
    kPhonePreempt,   // 手机接入时踢掉报警器；手机在线时拒绝报警器
};

} // namespace rtk::core
