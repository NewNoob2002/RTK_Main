// 应用上下文拥有的共享主题与跨任务锁。
// init 仅启动前调用；绑定后各 Slot 支持跨任务读写，非 ISR。
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "fw/FanoutRing.hpp"
#include "fw/Slot.hpp"
#include "fw/Status.hpp"
#include "AppConfig.hpp"
#if RTK_GNSS_ENABLED
#include "data/GnssOutput.hpp"
#include "def/GnssDef.hpp"
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
    void bind_lock(fw::LockOps lock) noexcept
    {
        if (!slot_.has_value()) {
            slot_.emplace();
        }
        slot_->bind_lock(lock);
    }
    void write(const T &value) noexcept
    {
        if (slot_.has_value()) {
            slot_->write(value);
        }
    }
    [[nodiscard]] T read() const noexcept
    {
        return slot_.has_value() ? slot_->read() : T{};
    }
    [[nodiscard]] T read(uint32_t &version_out) const noexcept
    {
        if (!slot_.has_value()) {
            version_out = 0U;
            return T{};
        }
        return slot_->read(version_out);
    }
    [[nodiscard]] uint32_t version() const noexcept
    {
        return slot_.has_value() ? slot_->version() : 0U;
    }

private:
    static_assert(std::is_trivially_destructible_v<fw::Slot<T>>, "Slot must be trivially destructible");
    std::optional<fw::Slot<T>> slot_{};
};

/** 跨任务共享的主题集合；init() 绑定同一把锁后各 Slot 支持跨任务读写，非 ISR。 */
struct DataCenter {
    /** 启动前单线程初始化自旋锁并绑定全部主题；不阻塞，非 ISR，无错误返回。 */
    void init() noexcept;
#if RTK_GNSS_ENABLED
    /** 启动前绑定外供 GNSS App 环形区与最新帧缓存（须在 init() 之后调用）。 */
    [[nodiscard]] fw::Status init_gnss_buffers(std::span<uint8_t> app_ring_buf, std::span<uint8_t> latest_buf) noexcept;
    /** 仅启动绑定回滚、消费者尚未启动时调用；清除全部 GNSS 外供引用，非 ISR。 */
    void unbind_gnss_buffers() noexcept;
#endif

    ZeroInitSlot<LinkStatus> link_status{};
    fw::Slot<Sessions> sessions{};
    fw::Slot<AlarmState> alarm_state{};
    ZeroInitSlot<Heartbeat> heartbeat{};
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
    ZeroInitSlot<gnss::Fix> gnss_fix{};
    ZeroInitSlot<gnss::SatTable> gnss_sats{};
    fw::Slot<GnssOutConfig> gnss_out_cfg{};
    fw::FanoutRing gnss_app_ring{};
    GnssLatestCache gnss_latest{};
    GnssAppSubState gnss_app_sub{};
#endif

    portMUX_TYPE mux{};
};
} // namespace app::data
