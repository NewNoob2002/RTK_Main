// 跨任务仲裁关机、重启与设置入队；单字原子操作，无堆分配。
// 所有入口任意任务可调用，不阻塞，非 ISR。
#pragma once

#include <atomic>
#include <cstdint>

#include "board/PowerLogic.hpp"
#include "fw/Atomic.hpp"

namespace app::power {

/** 原子仲裁动作及尚未结束入队的设置请求。 */
class PowerRequests final {
public:
    /** @brief 登记一次设置入队。 @retval true 已登记。 @retval false 已停止接收新请求。 */
    [[nodiscard]] bool beginSetting() noexcept
    {
        auto state = state_.load(std::memory_order_acquire);
        while ((state & kActionMask) == 0U && state != kCountMask) {
            if (state_.compare_exchange_weak(state, state + 1U, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    /** @brief 结束一次已登记的入队或直写。 */
    void endSetting() noexcept
    {
        state_.fetch_sub(1U, std::memory_order_acq_rel);
    }

    /** @brief 查询入队是否已结束。 @retval true 已登记的请求均已完成投递或直写。 */
    [[nodiscard]] bool settingsSubmitted() const noexcept
    {
        return (state_.load(std::memory_order_acquire) & kCountMask) == 0U;
    }

    /**
     * @brief 受理关机并原子关闭设置准入
     * @param mode: 启动运行档
     * @param reason: 本次关机原因
     * @retval true 已受理
     * @retval false 充电待机、原因无效或已有动作
     */
    [[nodiscard]] bool requestShutdown(board::BootMode mode, board::ShutdownReason reason) noexcept
    {
        // 原因枚举底层是 uint8_t，整体存入高 8 位。
        return mode == board::BootMode::kNormal && reason != board::ShutdownReason::kNone &&
               claimAction(static_cast<uint32_t>(reason) << kActionShift);
    }

    /** @brief 查询已受理的关机原因。 @retval 关机原因或 kNone。 */
    [[nodiscard]] board::ShutdownReason shutdownReason() const noexcept
    {
        const auto action = state_.load(std::memory_order_acquire) & kActionMask;
        // 已去除计数，剩余高 8 位为原始枚举或重启标记。
        return action == kRestart ? board::ShutdownReason::kNone
                                  : static_cast<board::ShutdownReason>(action >> kActionShift);
    }

    /** @brief 查询是否可安排重启。 @retval true 暂无关机或重启动作。 */
    [[nodiscard]] bool canScheduleRestart() const noexcept
    {
        return (state_.load(std::memory_order_acquire) & kActionMask) == 0U;
    }

    /** @brief 提交普通重启。 @retval true 取得重启权。 @retval false 已有动作。 */
    [[nodiscard]] bool claimRestart() noexcept
    {
        return claimAction(kRestart);
    }

    /** @brief 关机超时或重启提交失败时由动作所有者释放仲裁。 */
    void releaseAction() noexcept
    {
        state_.fetch_and(kCountMask, std::memory_order_acq_rel);
    }

private:
    /**
     * @brief 尝试独占电源动作并关闭设置准入
     * @param action: 高 8 位内编码的非零动作
     * @retval true 已取得独占权
     */
    [[nodiscard]] bool claimAction(uint32_t action) noexcept
    {
        auto state = state_.load(std::memory_order_acquire);
        while ((state & kActionMask) == 0U) {
            if (state_.compare_exchange_weak(state, state | action, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    static constexpr unsigned kActionShift = 24U;
    static constexpr uint32_t kActionMask = 0xFFU << kActionShift;
    static constexpr uint32_t kCountMask = ~kActionMask;
    static constexpr uint32_t kRestart = 0xFFU << kActionShift;
    std::atomic<uint32_t> state_{0U};
};

static_assert(fw::kAtomicUsable<uint32_t>);
inline constinit PowerRequests s_power_requests{};

/** 设置请求从检查到投递或同步直写均登记在内。 */
class SettingAdmission final {
public:
    /** @brief 尝试登记一次设置写入口。 */
    SettingAdmission() noexcept : admitted_(s_power_requests.beginSetting()) {}
    /** @brief 结束入队登记或同步直写。 */
    ~SettingAdmission() noexcept
    {
        if (admitted_) {
            s_power_requests.endSetting();
        }
    }

    SettingAdmission(const SettingAdmission &) = delete;
    SettingAdmission &operator=(const SettingAdmission &) = delete;

    /** @brief 查询准入结果。 @retval true 请求可继续投递或直写。 */
    [[nodiscard]] bool admitted() const noexcept
    {
        return admitted_;
    }

private:
    bool admitted_;
};

} // namespace app::power
