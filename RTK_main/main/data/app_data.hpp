// 应用上下文拥有的共享主题与跨任务锁。
// init 仅启动前调用；绑定后各 Slot 支持跨任务读写，非 ISR。
#pragma once

#include "fw/fanout_ring.hpp"
#include "fw/slot.hpp"
#include "app_config.hpp"
#if RTK_GNSS_ENABLED
#include "data/gnss_output.hpp"
#include "def/gnss_def.hpp"
#endif
#include "def/link_def.hpp"
#include "def/rtcm_def.hpp"
#include "def/storage_def.hpp"
#include "def/system_def.hpp"
#include "modules/rtcm/rtcm_forwarder.hpp"
#include "rtos/spin_lock.hpp"

namespace app::data {
/** 跨任务共享的主题集合；init() 绑定同一把锁后各 Slot 支持跨任务读写，非 ISR。 */
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
    fw::Slot<rtos::ExecStats> exec_stats{};
    fw::Slot<SystemStatus> system_status{};
    fw::Slot<SystemMode> system_mode{};
    fw::Slot<uint32_t> rtcm_reset{};
#if RTK_GNSS_ENABLED
    fw::Slot<rtos::ExecStats> io_exec_stats{};
    fw::Slot<GnssStatus> gnss_status{};
    fw::Slot<gnss::Fix> gnss_fix{};
    fw::Slot<gnss::SatTable> gnss_sats{};
    fw::Slot<GnssOutConfig> gnss_out_cfg{};
    std::array<uint8_t, kGnssAppRingBytes> gnss_app_ring_buf{};
    fw::FanoutRing gnss_app_ring{};
    GnssLatestCache gnss_latest{};
    GnssAppSubState gnss_app_sub{};
#endif

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
} // namespace app::data
