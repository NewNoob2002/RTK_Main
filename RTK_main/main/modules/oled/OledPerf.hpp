// OLED 绝对帧节拍与性能计数。
// 纯值运算，调用方负责跨任务同步，非 ISR。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace app::oled {

inline constexpr uint32_t kWaitForEvent = UINT32_MAX;

/**
 * @brief 计算距绝对时间的剩余毫秒。
 * @param nowMs: 当前时刻。
 * @param deadlineMs: 截止时刻，距离小于 2^31 ms。
 * @retval 超期返回 0，否则返回剩余毫秒。
 */
[[nodiscard]] inline uint32_t remainingMs(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    // 有符号差在两时刻相距小于半个计数周期时支持回绕。
    const auto delta = static_cast<int32_t>(deadlineMs - nowMs);
    return delta > 0 ? static_cast<uint32_t>(delta) : 0U;
}

/** OLED 活动期每 100 ms 分配 3 帧，超期槽跳过。 */
class FramePacer final {
public:
    /**
     * @brief 消费一帧并寻找工作完成后的下一个绝对槽。
     * @param startMs: 本帧开始时刻，首次启动时用于锚定节拍。
     * @param endMs: 本帧完成时刻。
     */
    void advance(uint32_t startMs, uint32_t endMs) noexcept
    {
        if (!active_) {
            active_ = true;
            nextMs_ = startMs;
        }
        if (remainingMs(endMs, nextMs_) != 0U) {
            return; // 提前处理的事件不消费还未到期的动画槽。
        }
        nextMs_ += ((endMs - nextMs_) / kCycleMs) * kCycleMs;
        do {
            nextMs_ += kIntervalsMs[phase_];
            phase_ = (phase_ + 1U) % kIntervalsMs.size();
        } while (remainingMs(endMs, nextMs_) == 0U);
    }

    /** @brief 估计下次等待时长。 @param nowMs: 当前时刻。 @retval 距下一动画槽的毫秒数，未活动时等事件。 */
    [[nodiscard]] uint32_t waitMs(uint32_t nowMs) const noexcept
    {
        return active_ ? remainingMs(nowMs, nextMs_) : kWaitForEvent;
    }

private:
    static constexpr uint32_t kCycleMs = 100U;
    static constexpr std::array<uint32_t, 3U> kIntervalsMs{34U, 33U, 33U};
    uint32_t nextMs_ = 0U;
    size_t phase_ = 0U;
    bool active_ = false;
};

/** 计数报告按同一时间区间计算单核占用、帧率及唤醒率。 */
struct PerfTotals {
    uint32_t frames = 0U;
    uint32_t wakeups = 0U;
    uint32_t workUs = 0U;
    static constexpr uint32_t kRateScale = 10000U; // 毫秒转秒，再取每秒速率的十分位。

    /** @brief 计算写屏帧率。 @param ms: 样本时长。 @retval 帧率，单位 0.1 fps。 */
    [[nodiscard]] uint32_t fpsX10(uint32_t ms) const noexcept
    {
        return ms == 0U ? 0U : frames * kRateScale / ms;
    }

    /** @brief 计算单核任务占用。 @param ms: 样本时长。 @retval 占用估算，单位 0.1%。 */
    [[nodiscard]] uint32_t cpuX10(uint32_t ms) const noexcept
    {
        return ms == 0U ? 0U : workUs / ms;
    }

    /** @brief 计算唤醒率。 @param ms: 样本时长。 @retval 唤醒率，单位 0.1 次/s。 */
    [[nodiscard]] uint32_t wakeupsX10(uint32_t ms) const noexcept
    {
        return ms == 0U ? 0U : wakeups * kRateScale / ms;
    }
};

inline constexpr uint32_t kPerfWindowMs = 2000U;

