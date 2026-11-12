// 应用上下文拥有的共享主题与跨任务锁。
// init 仅启动前调用；绑定后各 Slot 支持跨任务读写，非 ISR。
#pragma once

#include "rtk/fw/slot.hpp"
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

    fw::Slot<LinkStatus> link_status{};
    fw::Slot<Sessions> sessions{};
    fw::Slot<AlarmState> alarm_state{};
    fw::Slot<Heartbeat> heartbeat{};
    fw::Slot<LinkStats> link_stats{};
    fw::Slot<RtcmStats> rtcm_stats{};
    fw::Slot<DemuxStats> demux_stats{};
    fw::Slot<StoreStatus> store_status{};
    fw::Slot<os::ExecStats> exec_stats{};
    fw::Slot<SystemStatus> system_status{};
    fw::Slot<SystemMode> system_mode{};
    fw::Slot<RtcmBatch> rtcm_batch{};
    fw::Slot<uint32_t> rtcm_reset{};

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
} // namespace rtk::app::data
