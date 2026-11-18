// 应用上下文拥有的共享主题与跨任务锁。
// init 仅启动前调用；绑定后各 Slot 支持跨任务读写，非 ISR。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "fw/FanoutRing.hpp"
#include "fw/Slot.hpp"
#include "fw/Status.hpp"
#include "AppConfig.hpp"
#if RTK_GNSS_ENABLED
#include "data/BaseRtcmRing.hpp"
#include "data/GnssOutput.hpp"
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#endif
#include "def/LinkDef.hpp"
#include "def/RtcmDef.hpp"
#include "def/StorageDef.hpp"
#include "def/SystemDef.hpp"
#include "modules/rtcm/RtcmForwarder.hpp"

namespace app::data {
/** 零初值延迟槽包装：静态存储落 .bss，在 DataCenter::init() 时原地构造并绑定锁（ADR-0011 §7）。 */
template <typename T> class ZeroInitSlot {
public:
    constexpr ZeroInitSlot() noexcept = default;
    /**
     * @brief 构造延迟槽并绑定跨任务锁
     * @param lock: 跨任务同步操作表
     * @note 仅启动前单线程调用，不阻塞，非 ISR。
     */
    void bindLock(fw::LockOps lock) noexcept
    {
        if (!slot_.has_value()) {
            slot_.emplace();
        }
        slot_->bindLock(lock);
    }
    /**
     * @brief 更新已构造槽，未构造时忽略
     * @param value: 待转换或写入的值
     * @note 绑定后支持跨任务调用，内部加锁，非 ISR。
     */
    void write(const T &value) noexcept
    {
        if (slot_.has_value()) {
            slot_->write(value);
        }
    }
    /**
     * @brief 读取槽快照，未构造时返回零初值
     * @retval 槽快照；未绑定时为 T{}
     * @note 绑定后支持跨任务调用，内部加锁，非 ISR。
     */
    [[nodiscard]] T read() const noexcept
    {
        return slot_.has_value() ? slot_->read() : T{};
    }
    /**
     * @brief 读取槽快照，未构造时返回零初值
     * @param[out] versionOut: 版本输出；未构造时置 0
     * @retval 槽快照；未绑定时为 T{}
     * @note 绑定后支持跨任务调用，内部加锁，非 ISR。
     */
    [[nodiscard]] T read(uint32_t &versionOut) const noexcept
    {
        if (!slot_.has_value()) {
            versionOut = 0U;
            return T{};
        }
        return slot_->read(versionOut);
    }
    /**
     * @brief 读取槽版本号
     * @retval 当前版本；未构造时为 0
     * @note 绑定后支持跨任务调用，内部加锁，非 ISR。
     */
    [[nodiscard]] uint32_t version() const noexcept
    {
        return slot_.has_value() ? slot_->version() : 0U;
    }

private:
    static_assert(std::is_trivially_destructible_v<fw::Slot<T>>,
                  "Slot must be trivially destructible");
    std::optional<fw::Slot<T>> slot_{};
};

/** 跨任务共享的主题集合；init() 绑定同一把锁后各 Slot 支持跨任务读写，非 ISR。 */
struct DataCenter {
    /**
     * @brief  初始化共享自旋锁并把全部主题槽绑定到它；启动前单线程调用一次
     */
    void init() noexcept;
#if RTK_GNSS_ENABLED
    /**
     * @brief 启动前绑定外供 GNSS App 环形区与最新帧缓存（须在 init() 之后调用）。
     * @param appRingBuf: GNSS App 多读环形存储，容量须匹配配置
     * @param latestBuf: 最新帧缓存，至少 kGnssLatestTotalBytes 字节
     * @retval kOk 两个数据缓冲均初始化成功
     * @retval kInvalidArg 环或缓存存储不满足容量要求
     * @note 仅 DataCenter::init 后、消费者启动前单线程调用，非 ISR。
     */
    [[nodiscard]] fw::Status initGnssBuffers(std::span<uint8_t> appRingBuf,
                                             std::span<uint8_t> latestBuf) noexcept;
    /**
     * @brief 仅启动绑定回滚、消费者尚未启动时调用；清除全部 GNSS 外供引用，非 ISR。
     * @note 仅启动绑定回滚、消费者尚未启动时调用；原地重建不分配堆，非 ISR。
     */
    void unbindGnssBuffers() noexcept;
#endif

    ZeroInitSlot<LinkStatus> linkStatus{};
    fw::Slot<Sessions> sessions{};
    fw::Slot<AlarmState> alarmState{};
    ZeroInitSlot<Heartbeat> heartbeat{};
    fw::Slot<LinkStats> linkStats{};
    fw::Slot<RtcmStats> rtcmStats{};
    fw::Slot<DemuxStats> demuxStats{};
    fw::Slot<StoreStatus> storeStatus{};
    fw::Slot<rtos::ExecStats> execStats{};
    fw::Slot<SystemStatus> systemStatus{};
    fw::Slot<SystemMode> systemMode{};
    fw::Slot<uint32_t> rtcmReset{};
#if RTK_GNSS_ENABLED
    fw::Slot<rtos::ExecStats> ioExecStats{};
    fw::Slot<GnssStatus> gnssStatus{};
    ZeroInitSlot<gnss::Fix> gnssFix{};
    ZeroInitSlot<gnss::SatTable> gnssSats{};
    fw::Slot<GnssOutConfig> gnssOutCfg{};
    fw::Slot<GnssReadback> gnssReadback{};
    // 读回文本交接缓冲（ADR-0012 §2.7）：只有 kIo 在读回进行中写，写完后才 write(gnssReadback) 发布；
    // 控制台在看到 gnssReadback.seq 变化后按 len 读取。下一次读回只能由控制台在读完后发起，故读写不重叠、无需加锁。
    std::array<char, kGnssReadbackBytes> gnssReadbackText{};
    fw::FanoutRing gnssAppRing{};
    GnssLatestCache gnssLatest{};
    GnssAppSubState gnssAppSub{};
    ZeroInitSlot<GnssWorkModeStatus> gnssWorkMode{};
    BaseRtcmRing baseRtcmRing{};
    std::atomic<bool> phoneStreamReady{false};
#endif

    portMUX_TYPE mux{};
};
} // namespace app::data
