// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "rtk/os/spin_lock.hpp"
#include "app_data.hpp"

namespace rtk::app::data {
namespace {
constinit portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
}
constinit ::app::Slot<LinkStatus> s_link_status{};
constinit ::app::Slot<Sessions> s_sessions{};
constinit ::app::Slot<AlarmState> s_alarm_state{};
constinit ::app::Slot<Heartbeat> s_heartbeat{};
constinit ::app::Slot<LinkStats> s_link_stats{};
constinit ::app::Slot<RtcmStats> s_rtcm_stats{};
constinit ::app::Slot<DemuxStats> s_demux_stats{};
constinit ::app::Slot<StoreStatus> s_store_status{};
constinit ::app::Slot<os::ExecStats> s_exec_stats{};
constinit ::app::Slot<SystemStatus> s_system_status{};
constinit ::app::Slot<SystemMode> s_system_mode{};
constinit ::app::Slot<RtcmBatch> s_rtcm_batch{};
constinit ::app::Slot<uint32_t> s_rtcm_reset{};

void init() noexcept
{
    const auto lock = os::spin_lock_ops(s_mux);
    kLinkStatus.bind_lock(lock);
    kSessions.bind_lock(lock);
    kAlarmState.bind_lock(lock);
    kHeartbeat.bind_lock(lock);
    kLinkStats.bind_lock(lock);
    kRtcmStats.bind_lock(lock);
    kDemuxStats.bind_lock(lock);
    kStoreStatus.bind_lock(lock);
    kExecStats.bind_lock(lock);
    kSystemStatus.bind_lock(lock);
    kSystemMode.bind_lock(lock);
    kRtcmBatch.bind_lock(lock);
    kRtcmReset.bind_lock(lock);
}
} // namespace rtk::app::data
