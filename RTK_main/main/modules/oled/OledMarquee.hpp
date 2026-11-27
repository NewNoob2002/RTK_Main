// OLED 单行文字的像素位移与可见期间的帧调度。
// 仅 OLED 绘制任务或 host 模拟器调用，无 IDF 依赖，不阻塞、非 ISR。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "def/OledDef.hpp"
#include "modules/oled/OledPerf.hpp"

namespace app::oled {

struct MarqueeMotion {
    int16_t offset = 0;
    uint32_t waitMs = kWaitForEvent;
    bool moving = false;
};

/**
 * @brief 计算单行文字的左移像素与停留期剩余时间。
 * @param overflow: 超出裁剪框的像素数。
 * @param elapsedMs: 自该文本变为可见后的毫秒数。
 * @retval 当前运动状态；移动时交由帧节拍器调度。
 * @note 纯函数，可跨任务调用。
 */
[[nodiscard]] constexpr MarqueeMotion marqueeMotion(int16_t overflow, uint32_t elapsedMs) noexcept
{
    constexpr uint32_t kHoldMs = 1000U;
    constexpr uint32_t kPixelsPerSecond = 30U;
    constexpr uint32_t kMsPerSecond = 1000U;
    if (overflow <= 0) {
        return {};
    }
    // 已检查正值，转无符号安全。位移上限仍为原 int16_t 宽度差。
    const auto pixels = static_cast<uint32_t>(overflow);
    const uint32_t moveMs = (pixels * kMsPerSecond + kPixelsPerSecond - 1U) / kPixelsPerSecond;
    const uint32_t phase = elapsedMs % (2U * kHoldMs + moveMs);
    if (phase < kHoldMs) {
        return {.waitMs = kHoldMs - phase};
    }
    const uint32_t movingMs = phase - kHoldMs;
    if (movingMs >= moveMs) {
        return {.offset = overflow, .waitMs = kHoldMs + moveMs - movingMs};
    }
    return {.offset = static_cast<int16_t>(movingMs * kPixelsPerSecond / kMsPerSecond),
            .moving = true};
}

inline constexpr size_t kMarqueeRowCount = 8U;
enum class MarqueeField : uint8_t {
    kCardPrimary = kMarqueeRowCount,
    kCardSecondary,
    kPrimary,
    kSecondary,
    kCount,
};

/** 跟踪可见字段的内容和出现时刻；字段离开屏幕即结束其本轮滚动。 */
class Marquee final {
public:
    /** @brief 开始采样一帧。 @param page: 当前页面。 @param enabled: 前景允许滚动。 */
    void begin(OledPage page, bool enabled) noexcept
    {
        if (page != page_) {
            visible_ = 0U;
            page_ = page;
        }
        enabled_ = enabled;
        sampled_ = 0U;
        moving_ = false;
        waitMs_ = kWaitForEvent;
    }

    /**
     * @brief 更新单个可见字段，文本变化时重新停留。
     * @param field: 本页唯一字段索引。
     * @param text: 完整显示文本。
     * @param overflow: 超出裁剪框的像素数。
     * @param nowMs: 帧开始时刻。
     * @retval 左移的像素数。
     */
    [[nodiscard]] int16_t offset(MarqueeField field, std::string_view text, int16_t overflow,
                                 uint32_t nowMs) noexcept
    {
        if (!enabled_ || overflow <= 0) {
            return 0;
        }
        const auto index = static_cast<size_t>(field);
        const uint32_t bit = uint32_t{1U} << index;
        sampled_ |= bit;
        constexpr uint32_t kFnvOffsetBasis = 2166136261U;
        constexpr uint32_t kFnvPrime = 16777619U;
        uint32_t hash = kFnvOffsetBasis;
        for (const char ch : text) {
            // 哈希使用原始字节，与 host 的 char 符号性无关。
            hash = (hash ^ static_cast<unsigned char>(ch)) * kFnvPrime;
        }
        auto &entry = entries_[index];
        if ((visible_ & bit) == 0U || entry.hash != hash) {
            entry = {.hash = hash, .startMs = nowMs};
        }
        const auto motion = marqueeMotion(overflow, nowMs - entry.startMs);
        waitMs_ = std::min(waitMs_, motion.waitMs);
        moving_ |= motion.moving;
        return motion.offset;
    }

    /** @brief 忘记本帧未绘制的字段。 */
    void finish() noexcept
    {
        visible_ = sampled_;
    }

    /** @brief 是否需要活动期帧节拍。 @retval 至少有一个可见字段在移动。 */
    [[nodiscard]] bool moving() const noexcept
    {
        return moving_;
    }

    /** @brief 下次停留结束的相对时间。 @retval 毫秒数，空闲时为 kWaitForEvent。 */
    [[nodiscard]] uint32_t waitMs() const noexcept
    {
        return waitMs_;
    }

private:
    struct Entry {
        uint32_t hash = 0U;
        uint32_t startMs = 0U;
    };
    static constexpr size_t kFieldCount = static_cast<size_t>(MarqueeField::kCount);
    static_assert(kFieldCount <= 32U);
    std::array<Entry, kFieldCount> entries_{};
    uint32_t visible_ = 0U;
    uint32_t sampled_ = 0U;
    uint32_t waitMs_ = kWaitForEvent;
    OledPage page_ = OledPage::kSystemCheck;
    bool enabled_ = false;
    bool moving_ = false;
};

} // namespace app::oled
