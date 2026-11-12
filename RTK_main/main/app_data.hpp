// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once

#include <array>
#include <span>
#include "app/slot.hpp"
#include "app_ids.hpp"
#include "app_rtcm.hpp"
#include "esp_err.h"
#include "rtk/core/link_service.hpp"
#include "rtk/os/executor.hpp"
#include "rtk_store.h"

namespace rtk::app {
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
struct DemuxStats {
    uint32_t unknown_bytes = 0;
    uint32_t product_rejects = 0;
    uint32_t rtcm_rejects = 0;
};
struct StoreStatus {
    bool whitelist_pending = false;
    esp_err_t whitelist_error = ESP_OK;
    uint32_t failures = 0;
    uint32_t retry_ms = 0;
    esp_err_t sn_error = ESP_OK;
};
struct SystemStatus {
    bool bt_ready = false;
    bool link_running = false;
    std::array<char, RTK_STORE_SN_MAX + 1U> sn{};
    std::array<char, kDeviceNameBuf> device_name{};
    uint32_t free_heap = 0;
    uint32_t min_free_heap = 0;
};
enum class Mode : uint8_t { kNormal };
struct SystemMode {
    Mode mode = Mode::kNormal;
    uint32_t since_ms = 0;
};

// 仅 kLink 内部传递完整 RTCM 批次。view 指向 bt_link 的静态存储，
// 在 rtcm.poll 消费后、下一轮 wait 前才复用；其他任务只读取 kRtcmStats。
// 每会话遗留数据最多 1029 B，新输入每轮最多 256 B，故 2×1029+256 B 足够。
inline constexpr size_t kRtcmBatchBytes = kMaxSessions * 1029U + 256U;
struct RtcmBatch {
    std::span<const uint8_t> wire{};
    uint32_t rejected = 0;
};

namespace data {
// 主题标识是 constexpr 引用，实际可变存储按 AGENTS.md 使用 s_ 命名；每个主题仍只有一个 Slot。
extern ::app::Slot<LinkStatus> s_link_status;
extern ::app::Slot<Sessions> s_sessions;
extern ::app::Slot<AlarmState> s_alarm_state;
extern ::app::Slot<Heartbeat> s_heartbeat;
extern ::app::Slot<LinkStats> s_link_stats;
extern ::app::Slot<RtcmStats> s_rtcm_stats;
extern ::app::Slot<DemuxStats> s_demux_stats;
extern ::app::Slot<StoreStatus> s_store_status;
extern ::app::Slot<os::ExecStats> s_exec_stats;
extern ::app::Slot<SystemStatus> s_system_status;
extern ::app::Slot<SystemMode> s_system_mode;
extern ::app::Slot<RtcmBatch> s_rtcm_batch;
extern ::app::Slot<uint32_t> s_rtcm_reset;
inline constexpr auto &kLinkStatus = s_link_status;
inline constexpr auto &kSessions = s_sessions;
inline constexpr auto &kAlarmState = s_alarm_state;
inline constexpr auto &kHeartbeat = s_heartbeat;
inline constexpr auto &kLinkStats = s_link_stats;
inline constexpr auto &kRtcmStats = s_rtcm_stats;
inline constexpr auto &kDemuxStats = s_demux_stats;
inline constexpr auto &kStoreStatus = s_store_status;
inline constexpr auto &kExecStats = s_exec_stats;
inline constexpr auto &kSystemStatus = s_system_status;
inline constexpr auto &kSystemMode = s_system_mode;
inline constexpr auto &kRtcmBatch = s_rtcm_batch;
inline constexpr auto &kRtcmReset = s_rtcm_reset;
// 启动前单线程调用一次；非 ISR。随后 Slot 支持跨任务读取。
void init() noexcept;
} // namespace data
} // namespace rtk::app
