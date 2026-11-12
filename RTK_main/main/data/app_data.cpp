// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "data/app_data.hpp"

namespace rtk::app::data {
void DataCenter::init() noexcept
{
    /* 启动前将所有主题绑定到上下文持有的同一把锁。 */
    const auto lock = os::spin_lock_ops(mux);
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
    rtcm_batch.bind_lock(lock);
    rtcm_reset.bind_lock(lock);
}
} // namespace rtk::app::data
