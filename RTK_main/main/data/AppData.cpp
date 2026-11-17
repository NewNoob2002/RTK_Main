// DataCenter 主题槽的锁绑定：全部 Slot 共用上下文持有的同一把 spin lock。
// 启动前单线程调用一次（init()），之后只被读写者经注入的锁访问。
#include "data/AppData.hpp"

#include <new>

namespace app::data {
/**
 * @brief  初始化共享自旋锁并把全部主题槽绑定到它；启动前单线程调用一次
 */
void DataCenter::init() noexcept
{
    /* 启动前初始化自旋锁，并将所有主题绑定到上下文持有的同一把锁。 */
    portMUX_INITIALIZE(&mux);
    const auto lock = rtos::spinLockOps(mux);
    link_status.bind_lock(lock);
    link_status.write(LinkStatus{});
    sessions.bindLock(lock);
    alarm_state.bindLock(lock);
    heartbeat.bind_lock(lock);
    heartbeat.write(Heartbeat{});
    link_stats.bindLock(lock);
    rtcm_stats.bindLock(lock);
    demux_stats.bindLock(lock);
    store_status.bindLock(lock);
    exec_stats.bindLock(lock);
    system_status.bindLock(lock);
    system_mode.bindLock(lock);
    rtcm_reset.bindLock(lock);
#if RTK_GNSS_ENABLED
    io_exec_stats.bindLock(lock);
    gnss_status.bindLock(lock);
    gnss_fix.bind_lock(lock);
    gnss_sats.bind_lock(lock);
    gnss_out_cfg.bindLock(lock);
    gnss_readback.bindLock(lock);
    gnss_latest.bind_lock(lock);
    gnss_app_sub.bind_lock(lock);
#endif
}

#if RTK_GNSS_ENABLED
fw::Status DataCenter::init_gnss_buffers(std::span<uint8_t> app_ring_buf,
                                         std::span<uint8_t> latest_buf) noexcept
{
    if ((app_ring_buf.data() == nullptr) || (app_ring_buf.size() < kGnssAppRingBytes) ||
        (latest_buf.data() == nullptr) || (latest_buf.size() < kGnssLatestTotalBytes)) {
        return fw::Status::kInvalidArg;
    }
    const auto lock = rtos::spinLockOps(mux);
    // App typed FanoutRing 最大载荷 = 容量 - 3 B 记录头（ADR-0011 §5.3）。
    if (gnss_app_ring.initTyped(app_ring_buf.first(kGnssAppRingBytes),
                                kGnssAppRingBytes - fw::kFanoutTypedHeaderBytes,
                                lock) != fw::Result::kOk) {
        return fw::Status::kInvalidArg;
    }
    return gnss_latest.init(latest_buf);
}

void DataCenter::unbind_gnss_buffers() noexcept
{
    // 未启动消费者时重建不可拷贝控制对象；不保留即将 abort 的地址，也不裸清对象内存。
    // placement new 仅在原存储重建控制对象，不申请堆；外供块由 arena 随后统一释放。
    gnss_app_ring.~FanoutRing();
    ::new (static_cast<void *>(&gnss_app_ring)) fw::FanoutRing{};
    gnss_latest.~GnssLatestCache();
    ::new (static_cast<void *>(&gnss_latest)) GnssLatestCache{};
    gnss_latest.bind_lock(rtos::spinLockOps(mux));
}
#endif
} // namespace app::data
