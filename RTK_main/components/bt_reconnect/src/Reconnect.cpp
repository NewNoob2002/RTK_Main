// 通用 SPP 客户端重连策略状态机实现（ADR-0007 §5）。纯 C++20，无 IDF/FreeRTOS 依赖。
// 只由持有它的任务单线程调用，非 ISR 安全；不阻塞、不分配堆。

#include "reconnect/Reconnect.hpp"

#include <algorithm>
#include <cstdint>

namespace reconnect {

namespace {

constexpr uint32_t kXorShiftA = 13U;
constexpr uint32_t kXorShiftB = 17U;
constexpr uint32_t kXorShiftC = 5U;
constexpr uint32_t kPercentBase = 100U;

/**
 * @brief  推进一次 xorshift32，产生下一个伪随机数
 * @param[in,out] state: 随机数状态；为 0 时先改用 kDefaultSeed，随后回写新状态
 * @retval 本轮 32 位伪随机数；状态非 0 时结果恒非 0，固定种子可复现
 */
uint32_t nextXorshift32(uint32_t &state) noexcept
{
    uint32_t x = (state != 0U) ? state : kDefaultSeed;
    x ^= x << kXorShiftA;
    x ^= x >> kXorShiftB;
    x ^= x << kXorShiftC;
    state = x;
    return x;
}

/**
 * @brief  对基础延时施加均匀整数抖动
 * @param  baseMs: 未被抖动的基础延时，单位 ms
 * @param  jitterPct: 抖动幅度百分比，0 表示不抖动
 * @param[in,out] rng: 随机数状态，每次计算推进一次
 * @retval jitterPct==0 时原样返回 baseMs；否则返回 baseMs × [100-j, 100+j]% 的整数结果，
 *         最小 1，超过 UINT32_MAX 时饱和为 UINT32_MAX
 */
uint32_t applyJitter(uint32_t baseMs, uint8_t jitterPct, uint32_t &rng) noexcept
{
    if (jitterPct == 0U) {
        return baseMs;
    }
    const uint32_t pctHalf = static_cast<uint32_t>(jitterPct); // uint8_t 扩宽至 uint32_t 无损
    const uint32_t span = (pctHalf * 2U) + 1U;
    const uint32_t pct = (kPercentBase - pctHalf) + (nextXorshift32(rng) % span);
    const uint64_t scaled =
        (static_cast<uint64_t>(baseMs) * static_cast<uint64_t>(pct)) / kPercentBase;
    if (scaled == 0ULL) {
        return 1U;
    }
    if (scaled > static_cast<uint64_t>(UINT32_MAX)) {
        return UINT32_MAX;
    }
    // 已校验 1 <= scaled <= UINT32_MAX，收窄到 uint32_t 安全。
    return static_cast<uint32_t>(scaled);
}

/**
 * @brief  计算本次重试延时并推进退避级数
 * @param  cfg: 重连参数，时长单位 ms
 * @param  nowMs: 当前毫秒时刻
 * @param  windowStartMs: 快速重试窗口起点（start 或稳定掉线时刻）
 * @param[in,out] backoffMs: 当前退避级数；为 0 且仍在快速窗口内时按 fastRetryMs 计，
 *                          否则从 backoffMinMs 起翻倍并封顶 backoffMaxMs
 * @param[in,out] rng: 随机数状态，用于抖动
 * @retval 施加抖动后的等待时长，单位 ms，最小 1
 * @note   只在失败路径调用；无堆分配，可在任意任务上下文执行
 */
uint32_t computeBackoffDelay(const Config &cfg, uint32_t nowMs, uint32_t windowStartMs,
                             uint32_t &backoffMs, uint32_t &rng) noexcept
{
    uint32_t baseDelay = 0U;
    const uint32_t windowElapsed = nowMs - windowStartMs;
    if ((backoffMs == 0U) && (windowElapsed < cfg.fastWindowMs)) {
        baseDelay = cfg.fastRetryMs;
    } else {
        if (backoffMs == 0U) {
            baseDelay = cfg.backoffMinMs;
        } else {
            baseDelay = std::clamp(backoffMs, cfg.backoffMinMs, cfg.backoffMaxMs);
        }

        if (baseDelay > (cfg.backoffMaxMs / 2U)) {
            backoffMs = cfg.backoffMaxMs;
        } else {
            backoffMs = baseDelay * 2U;
        }
    }
    return applyJitter(baseDelay, cfg.jitterPct, rng);
}

} // namespace

bool Reconnector::configure(const Config &cfg, uint32_t seed) noexcept
{
    if ((cfg.fastWindowMs == 0U) || (cfg.fastRetryMs == 0U) || (cfg.backoffMinMs == 0U) ||
        (cfg.backoffMaxMs == 0U) || (cfg.stableOnlineMs == 0U) || (cfg.attemptTimeoutMs == 0U)) {
        return false;
    }
    if ((cfg.jitterPct > kMaxJitterPct) || (cfg.sdpAfterFailures == 0U) ||
        (cfg.backoffMinMs > cfg.backoffMaxMs)) {
        return false;
    }

    cfg_ = cfg;
    rng_ = (seed != 0U) ? seed : kDefaultSeed;
    return true;
}

void Reconnector::setTarget(const Addr &addr) noexcept
{
    if (has_target_ && (addr_ == addr)) {
        return;
    }
    addr_ = addr;
    has_target_ = true;
    has_scn_ = false;
    scn_ = 0U;
    connect_failures_ = 0U;
    backoff_ms_ = 0U;
    next_attempt_ms_ = 0U;
    state_ = State::kIdle;
}

void Reconnector::clearTarget() noexcept
{
    has_target_ = false;
    addr_.fill(0U);
    has_scn_ = false;
    scn_ = 0U;
    connect_failures_ = 0U;
    backoff_ms_ = 0U;
    window_start_ms_ = 0U;
    state_since_ms_ = 0U;
    next_attempt_ms_ = 0U;
    state_ = State::kIdle;
}

bool Reconnector::start(uint32_t nowMs, Action &out) noexcept
{
    out = Action{};
    if (!has_target_) {
        return false;
    }
    if ((state_ == State::kDiscovering) || (state_ == State::kConnecting) ||
        (state_ == State::kConnected)) {
        return false;
    }

    window_start_ms_ = nowMs;
    backoff_ms_ = 0U;
    connect_failures_ = 0U;
    next_attempt_ms_ = 0U;

    if (has_scn_) {
        state_ = State::kConnecting;
        state_since_ms_ = nowMs;
        ++stats_.attempts;
        out = Action{ActionType::kConnect, scn_};
    } else {
        state_ = State::kDiscovering;
        state_since_ms_ = nowMs;
        ++stats_.sdpQueries;
        out = Action{ActionType::kStartSdp, 0U};
    }
    return true;
}

void Reconnector::stop() noexcept
{
    state_ = State::kStopped;
    next_attempt_ms_ = 0U;
}

Action Reconnector::poll(uint32_t nowMs) noexcept
{
    if (state_ == State::kBackoff) {
        const uint32_t elapsed = nowMs - state_since_ms_;
        if (elapsed >= stats_.lastDelayMs) {
            next_attempt_ms_ = 0U;
            if (has_scn_) {
                state_ = State::kConnecting;
                state_since_ms_ = nowMs;
                ++stats_.attempts;
                return Action{ActionType::kConnect, scn_};
            }
            state_ = State::kDiscovering;
            state_since_ms_ = nowMs;
            ++stats_.sdpQueries;
            return Action{ActionType::kStartSdp, 0U};
        }
        return Action{};
    }

    if (state_ == State::kDiscovering) {
        const uint32_t elapsed = nowMs - state_since_ms_;
        if (elapsed >= cfg_.attemptTimeoutMs) {
            ++stats_.failures;
            const uint32_t delay =
                computeBackoffDelay(cfg_, nowMs, window_start_ms_, backoff_ms_, rng_);
            state_ = State::kBackoff;
            state_since_ms_ = nowMs;
            next_attempt_ms_ = nowMs + delay;
            stats_.lastDelayMs = delay;
        }
        return Action{};
    }

    if (state_ == State::kConnecting) {
        const uint32_t elapsed = nowMs - state_since_ms_;
        if (elapsed >= cfg_.attemptTimeoutMs) {
            ++stats_.failures;
            if (has_scn_) {
                ++connect_failures_;
                if (connect_failures_ >= cfg_.sdpAfterFailures) {
                    has_scn_ = false;
                    scn_ = 0U;
                    connect_failures_ = 0U;
                }
            }
            const uint32_t delay =
                computeBackoffDelay(cfg_, nowMs, window_start_ms_, backoff_ms_, rng_);
            state_ = State::kBackoff;
            state_since_ms_ = nowMs;
            next_attempt_ms_ = nowMs + delay;
            stats_.lastDelayMs = delay;
        }
        return Action{};
    }

    return Action{};
}

Action Reconnector::onSdpResult(bool ok, uint8_t scn, uint32_t nowMs) noexcept
{
    if (state_ == State::kStopped) {
        if (!ok) {
            ++stats_.failures;
        }
        return Action{};
    }
    if (state_ != State::kDiscovering) {
        return Action{};
    }

    if (ok) {
        scn_ = scn;
        has_scn_ = true;
        connect_failures_ = 0U;
        state_ = State::kConnecting;
        state_since_ms_ = nowMs;
        ++stats_.attempts;
        return Action{ActionType::kConnect, scn_};
    }

    ++stats_.failures;
    const uint32_t delay = computeBackoffDelay(cfg_, nowMs, window_start_ms_, backoff_ms_, rng_);
    state_ = State::kBackoff;
    state_since_ms_ = nowMs;
    next_attempt_ms_ = nowMs + delay;
    stats_.lastDelayMs = delay;
    return Action{};
}

Action Reconnector::onOpen(uint32_t nowMs) noexcept
{
    if (state_ == State::kStopped) {
        ++stats_.connects;
        return Action{};
    }
    if (state_ != State::kConnecting) {
        return Action{};
    }

    ++stats_.connects;
    connect_failures_ = 0U;
    state_ = State::kConnected;
    state_since_ms_ = nowMs;
    next_attempt_ms_ = 0U;
    return Action{};
}

Action Reconnector::onConnectFailed(uint32_t nowMs) noexcept
{
    if (state_ == State::kStopped) {
        ++stats_.failures;
        return Action{};
    }
    if (state_ != State::kConnecting) {
        return Action{};
    }

    ++stats_.failures;
    if (has_scn_) {
        ++connect_failures_;
        if (connect_failures_ >= cfg_.sdpAfterFailures) {
            has_scn_ = false;
            scn_ = 0U;
            connect_failures_ = 0U;
        }
    }
    const uint32_t delay = computeBackoffDelay(cfg_, nowMs, window_start_ms_, backoff_ms_, rng_);
    state_ = State::kBackoff;
    state_since_ms_ = nowMs;
    next_attempt_ms_ = nowMs + delay;
    stats_.lastDelayMs = delay;
    return Action{};
}

Action Reconnector::onClose(uint32_t nowMs) noexcept
{
    if (state_ == State::kStopped) {
        ++stats_.disconnects;
        return Action{};
    }
    if (state_ != State::kConnected) {
        return Action{};
    }

    ++stats_.disconnects;
    const uint32_t onlineMs = nowMs - state_since_ms_;
    if (onlineMs >= cfg_.stableOnlineMs) {
        backoff_ms_ = 0U;
        window_start_ms_ = nowMs;
    }

    const uint32_t delay = computeBackoffDelay(cfg_, nowMs, window_start_ms_, backoff_ms_, rng_);
    state_ = State::kBackoff;
    state_since_ms_ = nowMs;
    next_attempt_ms_ = nowMs + delay;
    stats_.lastDelayMs = delay;
    return Action{};
}

uint32_t Reconnector::nextAttemptIn(uint32_t nowMs) const noexcept
{
    if (state_ != State::kBackoff) {
        return 0U;
    }
    const uint32_t elapsed = nowMs - state_since_ms_;
    if (elapsed >= stats_.lastDelayMs) {
        return 0U;
    }
    return stats_.lastDelayMs - elapsed;
}

} // namespace reconnect
