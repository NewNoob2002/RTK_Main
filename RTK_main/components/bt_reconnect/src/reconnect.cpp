// 通用 SPP 客户端重连策略状态机实现（ADR-0007 §5）。纯 C++20，无 IDF/FreeRTOS 依赖。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "bt_reconnect/reconnect.hpp"

#include <algorithm>
#include <cstdint>

namespace btrc {

namespace {

constexpr uint32_t kXorShiftA = 13U;
constexpr uint32_t kXorShiftB = 17U;
constexpr uint32_t kXorShiftC = 5U;
constexpr uint32_t kPercentBase = 100U;

uint32_t next_xorshift32(uint32_t &state) noexcept
{
    uint32_t x = (state != 0U) ? state : kDefaultSeed;
    x ^= x << kXorShiftA;
    x ^= x >> kXorShiftB;
    x ^= x << kXorShiftC;
    state = x;
    return x;
}

uint32_t apply_jitter(uint32_t base_ms, uint8_t jitter_pct, uint32_t &rng) noexcept
{
    if (jitter_pct == 0U) {
        return base_ms;
    }
    const uint32_t pct_half = static_cast<uint32_t>(jitter_pct); // uint8_t 扩宽至 uint32_t 无损
    const uint32_t span = (pct_half * 2U) + 1U;
    const uint32_t pct = (kPercentBase - pct_half) + (next_xorshift32(rng) % span);
    const uint64_t scaled = (static_cast<uint64_t>(base_ms) * static_cast<uint64_t>(pct)) / kPercentBase;
    if (scaled == 0ULL) {
        return 1U;
    }
    if (scaled > static_cast<uint64_t>(UINT32_MAX)) {
        return UINT32_MAX;
    }
    // 已校验 1 <= scaled <= UINT32_MAX，收窄到 uint32_t 安全。
    return static_cast<uint32_t>(scaled);
}

uint32_t compute_backoff_delay(const Config &cfg, uint32_t now_ms, uint32_t window_start_ms, uint32_t &backoff_ms,
                               uint32_t &rng) noexcept
{
    uint32_t base_delay = 0U;
    const uint32_t window_elapsed = now_ms - window_start_ms;
    if ((backoff_ms == 0U) && (window_elapsed < cfg.fast_window_ms)) {
        base_delay = cfg.fast_retry_ms;
    } else {
        if (backoff_ms == 0U) {
            base_delay = cfg.backoff_min_ms;
        } else {
            base_delay = std::clamp(backoff_ms, cfg.backoff_min_ms, cfg.backoff_max_ms);
        }

        if (base_delay > (cfg.backoff_max_ms / 2U)) {
            backoff_ms = cfg.backoff_max_ms;
        } else {
            backoff_ms = base_delay * 2U;
        }
    }
    return apply_jitter(base_delay, cfg.jitter_pct, rng);
}

} // namespace

bool Reconnector::configure(const Config &cfg, uint32_t seed) noexcept
{
    if ((cfg.fast_window_ms == 0U) || (cfg.fast_retry_ms == 0U) || (cfg.backoff_min_ms == 0U) ||
        (cfg.backoff_max_ms == 0U) || (cfg.stable_online_ms == 0U) || (cfg.attempt_timeout_ms == 0U)) {
        return false;
    }
    if ((cfg.jitter_pct > kMaxJitterPct) || (cfg.sdp_after_failures == 0U) ||
        (cfg.backoff_min_ms > cfg.backoff_max_ms)) {
        return false;
    }

    cfg_ = cfg;
    rng_ = (seed != 0U) ? seed : kDefaultSeed;
    return true;
}

void Reconnector::set_target(const Addr &addr) noexcept
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

void Reconnector::clear_target() noexcept
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

bool Reconnector::start(uint32_t now_ms, Action &out) noexcept
{
    out = Action{};
    if (!has_target_) {
        return false;
    }
    if ((state_ == State::kDiscovering) || (state_ == State::kConnecting) || (state_ == State::kConnected)) {
        return false;
    }

    window_start_ms_ = now_ms;
    backoff_ms_ = 0U;
    connect_failures_ = 0U;
    next_attempt_ms_ = 0U;

    if (has_scn_) {
        state_ = State::kConnecting;
        state_since_ms_ = now_ms;
        ++stats_.attempts;
        out = Action{ActionType::kConnect, scn_};
    } else {
        state_ = State::kDiscovering;
        state_since_ms_ = now_ms;
        ++stats_.sdp_queries;
        out = Action{ActionType::kStartSdp, 0U};
    }
    return true;
}

void Reconnector::stop() noexcept
{
    state_ = State::kStopped;
    next_attempt_ms_ = 0U;
}

Action Reconnector::poll(uint32_t now_ms) noexcept
{
    if (state_ == State::kBackoff) {
        const uint32_t elapsed = now_ms - state_since_ms_;
        if (elapsed >= stats_.last_delay_ms) {
            next_attempt_ms_ = 0U;
            if (has_scn_) {
                state_ = State::kConnecting;
                state_since_ms_ = now_ms;
                ++stats_.attempts;
                return Action{ActionType::kConnect, scn_};
            }
            state_ = State::kDiscovering;
            state_since_ms_ = now_ms;
            ++stats_.sdp_queries;
            return Action{ActionType::kStartSdp, 0U};
        }
        return Action{};
    }

    if (state_ == State::kDiscovering) {
        const uint32_t elapsed = now_ms - state_since_ms_;
        if (elapsed >= cfg_.attempt_timeout_ms) {
            ++stats_.failures;
            const uint32_t delay = compute_backoff_delay(cfg_, now_ms, window_start_ms_, backoff_ms_, rng_);
            state_ = State::kBackoff;
            state_since_ms_ = now_ms;
            next_attempt_ms_ = now_ms + delay;
            stats_.last_delay_ms = delay;
        }
        return Action{};
    }

    if (state_ == State::kConnecting) {
        const uint32_t elapsed = now_ms - state_since_ms_;
        if (elapsed >= cfg_.attempt_timeout_ms) {
            ++stats_.failures;
            if (has_scn_) {
                ++connect_failures_;
                if (connect_failures_ >= cfg_.sdp_after_failures) {
                    has_scn_ = false;
                    scn_ = 0U;
                    connect_failures_ = 0U;
                }
            }
            const uint32_t delay = compute_backoff_delay(cfg_, now_ms, window_start_ms_, backoff_ms_, rng_);
            state_ = State::kBackoff;
            state_since_ms_ = now_ms;
            next_attempt_ms_ = now_ms + delay;
            stats_.last_delay_ms = delay;
        }
        return Action{};
    }

    return Action{};
}

Action Reconnector::on_sdp_result(bool ok, uint8_t scn, uint32_t now_ms) noexcept
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
        state_since_ms_ = now_ms;
        ++stats_.attempts;
        return Action{ActionType::kConnect, scn_};
    }

    ++stats_.failures;
    const uint32_t delay = compute_backoff_delay(cfg_, now_ms, window_start_ms_, backoff_ms_, rng_);
    state_ = State::kBackoff;
    state_since_ms_ = now_ms;
    next_attempt_ms_ = now_ms + delay;
    stats_.last_delay_ms = delay;
    return Action{};
}

