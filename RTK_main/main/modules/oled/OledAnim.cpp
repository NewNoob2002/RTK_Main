// OLED 纯定点动画引擎与按键手势消歧实现（OL2，无浮点、无堆、无 IDF/FreeRTOS 依赖）。
// 所有函数由 OLED 任务或主机测试单线程串行调用；不阻塞、不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/oled/OledAnim.hpp"

#include <algorithm>
#include <cstdlib>

namespace app::oled {
namespace {

constexpr int32_t kSpringSettlePosQ8 = FixedSpring::kScale / 2; // 0.5 px 吸附阈值
constexpr int32_t kSpringSettleVelQ8 = FixedSpring::kScale * 2; // 2 px/s 吸附阈值
constexpr int32_t kMsPerSec = 1000;
constexpr int32_t kHalfScale = FixedSpring::kScale / 2;
constexpr uint32_t kEaseCubeShift = 20U; // 1024^3 >> 20 == 1024

} // namespace

void ClickDetector::reset() noexcept
{
    pending_ = false;
    firstPressMs_ = 0U;
}

ClickEvent ClickDetector::onPress(uint32_t pressMs) noexcept
{
    if (!pending_) {
        pending_ = true;
        firstPressMs_ = pressMs;
        return ClickEvent::kNone;
    }
    const int32_t diff = static_cast<int32_t>(pressMs - firstPressMs_);
    if ((diff < 0) || (static_cast<uint32_t>(diff) < kDoubleClickWindowMs)) {
        pending_ = false;
        return ClickEvent::kDouble;
    }
    firstPressMs_ = pressMs;
    return ClickEvent::kSingle;
}

ClickEvent ClickDetector::pollTimeout(uint32_t nowMs) noexcept
{
    if (!pending_) {
        return ClickEvent::kNone;
    }
    if (static_cast<int32_t>(nowMs - deadlineMs()) >= 0) {
        pending_ = false;
        return ClickEvent::kSingle;
    }
    return ClickEvent::kNone;
}

uint32_t ClickDetector::remainingMs(uint32_t nowMs) const noexcept
{
    if (!pending_) {
        return 0U;
    }
    const int32_t rem = static_cast<int32_t>(deadlineMs() - nowMs);
    return (rem <= 0) ? 0U : static_cast<uint32_t>(rem);
}

uint16_t easeOutCubic(uint32_t elapsedMs, uint32_t durationMs) noexcept
{
    if ((durationMs == 0U) || (elapsedMs >= durationMs)) {
        return kEaseScale;
    }
    if (elapsedMs == 0U) {
        return 0U;
    }
    // 使用 uint64_t 计算 t = elapsed * 1024 / duration，杜绝任何大时长乘法溢出。
    const uint32_t t = static_cast<uint32_t>((static_cast<uint64_t>(elapsedMs) * kEaseScale) /
                                             static_cast<uint64_t>(durationMs));
    const uint32_t inv = static_cast<uint32_t>(kEaseScale) - t;
    // inv <= 1024，inv^3 <= 2^30，完全位于 uint32_t 范围内；右移 20 位结果为 0..1024，窄化安全。
    const uint32_t invCubedQ10 = (inv * inv * inv) >> kEaseCubeShift;
    return static_cast<uint16_t>(static_cast<uint32_t>(kEaseScale) - invCubedQ10);
}

int16_t lerpFixed(int16_t from, int16_t to, uint16_t progressQ10) noexcept
{
    if (progressQ10 == 0U) {
        return from;
    }
    if (progressQ10 >= kEaseScale) {
        return to;
    }
    const int32_t diff = static_cast<int32_t>(to) - static_cast<int32_t>(from);
    // diff 在 [-32768..32767]，乘以 progressQ10(<=1024) 在 int32_t 内，加半步舍入后处于 [from, to]。
    const int32_t scaled = diff * static_cast<int32_t>(progressQ10);
    const int32_t half = static_cast<int32_t>(kEaseScale / 2U);
    const int32_t delta = (scaled >= 0) ? ((scaled + half) / static_cast<int32_t>(kEaseScale))
                                        : ((scaled - half) / static_cast<int32_t>(kEaseScale));
    return static_cast<int16_t>(static_cast<int32_t>(from) + delta);
}

void FixedSpring::snap(int16_t posPx) noexcept
{
    posQ8_ = static_cast<int32_t>(posPx) * kScale;
    targetQ8_ = posQ8_;
    velQ8_ = 0;
    moving_ = false;
}

void FixedSpring::retarget(int16_t targetPx) noexcept
{
    const int32_t nextTarget = static_cast<int32_t>(targetPx) * kScale;
    if ((nextTarget == targetQ8_) && !moving_) {
        return;
    }
    targetQ8_ = nextTarget;
    moving_ = true;
}

void FixedSpring::step(uint32_t dtMs) noexcept
{
    if (!moving_ || (dtMs == 0U)) {
        return;
    }
    const int32_t prevPos = posQ8_;
    const int32_t prevVel = velQ8_;
    uint32_t remaining = std::min(dtMs, kMaxStepMs);
    while (remaining > 0U) {
        const uint32_t sub = std::min(remaining, kSubStepMs);
        remaining -= sub;
        const int64_t errQ8 = static_cast<int64_t>(targetQ8_) - static_cast<int64_t>(posQ8_);
        const int64_t accQ8 =
            (errQ8 * kDefaultStiffness) - (static_cast<int64_t>(velQ8_) * kDefaultDamping);
        // sub <= 16 ms，中间值采用 int64_t 避免溢出，单步速度/位置增量远小于 int32_t 范围。
        const int64_t nextVel =
            static_cast<int64_t>(velQ8_) + ((accQ8 * static_cast<int64_t>(sub)) / kMsPerSec);
        const int64_t nextPos =
            static_cast<int64_t>(posQ8_) + ((nextVel * static_cast<int64_t>(sub)) / kMsPerSec);
        velQ8_ = static_cast<int32_t>(nextVel);
        posQ8_ = static_cast<int32_t>(nextPos);
    }

    const int32_t absErr = std::abs(targetQ8_ - posQ8_);
    const int32_t absVel = std::abs(velQ8_);
    const bool stalled = (posQ8_ == prevPos) && (velQ8_ == prevVel) && (absErr <= kScale);
    if (stalled || ((absErr <= kSpringSettlePosQ8) && (absVel <= kSpringSettleVelQ8))) {
        posQ8_ = targetQ8_;
        velQ8_ = 0;
        moving_ = false;
    }
}

int16_t FixedSpring::position() const noexcept
{
    const int32_t rounded =
        (posQ8_ >= 0) ? ((posQ8_ + kHalfScale) / kScale) : ((posQ8_ - kHalfScale) / kScale);
    // 屏幕像素坐标均在 int16_t 范围内，窄化安全。
    return static_cast<int16_t>(rounded);
}

int16_t FixedSpring::target() const noexcept
{
    return static_cast<int16_t>(targetQ8_ / kScale);
}

void NumberRoll::reset() noexcept
{
    initialized_ = false;
    rolling_ = false;
    prev_ = 0;
    curr_ = 0;
    startMs_ = 0U;
}

/**
 * @brief 喂入最新数值，发生有效变化时启动 180 ms 滚动动画
 * @param value: 新数值
 * @param nowMs: 当前单调时间（ms）
 */
void NumberRoll::update(int32_t value, uint32_t nowMs) noexcept
{
    if (!initialized_) {
        initialized_ = true;
        prev_ = value;
        curr_ = value;
        rolling_ = false;
        return;
    }
    if (value != curr_) {
        prev_ = curr_;
        curr_ = value;
        startMs_ = nowMs;
        rolling_ = true;
    } else if (rolling_ && ((nowMs - startMs_) >= kNumberRollMs)) {
        rolling_ = false;
        prev_ = curr_;
    }
}

bool NumberRoll::active(uint32_t nowMs) const noexcept
{
    return rolling_ && ((nowMs - startMs_) < kNumberRollMs);
}

uint16_t NumberRoll::progressQ10(uint32_t nowMs) const noexcept
{
    if (!rolling_) {
        return kEaseScale;
    }
    return easeOutCubic(nowMs - startMs_, kNumberRollMs);
}

/**
 * @brief 启动一次页面过渡动画
 * @param newKind: 过渡类型
 * @param from: 离开页
 * @param to: 进入页
 * @param anchor: 图标锚点矩形
 * @param nowMs: 起始单调时间（ms）
 * @param durMs: 过渡时长（ms）
 */
void PageTransition::start(TransitionKind newKind, OledPage from, OledPage to, Rect anchor,
                           uint32_t nowMs, uint32_t durMs) noexcept
{
    kind = newKind;
    fromPage = from;
    toPage = to;
    iconRect = anchor;
    startMs = nowMs;
    durationMs = (durMs == 0U) ? kPageTransitionMs : durMs;
}

void PageTransition::clear() noexcept
{
    kind = TransitionKind::kNone;
}

bool PageTransition::pollActive(uint32_t nowMs) noexcept
{
    if (kind == TransitionKind::kNone) {
        return false;
    }
    if ((nowMs - startMs) >= durationMs) {
        kind = TransitionKind::kNone;
        return false;
    }
    return true;
}

uint16_t PageTransition::progressQ10(uint32_t nowMs) const noexcept
{
    if (kind == TransitionKind::kNone) {
        return kEaseScale;
    }
    return easeOutCubic(nowMs - startMs, durationMs);
}

} // namespace app::oled
