// DataCenter 主题槽的锁绑定：全部 Slot 共用上下文持有的同一把 spin lock。
// 启动前单线程调用一次（init()），之后只被读写者经注入的锁访问。
#include "data/app_data.hpp"

namespace app::data {
void DataCenter::init() noexcept
{
    /* 启动前将所有主题绑定到上下文持有的同一把锁。 */
    const auto lock = rtos::spin_lock_ops(mux);
    link_status.bind_lock(lock);
    sessions.bind_lock(lock);
    alarm_state.bind_lock(lock);
    heartbeat.bind_lock(lock);
    link_stats.bind_lock(lock);
    rtcm_stats.bind_lock(lock);
    demux_stats.bind_lock(lock);
    store_status.bind_lock(lock);
    exec_stats.bind_lock(lock);
    system_status.bind_lock(lock);
    system_mode.bind_lock(lock);
    rtcm_reset.bind_lock(lock);
#if RTK_GNSS_ENABLED
    io_exec_stats.bind_lock(lock);
    gnss_status.bind_lock(lock);
    gnss_fix.bind_lock(lock);
    gnss_sats.bind_lock(lock);
    gnss_out_cfg.bind_lock(lock);
    // App typed FanoutRing 最大载荷 = 容量 - 3 B 记录头（ADR-0011 §5.3）。
    (void)gnss_app_ring.init_typed(gnss_app_ring_buf, kGnssAppRingBytes - fw::kFanoutTypedHeaderBytes, lock);
    gnss_latest.bind_lock(lock);
    gnss_app_sub.bind_lock(lock);
#endif
}
} // namespace app::data
