// 应用可绑定的配对窗口。LinkManager 和 GAP 使用同一窗口。
#pragma once

#include <cstdint>

#include "core/LinkManager.hpp"
#include "fw/Lock.hpp"

namespace core {

struct PairWindowStatus {
    uint32_t remainingMs = 0U;
    bool confirm = false;
};

/** 链路任务写入、GAP 工作任务读取的配对窗口。 */
class PairWindow final {
public:
    /** @brief 注入快照锁。 @param lock: 锁操作。 @note 启动时绑定。 */
    void bindLock(fw::LockOps lock) noexcept
    {
        lock_ = lock;
    }

    /** @brief 设置窗口。 @param nowMs: 时钟。 @param durationMs: 0 关闭。 @param confirm: 逐个确认。 */
    void set(uint32_t nowMs, uint32_t durationMs, bool confirm = false) noexcept;

    /** @brief 查询窗口。 @param nowMs: 时钟。 @retval 剩余时长与确认模式。 */
    [[nodiscard]] PairWindowStatus snapshot(uint32_t nowMs) const noexcept;

    /** @brief 到期时关闭。 @param nowMs: 时钟。 @retval true 本轮关闭。 */
    [[nodiscard]] bool expire(uint32_t nowMs) noexcept;

    /** @brief 查询是否已打开且尚未处理到期。 @retval true 已打开。 */
    [[nodiscard]] bool active() const noexcept;

private:
    fw::LockOps lock_{};
    uint32_t start_ms_ = 0U;
    uint32_t duration_ms_ = 0U;
    bool confirm_ = false;
};

/** @brief 绑定应用唯一的链路窗口。 @param manager: 链路管理器。 @param window: 同寿命窗口，空指针解除绑定。 @note 仅启动。 */
void bindPairWindow(const LinkManager &manager, PairWindow *window) noexcept;

/** @brief 开窗并追踪。 @param window: 已绑定窗口。 @param nowMs: 时钟。 @param durationMs: 窗口时长。 @param confirm: 逐个确认。 */
void openPairWindow(PairWindow &window, uint32_t nowMs, uint32_t durationMs, bool confirm) noexcept;

/** @brief 关闭窗口。 @param window: 已绑定窗口。 @param nowMs: 时钟。 */
void closePairWindow(PairWindow &window, uint32_t nowMs) noexcept;

} // namespace core
