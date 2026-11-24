// Wi-Fi 配置模式空闲回落计时器（ADR-0017 §2.2）：无终端连接满超时后触发切回蓝牙。
// 纯 C++20 无 IDF 依赖；仅 kLink 任务串行调用，不阻塞、不分配堆，非 ISR。
#pragma once

#include <cstdint>

namespace app::web_cfg {

constexpr uint32_t kStaCountBits = 4U;
constexpr uint32_t kStaCountMask = (1U << kStaCountBits) - 1U;
constexpr uint32_t kStaSeqMask = 0x0FFFFFFFU;

/**
 * @brief 将 SoftAP 终端数与活动序号打包进单个 32 位一致原子快照
 * @param staCount: 当前连接终端数（钳制至 0..15）
 * @param activitySeq: 连接/断开事件累计序号（低 28 位）
 * @retval 打包后的 32 位状态值
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint32_t packStaState(uint32_t staCount, uint32_t activitySeq) noexcept
{
    const uint32_t clamped = (staCount > kStaCountMask) ? kStaCountMask : staCount;
    return ((activitySeq & kStaSeqMask) << kStaCountBits) | clamped;
}

/**
 * @brief 从一致快照中提取当前连接终端数
 * @param packed: packStaState 生成的 32 位值
 * @retval 连接终端数（0..15）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint32_t staStateCount(uint32_t packed) noexcept
{
    return packed & kStaCountMask;
}

/**
 * @brief 从一致快照中提取活动序号
 * @param packed: packStaState 生成的 32 位值
 * @retval 28 位活动序号
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint32_t staStateSeq(uint32_t packed) noexcept
{
    return (packed >> kStaCountBits) & kStaSeqMask;
}

/**
 * @brief 提交空闲回落重启前复核快照：确认当前无终端且自排定以来未发生新关联事件
 * @param packedStaState: 最新读取的 32 位一致快照
 * @param scheduledActSeq: 排定空闲回落时记录的活动序号
 * @retval true 仍满足空闲回落条件，可提交重启
 * @retval false 期间已有终端重连或发生新的连接事件，应取消本次空闲重启
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr bool canCommitIdleExit(uint32_t packedStaState,
                                               uint32_t scheduledActSeq) noexcept
{
    return (staStateCount(packedStaState) == 0U) &&
           (staStateSeq(packedStaState) == (scheduledActSeq & kStaSeqMask));
}

/** 空闲超时状态机：跟踪无终端连接持续时长，支持 32 位毫秒回绕。 */
class WebIdleTimer final {
public:
    constexpr WebIdleTimer() noexcept = default;
    ~WebIdleTimer() = default;
    WebIdleTimer(const WebIdleTimer &) = delete;
    WebIdleTimer &operator=(const WebIdleTimer &) = delete;

    /**
     * @brief 启动空闲回落计时（从当前时刻起无终端连接满 timeoutMs 后到期）
     * @param nowMs: 当前单调时钟（ms）
     * @param timeoutMs: 空闲超时时长（ms）；0 表示不触发超时
     * @note 启动任务或 kLink 独占调用；重新开始时清除旧空闲时长，不阻塞。
     */
    void start(uint32_t nowMs, uint32_t timeoutMs) noexcept;

    /**
     * @brief 按当前连接终端数与活动序号推进空闲计时
     * @param staCount: 当前连接至 SoftAP 的终端数
     * @param activitySeq: 终端连入/断开事件累计序号（避免两次轮询间短连漏检）
     * @param nowMs: 当前单调时钟（ms）
     * @retval true 空闲已达超时阈值，应推 bye{reason:"idle"} 并切回蓝牙
     * @retval false 尚有终端连接或未满空闲时长
     * @note 仅 kLink 任务调用，不阻塞；到期后继续保持返回 true 直到外部重启或重新 start。
     */
    [[nodiscard]] bool tick(uint32_t staCount, uint32_t activitySeq, uint32_t nowMs) noexcept;

    /**
     * @brief 查询是否正在进行无终端空闲倒计时
     * @retval true 当前无终端且倒计时激活中
     * @retval false 已有终端连接或尚未启动
     * @note 仅 kLink 任务查询，不阻塞。
     */
    [[nodiscard]] bool armed() const noexcept
    {
        return armed_;
    }

private:
    uint32_t timeoutMs_ = 0U;
    uint32_t idleSinceMs_ = 0U;
    uint32_t lastActivitySeq_ = 0U;
    bool started_ = false;
    bool armed_ = false;
};

} // namespace app::web_cfg
