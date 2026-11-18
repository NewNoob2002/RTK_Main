// 应用 NVS 写入计数与恢复出厂排他门控。各执行器可获取写入许可，擦除仅 kLink 执行。
#pragma once

#include <atomic>
#include <cstdint>

#include "fw/Atomic.hpp"

namespace app::store {

inline constexpr uint32_t kWritesClosed = 1U << 31U;
inline constinit std::atomic<uint32_t> s_writes{0U};
static_assert(fw::kAtomicUsable<uint32_t>);

/** 正常写入跨完整 NVS 调用持有许可。 */
class WriteGuard final {
public:
    /** @brief 尝试登记写入。 @note 任意任务，非 ISR。 */
    WriteGuard() noexcept
    {
        auto count = s_writes.load(std::memory_order_relaxed);
        while ((count & kWritesClosed) == 0U) {
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

} // namespace app::store
