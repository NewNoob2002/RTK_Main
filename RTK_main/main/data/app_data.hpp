// DataCenter 主题 Slot 实例声明与跨任务初始化入口（拆出共享类型至 def/*_def.hpp）。
#pragma once

#include <cstdint>

#include "app/slot.hpp"
#include "app_config.hpp"
#include "def/link_def.hpp"
#include "def/rtcm_def.hpp"
#include "def/storage_def.hpp"
#include "def/system_def.hpp"
#include "modules/rtcm/rtcm_forwarder.hpp"
#include "rtk/os/executor.hpp"

namespace rtk::app::data {
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
} // namespace rtk::app::data
