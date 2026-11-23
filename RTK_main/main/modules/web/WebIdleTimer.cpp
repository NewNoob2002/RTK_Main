// Wi-Fi 配置模式空闲回落计时器实现（ADR-0017 §2.2）。
// 仅 kLink 任务串行调用，不阻塞、不分配堆，非 ISR。
#include "modules/web/WebIdleTimer.hpp"

namespace app::web_cfg {

/**
 * @brief 启动空闲回落计时（从当前时刻起无终端连接满 timeoutMs 后到期）
 * @param nowMs: 当前单调时钟（ms）
 * @param timeoutMs: 空闲超时时长（ms）；0 表示不触发超时
 * @note 启动任务或 kLink 独占调用；重新开始时清除旧空闲时长，不阻塞。
 */
void WebIdleTimer::start(uint32_t nowMs, uint32_t timeoutMs) noexcept
{
    timeoutMs_ = timeoutMs;
    idleSinceMs_ = nowMs;
    lastActivitySeq_ = 0U;
    started_ = (timeoutMs > 0U);
    armed_ = started_;
}

bool WebIdleTimer::tick(uint32_t staCount, uint32_t activitySeq, uint32_t nowMs) noexcept
{
    if (!started_) {
        return false;
    }
    if (staCount > 0U) {
        armed_ = false;
        lastActivitySeq_ = activitySeq;
        return false;
    }
    if (!armed_ || (activitySeq != lastActivitySeq_)) {
        armed_ = true;
        idleSinceMs_ = nowMs;
        lastActivitySeq_ = activitySeq;
        return false;
    }
    return static_cast<uint32_t>(nowMs - idleSinceMs_) >= timeoutMs_;
}

} // namespace app::web_cfg