/** 最近 2 s 的计数，250 ms 分桶，上报精度受分桶边界影响。 */
class PerfWindow final {
public:
    /**
     * @brief 记入一轮工作，按起始时刻归桶。
     * @param nowMs: 本轮开始时刻。
     * @param workUs: 去除同步 I2C 等待的墙钟工作量。
     * @param written: 至少写出一页且本帧成功完成。
     * @param woke: 本轮从任务等待中返回。
     */
    void record(uint32_t nowMs, uint32_t workUs, bool written, bool woke) noexcept
    {
        // 使用无符号毫秒差推进桶位置，包含 32-bit 单调时间回绕。
        const uint32_t elapsed = (nowMs - bucketStartMs_) / kBucketMs;
        if (elapsed >= buckets_.size()) {
            buckets_ = {};
        } else {
            for (size_t i = 1U; i <= elapsed; ++i) {
                buckets_[(head_ + i) % buckets_.size()] = {};
            }
        }
        head_ = (head_ + elapsed) % buckets_.size();
        bucketStartMs_ += elapsed * kBucketMs;
        auto &bucket = buckets_[head_];
        if (written) {
            ++bucket.frames;
        }
        if (woke) {
            ++bucket.wakeups;
        }
        bucket.workUs += workUs;
    }

    /** @brief 汇总最近 2 s。 @param nowMs: 当前时刻。 @retval 分桶计数总和。 */
    [[nodiscard]] PerfTotals recent(uint32_t nowMs) const noexcept
    {
        PerfTotals total{};
        const uint32_t age = nowMs - bucketStartMs_;
        for (size_t i = 0U; i < buckets_.size() && age + i * kBucketMs < kPerfWindowMs; ++i) {
            const auto &bucket = buckets_[(head_ + buckets_.size() - i) % buckets_.size()];
            total.frames += bucket.frames;
            total.wakeups += bucket.wakeups;
            total.workUs += bucket.workUs;
        }
        return total;
    }

private:
    static constexpr uint32_t kBucketMs = 250U;
    struct Bucket {
        uint16_t frames = 0U;
        uint16_t wakeups = 0U;
        uint32_t workUs = 0U;
    };
    std::array<Bucket, kPerfWindowMs / kBucketMs> buckets_{};
    uint32_t bucketStartMs_ = 0U;
    size_t head_ = 0U;
};

/** 一次独立 bench 的结果。只累计从 bench 开始后发起的工作。 */
struct BenchStats {
    PerfTotals totals{};
    uint32_t elapsedMs = 0U;
    uint32_t bytes = 0U;
    uint32_t lastRenderUs = 0U;
    uint32_t maxRenderUs = 0U;
    uint32_t lastIntervalMs = 0U;
    uint32_t maxIntervalMs = 0U;
    uint32_t lastWriteMs = 0U;
    uint32_t i2cErrors = 0U;
    bool interrupted = false;

    /**
     * @brief 更新 bench 中的一轮工作与写屏间隔。
     * @param nowMs: 本轮起始时刻。
     * @param workUs: 去除同步 I2C 等待的墙钟工作量。
     * @param renderUs: 渲染耗时。
     * @param txBytes: 成功传输的字节数。
     * @param written: 是否成功写完一帧。
     * @param woke: 是否从任务等待中返回。
     */
    void record(uint32_t nowMs, uint32_t workUs, uint32_t renderUs, uint32_t txBytes, bool written,
                bool woke) noexcept
    {
        totals.wakeups += woke ? 1U : 0U;
        totals.workUs += workUs;
        if (renderUs != 0U) {
            lastRenderUs = renderUs;
            maxRenderUs = std::max(maxRenderUs, renderUs);
        }
        if (written) {
            bytes += txBytes;
            if (totals.frames != 0U) {
                lastIntervalMs = nowMs - lastWriteMs;
                maxIntervalMs = std::max(maxIntervalMs, lastIntervalMs);
            }
            lastWriteMs = nowMs;
            ++totals.frames;
        }
    }
};

} // namespace app::oled
