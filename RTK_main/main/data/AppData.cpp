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
    linkStatus.bindLock(lock);
    linkStatus.write(LinkStatus{});
    sessions.bindLock(lock);
    alarmState.bindLock(lock);
    heartbeat.bindLock(lock);
    heartbeat.write(Heartbeat{});
    linkStats.bindLock(lock);
    rtcmStats.bindLock(lock);
    demuxStats.bindLock(lock);
    storeStatus.bindLock(lock);
    execStats.bindLock(lock);
    systemStatus.bindLock(lock);
    systemMode.bindLock(lock);
    rtcmReset.bindLock(lock);
#if RTK_GNSS_ENABLED
    ioExecStats.bindLock(lock);
    gnssStatus.bindLock(lock);
    gnssFix.bindLock(lock);
    gnssSats.bindLock(lock);
    gnssOutCfg.bindLock(lock);
    gnssReadback.bindLock(lock);
    gnssLatest.bindLock(lock);
    gnssAppSub.bindLock(lock);
#endif
}

#if RTK_GNSS_ENABLED
fw::Status DataCenter::initGnssBuffers(std::span<uint8_t> appRingBuf,
                                       std::span<uint8_t> latestBuf) noexcept
{
    if ((appRingBuf.data() == nullptr) || (appRingBuf.size() < kGnssAppRingBytes) ||
        (latestBuf.data() == nullptr) || (latestBuf.size() < kGnssLatestTotalBytes)) {
        return fw::Status::kInvalidArg;
    }
    const auto lock = rtos::spinLockOps(mux);
    // App typed FanoutRing 最大载荷 = 容量 - 3 B 记录头（ADR-0011 §5.3）。
    if (gnssAppRing.initTyped(appRingBuf.first(kGnssAppRingBytes),
                              kGnssAppRingBytes - fw::kFanoutTypedHeaderBytes,
                              lock) != fw::Result::kOk) {
        return fw::Status::kInvalidArg;
    }
    return gnssLatest.init(latestBuf);
}

void DataCenter::unbindGnssBuffers() noexcept
{
    // 未启动消费者时重建不可拷贝控制对象；不保留即将 abort 的地址，也不裸清对象内存。
    // placement new 仅在原存储重建控制对象，不申请堆；外供块由 arena 随后统一释放。
    gnssAppRing.~FanoutRing();
    ::new (static_cast<void *>(&gnssAppRing)) fw::FanoutRing{};
    gnssLatest.~GnssLatestCache();
    ::new (static_cast<void *>(&gnssLatest)) GnssLatestCache{};
    gnssLatest.bindLock(rtos::spinLockOps(mux));
}
#endif
} // namespace app::data
