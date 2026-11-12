// 应用上下文拥有的共享主题与跨任务锁。
// init 仅启动前调用；绑定后各 Slot 支持跨任务读写，非 ISR。
#pragma once

#include "app/slot.hpp"
#include "app_config.hpp"
#include "def/link_def.hpp"
#include "def/rtcm_def.hpp"
#include "def/storage_def.hpp"
#include "def/system_def.hpp"
#include "modules/rtcm/rtcm_forwarder.hpp"
#include "rtk/os/spin_lock.hpp"

namespace rtk::app::data {
struct DataCenter {
    /** 启动前单线程绑定全部主题的共享锁；不阻塞，非 ISR，无错误返回。 */
    void init() noexcept;

    ::app::Slot<LinkStatus> link_status{};
    ::app::Slot<Sessions> sessions{};
    ::app::Slot<AlarmState> alarm_state{};
    ::app::Slot<Heartbeat> heartbeat{};
    ::app::Slot<LinkStats> link_stats{};
    ::app::Slot<RtcmStats> rtcm_stats{};
    ::app::Slot<DemuxStats> demux_stats{};
    ::app::Slot<StoreStatus> store_status{};
    ::app::Slot<os::ExecStats> exec_stats{};
    ::app::Slot<SystemStatus> system_status{};
    ::app::Slot<SystemMode> system_mode{};
    ::app::Slot<RtcmBatch> rtcm_batch{};
    ::app::Slot<uint32_t> rtcm_reset{};

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
} // namespace rtk::app::data
