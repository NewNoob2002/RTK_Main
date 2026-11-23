// 应用 NVS 写入计数与恢复出厂排他门控。各执行器可获取写入许可，擦除仅 kLink 执行。
#pragma once

#include <atomic>
#include <cstdint>

#include "board/PowerLogic.hpp"
#include "fw/Atomic.hpp"
#include "rtos/EspStatus.hpp"

namespace app::store {

inline constexpr uint32_t kWritesClosed = 1U << 31U;
inline constinit std::atomic<uint32_t> s_writes{0U};
static_assert(fw::kAtomicUsable<uint32_t>);

enum class PowerSavePhase : uint8_t { kNormal, kSaving, kPaused };
static_assert(fw::kAtomicUsable<PowerSavePhase>);
inline constinit std::atomic<PowerSavePhase> s_power_save_phase{PowerSavePhase::kNormal};
inline constinit std::atomic<uint32_t> s_power_save_start_ms{0U};

/** @brief 发布关机保存的起点。 @param nowMs: 状态机保存阶段起点。 @note 仅 kLink。 */
inline void startPowerSave(uint32_t nowMs) noexcept
{
    s_power_save_start_ms.store(nowMs, std::memory_order_relaxed);
    s_power_save_phase.store(PowerSavePhase::kSaving, std::memory_order_release);
}

/** @brief 静默后停止启动新写。 @note 仅 kLink。 */
inline void pausePowerSave() noexcept
{
    s_power_save_phase.store(PowerSavePhase::kPaused, std::memory_order_release);
}

/** @brief 关机超时后恢复写入。 @note 仅 kLink。 */
inline void resumePowerSave() noexcept
{
    s_power_save_phase.store(PowerSavePhase::kNormal, std::memory_order_release);
}

/** @brief 查询当前能否启动写入。 @retval true 允许新的写入。 @note 任意任务，非 ISR。 */
[[nodiscard]] inline bool canStartWrite() noexcept
{
    const auto phase = s_power_save_phase.load(std::memory_order_acquire);
    return phase == PowerSavePhase::kNormal ||
           (phase == PowerSavePhase::kSaving &&
            rtos::nowMs() - s_power_save_start_ms.load(std::memory_order_relaxed) <
                board::kShutdownSaveTimeoutMs);
}

/** 正常写入跨完整 NVS 调用持有许可。 */
class WriteGuard final {
public:
    /** @brief 尝试登记写入。 @note 任意任务，非 ISR。 */
    WriteGuard() noexcept
    {
        auto count = s_writes.load(std::memory_order_relaxed);
        while ((count & kWritesClosed) == 0U && canStartWrite()) {
            // 在途写入数受执行器数量限制，远低于状态位。
            if (s_writes.compare_exchange_weak(count, count + 1U, std::memory_order_acq_rel)) {
                active_ = true;
                break;
            }
        }
    }

    /** @brief 释放已登记的写入。 @note 任意任务，非 ISR。 */
    ~WriteGuard() noexcept
    {
        if (active_) {
            s_writes.fetch_sub(1U, std::memory_order_acq_rel);
        }
    }

    WriteGuard(const WriteGuard &) = delete;
    WriteGuard &operator=(const WriteGuard &) = delete;

    /** @brief 查询是否获准写入。 @retval true 已取得许可。 */
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }

private:
    bool active_ = false;
};

/** @brief 在无在途写入时独占应用 NVS。 @retval true 取得擦除许可。 @note 仅 kLink。 */
[[nodiscard]] inline bool closeWritesForErase() noexcept
{
    uint32_t idle = 0U;
    return s_writes.compare_exchange_strong(idle, kWritesClosed, std::memory_order_acq_rel);
}

/** @brief 擦除失败后重新允许写入。 @note 仅 kLink。 */
inline void reopenWrites() noexcept
{
    s_writes.store(0U, std::memory_order_release);
}

/**
 * @brief 查询当前在途 NVS 写入计数（不含关闭位）
 * @retval 在途写入数量
 * @note 任意任务可调用（无锁原子），不阻塞，非 ISR。
 */
[[nodiscard]] inline uint32_t inFlightWrites() noexcept
{
    return s_writes.load(std::memory_order_acquire) & ~kWritesClosed;
}

} // namespace app::store
