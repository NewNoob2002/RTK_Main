// 链路模块跨模块共享类型与命令动词定义（拆自原 app_data.hpp 与 app_ids.hpp）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "app_config.hpp"
#include "rtk/core/link_service.hpp"

namespace rtk::app {

enum class LinkVerb : uint8_t { kAlarmSet, kAlarmClear, kPair, kWhitelistClear };

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t verb(LinkVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

struct SessionSnapshot {
    bool used = false;
    core::SessionHandle handle = 0;
    core::Role role = core::Role::kNone;
    core::BtAddr addr{};
};
struct LinkStatus {
    core::Policy policy = policy_from_config();
    bool pairing = false;
    size_t whitelist_count = 0;
    std::array<core::BtAddr, kMaxAlarms> whitelist{};
    uint32_t snapshot_ms = 0;
};
struct Sessions {
    size_t count = 0;
    std::array<SessionSnapshot, kMaxSessions> entries{};
};
struct AlarmState {
    uint8_t state = 0;
    uint8_t level = 0;
    bool override = false;
};
struct Heartbeat {
    bool present = false;
    proto::alarm::Heartbeat value{};
};
struct LinkStats : core::LinkStats {
    uint32_t parser_crc_errors = 0;
    uint32_t parser_length_errors = 0;
    uint32_t tx_sent = 0;       // 异步发送任务底层 write 成功
    uint32_t tx_failed = 0;     // 异步发送任务底层 write 失败
    uint32_t tx_queue_full = 0; // 发送队列满被丢弃的回复
    uint32_t tx_max_depth = 0;  // 发送队列最大深度
    uint32_t tx_stack_free = 0; // rtk_tx 栈剩余字节（未启动为 0）
};

} // namespace rtk::app