Action Reconnector::on_open(uint32_t now_ms) noexcept
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
    state_since_ms_ = now_ms;
    next_attempt_ms_ = 0U;
    return Action{};
}

Action Reconnector::on_connect_failed(uint32_t now_ms) noexcept
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
        if (connect_failures_ >= cfg_.sdp_after_failures) {
            has_scn_ = false;
            scn_ = 0U;
            connect_failures_ = 0U;
        }
    }
    const uint32_t delay = compute_backoff_delay(cfg_, now_ms, window_start_ms_, backoff_ms_, rng_);
    state_ = State::kBackoff;
    state_since_ms_ = now_ms;
    next_attempt_ms_ = now_ms + delay;
    stats_.last_delay_ms = delay;
    return Action{};
}

Action Reconnector::on_close(uint32_t now_ms) noexcept
{
    if (state_ == State::kStopped) {
        ++stats_.disconnects;
        return Action{};
    }
    if (state_ != State::kConnected) {
        return Action{};
    }

    ++stats_.disconnects;
    const uint32_t online_ms = now_ms - state_since_ms_;
    if (online_ms >= cfg_.stable_online_ms) {
        backoff_ms_ = 0U;
        window_start_ms_ = now_ms;
    }

    const uint32_t delay = compute_backoff_delay(cfg_, now_ms, window_start_ms_, backoff_ms_, rng_);
    state_ = State::kBackoff;
    state_since_ms_ = now_ms;
    next_attempt_ms_ = now_ms + delay;
    stats_.last_delay_ms = delay;
    return Action{};
}

uint32_t Reconnector::next_attempt_in(uint32_t now_ms) const noexcept
{
    if (state_ != State::kBackoff) {
        return 0U;
    }
    const uint32_t elapsed = now_ms - state_since_ms_;
    if (elapsed >= stats_.last_delay_ms) {
        return 0U;
    }
    return stats_.last_delay_ms - elapsed;
}

} // namespace btrc
