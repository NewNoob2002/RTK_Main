// 链路模块跨模块共享类型与命令动词定义（拆自原 app_data.hpp 与 app_ids.hpp）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "app_config.hpp"
#include "core/link_service.hpp"

namespace app {

/** 链路模块命令动词（由 fw::Command 的 arg 携带）。 */
/** kWhitelistAdd/kWhitelistDel 的 args 为 6 字节 MAC（core::kAddrLen）。 */
enum class LinkVerb : uint8_t { kAlarmSet, kAlarmClear, kPair, kWhitelistClear, kWhitelistAdd, kWhitelistDel };

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t verb(LinkVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** 单个会话的快照，供 status 展示；纯数据。 */
struct SessionSnapshot {
    bool used = false;
    core::SessionHandle handle = 0;
    core::Role role = core::Role::kNone;
    core::BtAddr addr{};
};
/** 链路策略、配对状态与白名单的快照；经 DataCenter 共享。 */
struct LinkStatus {
    core::Policy policy = policy_from_config();
    bool pairing = false;
    size_t whitelist_count = 0;
    std::array<core::BtAddr, kMaxAlarms> whitelist{};
    uint32_t snapshot_ms = 0;
};
/** 当前会话表的快照。 */
struct Sessions {
    size_t count = 0;
    std::array<SessionSnapshot, kMaxSessions> entries{};
};
/** 报警状态快照（测试覆盖优先）。 */
struct AlarmState {
    uint8_t state = 0;
    uint8_t level = 0;
    bool override = false;
};
/** 最近一次收到的心跳；present=false 表示尚无。 */
struct Heartbeat {
    bool present = false;
    proto::alarm::Heartbeat value{};
};
/** 链路服务统计 + 异步发送统计；纯数据快照。 */
struct LinkStats : core::LinkStats {
    uint32_t parser_crc_errors = 0;
    uint32_t parser_length_errors = 0;
    uint32_t tx_sent = 0;       // 异步发送任务底层 write 成功
    uint32_t tx_failed = 0;     // 异步发送任务底层 write 失败
    uint32_t tx_queue_full = 0; // 发送队列满被丢弃的回复
    uint32_t tx_max_depth = 0;  // 发送队列最大深度
    uint32_t tx_stack_free = 0; // async_tx 栈剩余字节（未启动为 0）
};

} // namespace app
