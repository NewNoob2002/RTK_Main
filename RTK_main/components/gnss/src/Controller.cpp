// GNSS 探测/配置/运行状态机实现（ADR-0010 §2.6）。纯逻辑：不读写 UART、不改波特率、不操作电源，
// 只通过 poll() 返回 Action 让 main 侧执行；所有时间由调用方传入 now_ms。
// 线程/调用约束：非线程安全，单任务顺序调用；不可阻塞、不可在 ISR 中使用。
// 堆：不使用。时间比较统一用 int32 差值，支持 uint32 回绕。
#include "gnss/Controller.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace gnss {
namespace {

struct Candidate {
    const Dialect *dialect = nullptr;
    uint32_t baud = 0;
};

const Dialect *hint_dialect(std::span<const Dialect *const> dialects, const Hint &hint) noexcept
{
    if (hint.family == Family::kUnknown) {
        return nullptr;
    }
    for (const Dialect *dialect : dialects) {
        if (dialect != nullptr && dialect->family == hint.family) {
            return dialect;
        }
    }
    return nullptr;
}

size_t candidate_count(std::span<const Dialect *const> dialects, std::span<const uint32_t> bauds,
                       const Hint &hint) noexcept
{
    size_t count = dialects.size() * bauds.size();
    if (hint_dialect(dialects, hint) != nullptr) {
        ++count; // 提示组合排在最前，优先探测
    }
    return count;
}

// 探测顺序：提示的（方言, 波特率）→ 启用方言 × 波特率全表（方言为外层循环）。
bool candidate_at(std::span<const Dialect *const> dialects, std::span<const uint32_t> bauds, const Hint &hint,
                  size_t index, Candidate &out) noexcept
{
    const Dialect *hinted = hint_dialect(dialects, hint);
    if (hinted != nullptr) {
        if (index == 0) {
            out.dialect = hinted;
            out.baud = hint.baud != 0 ? hint.baud : bauds.front();
            return true;
        }
        --index;
    }
    const size_t baud_count = bauds.size();
    if (baud_count == 0 || index >= dialects.size() * baud_count) {
        return false;
    }
    out.dialect = dialects[index / baud_count];
    out.baud = bauds[index % baud_count];
    return true;
}

bool reached(uint32_t now_ms, uint32_t deadline_ms_ms) noexcept
{
    return static_cast<int32_t>(now_ms - deadline_ms_ms) >= 0;
}

} // namespace

bool Controller::init(std::span<const Dialect *const> dialects, std::span<const uint32_t> bauds, const RoverConfig &cfg,
                      const Hint &hint, const Timing &timing) noexcept
{
    if (dialects.empty() || bauds.empty()) {
        return false;
    }
    for (const Dialect *dialect : dialects) {
        if (dialect == nullptr) {
            return false;
        }
    }
    for (const uint32_t baud : bauds) {
        if (baud == 0) {
            return false;
        }
    }
    dialects_ = dialects;
    bauds_ = bauds;
    cfg_ = &cfg;
    hint_ = hint;
    timing_ = timing;
    host_baud_ = bauds.front();
    state_ = State::kOff;
    phase_ = 0;
    probe_index_ = 0;
    probe_round_ = 0;
    step_index_ = 0;
    step_tries_ = 0;
    waiting_reply_ = false;
    reply_ok_ = false;
    reply_nack_ = false;
    probe_hit_ = false;
    detected_ = Detected{};
    stats_ = ControllerStats{};
    return true;
}

void Controller::start(uint32_t now_ms) noexcept
{
    state_ = State::kPowerUp;
    phase_ = 0;
    probe_index_ = 0;
    probe_round_ = 0;
    probe_hit_ = false;
    waiting_reply_ = false;
    detected_ = Detected{};
    last_frame_ms_ = now_ms;
    deadline_ms_ = now_ms;
}

void Controller::stop(uint32_t now_ms) noexcept
{
    state_ = State::kOff;
    phase_ = 1; // 下一轮 poll 先发一次断电动作
    waiting_reply_ = false;
    deadline_ms_ = now_ms;
}

void Controller::request_reprobe(uint32_t now_ms) noexcept
{
    if (state_ != State::kOff) {
        ++stats_.reprobes;
    }
    state_ = State::kProbing;
    phase_ = 0;
    probe_index_ = 0;
    probe_round_ = 0;
    probe_hit_ = false;
    waiting_reply_ = false;
    detected_ = Detected{};
    last_frame_ms_ = now_ms;
}

void Controller::request_reconfigure(uint32_t now_ms) noexcept
{
    if (detected_.dialect == nullptr) {
        request_reprobe(now_ms);
        return;
    }
    state_ = State::kConfiguring;
    phase_ = 0;
    step_index_ = 0;
    step_tries_ = 0;
    waiting_reply_ = false;
    reply_ok_ = false;
    reply_nack_ = false;
}

Action Controller::poll(uint32_t now_ms) noexcept
{
    if (state_ == State::kOff) {
        if (phase_ == 1) {
            phase_ = 0;
            Action action{};
            action.kind = Action::Kind::kPower;
            action.on = false;
            return action;
        }
        return Action{};
    }

    for (uint8_t guard = 0; guard < 16; ++guard) {
        switch (state_) {
        case State::kOff:
            return Action{};

        case State::kPowerUp:
            if (phase_ == 0) {
                deadline_ms_ = now_ms + timing_.power_off_ms;
                phase_ = 1;
                Action action{};
                action.kind = Action::Kind::kPower;
                action.on = false;
                return action;
            }
            if (phase_ == 1) {
                if (!reached(now_ms, deadline_ms_)) {
                    return Action{};
                }
                phase_ = 2;
                deadline_ms_ = now_ms + timing_.boot_ms;
                Action action{};
                action.kind = Action::Kind::kPower;
                action.on = true;
                return action;
            }
            if (!reached(now_ms, deadline_ms_)) {
                return Action{};
            }
            state_ = State::kProbing;
            phase_ = 0;
            probe_index_ = 0;
            probe_round_ = 0;
            probe_hit_ = false;
            waiting_reply_ = false;
            continue;

        case State::kProbing:
            if (phase_ == 0) {
                const size_t total = candidate_count(dialects_, bauds_, hint_);
                if (total == 0) {
                    state_ = State::kFailed;
                    ++stats_.failures;
                    deadline_ms_ = now_ms + timing_.retry_backoff_ms;
                    continue;
                }
                if (probe_index_ >= total) {
                    ++probe_round_; // 完成一轮全表探测
                    probe_index_ = 0;
                    if (probe_round_ >= timing_.probe_rounds) {
                        state_ = State::kFailed;
                        ++stats_.failures;
                        deadline_ms_ = now_ms + timing_.retry_backoff_ms;
                        continue;
                    }
                }
                Candidate candidate{};
                if (!candidate_at(dialects_, bauds_, hint_, probe_index_, candidate)) {
                    state_ = State::kFailed;
                    ++stats_.failures;
                    deadline_ms_ = now_ms + timing_.retry_backoff_ms;
                    continue;
                }
                if (host_baud_ != candidate.baud) {
                    host_baud_ = candidate.baud;
                    Action action{};
                    action.kind = Action::Kind::kSetBaud;
                    action.baud = candidate.baud;
                    return action;
                }
                probe_dialect_ = candidate.dialect;
                probe_cmd_.clear();
                if (!candidate.dialect->build_probe(probe_cmd_)) {
                    ++probe_index_; // 方言自身无法生成查询命令：跳过该组合
                    continue;
                }
                ++stats_.probes_sent;
                waiting_reply_ = true;
                probe_hit_ = false;
                phase_ = 1;
                deadline_ms_ = now_ms + timing_.probe_timeout_ms;
                Action action{};
                action.kind = Action::Kind::kWrite;
                action.bytes = probe_cmd_.view();
                return action;
            }
            if (probe_hit_) {
                detected_.dialect = probe_dialect_;
                detected_.baud = host_baud_;
                detected_.caps = probe_dialect_ != nullptr ? probe_dialect_->caps(detected_.probe.model) : 0U;
                state_ = State::kConfiguring;
                phase_ = 0;
                step_index_ = 0;
                step_tries_ = 0;
                waiting_reply_ = false;
                reply_ok_ = false;
                reply_nack_ = false;
                probe_hit_ = false;
                continue;
            }
            if (!reached(now_ms, deadline_ms_)) {
                return Action{};
            }
            waiting_reply_ = false;
            ++probe_index_;
            phase_ = 0;
            continue;

        case State::kConfiguring:
            if (cfg_ == nullptr || detected_.dialect == nullptr) {
                state_ = State::kFailed;
                ++stats_.failures;
                deadline_ms_ = now_ms + timing_.retry_backoff_ms;
                continue;
            }
            if (phase_ == 0) {
                ConfigStep step{};
                if (!detected_.dialect->rover_step(step_index_, detected_.probe.model, *cfg_, step)) {
                    state_ = State::kRunning;
                    last_frame_ms_ = now_ms;
                    phase_ = 0;
                    continue;
                }
                step_ = step; // 命令字节必须在应答到达前一直有效
                ++stats_.steps_sent;
                waiting_reply_ = step_.expect_reply;
                reply_ok_ = false;
                reply_nack_ = false;
                deadline_ms_ = now_ms + step_.timeout_ms;
                phase_ = 1;
                Action action{};
                action.kind = Action::Kind::kWrite;
                action.bytes = step_.cmd.view();
                return action;
            }
            if (phase_ == 1 && waiting_reply_) {
                if (reply_ok_ || reply_nack_) {
                    const bool ok = reply_ok_ && !reply_nack_;
                    waiting_reply_ = false;
                    if (!ok) {
                        ++stats_.step_nacks;
                        if (step_tries_ < timing_.step_retries) {
                            ++step_tries_;
                            phase_ = 0;
                            continue;
                        }
                        state_ = State::kFailed;
                        ++stats_.failures;
                        deadline_ms_ = now_ms + timing_.retry_backoff_ms;
                        continue;
                    }
                    if (step_.switch_baud != 0) {
                        phase_ = 2;
                        continue;
                    }
                    ++step_index_;
                    step_tries_ = 0;
                    phase_ = 0;
                    continue;
                }
                if (!reached(now_ms, deadline_ms_)) {
                    return Action{};
                }
                ++stats_.step_timeouts;
                waiting_reply_ = false;
                if (step_tries_ < timing_.step_retries) {
                    ++step_tries_;
                    phase_ = 0;
                    continue;
                }
                state_ = State::kFailed;
                ++stats_.failures;
                deadline_ms_ = now_ms + timing_.retry_backoff_ms;
                continue;
            }
            // 无需应答的步骤：发送后立即推进；需要改波特率的在 phase 2 处理。
            if (phase_ == 1) {
                if (step_.switch_baud != 0) {
                    phase_ = 2;
                    continue;
                }
                ++step_index_;
                step_tries_ = 0;
                phase_ = 0;
                continue;
            }
            if (phase_ == 2) {
                host_baud_ = step_.switch_baud;
                detected_.baud = host_baud_; // 探测结论里的“当前波特率”跟随配置切换
                ++step_index_;
                step_tries_ = 0;
                phase_ = 0;
                Action action{};
                action.kind = Action::Kind::kSetBaud;
                action.baud = host_baud_;
                return action;
            }
            return Action{};

        case State::kRunning:
            if (static_cast<int32_t>(now_ms - last_frame_ms_) > static_cast<int32_t>(timing_.silence_ms)) {
                ++stats_.reprobes;
                detected_ = Detected{};
                state_ = State::kProbing;
                phase_ = 0;
                probe_index_ = 0;
                probe_round_ = 0;
                probe_hit_ = false;
                waiting_reply_ = false;
                continue;
            }
            return Action{};

        case State::kFailed:
            if (!reached(now_ms, deadline_ms_)) {
                return Action{};
            }
            state_ = State::kPowerUp;
            phase_ = 0;
            continue;

        default:
            return Action{};
        }
    }
    return Action{};
}

void Controller::on_frame(const demux::Decoder &dec, std::span<const uint8_t> frame, uint32_t now_ms) noexcept
{
    last_frame_ms_ = now_ms; // 任意协议帧都算“模块还在说话”
    if (state_ == State::kProbing && phase_ == 1 && probe_dialect_ != nullptr) {
        ProbeResult result{};
        if (probe_dialect_->match_probe(dec, frame, result)) {
            detected_.probe = result;
            detected_.dialect = probe_dialect_;
            detected_.baud = host_baud_;
            probe_hit_ = true;
        }
        return;
    }
    if (state_ == State::kConfiguring && phase_ == 1 && waiting_reply_ && detected_.dialect != nullptr) {
        const Reply reply = detected_.dialect->match_reply(dec, frame, step_.cmd.view());
        if (reply == Reply::kAck) {
            reply_ok_ = true;
        } else if (reply == Reply::kNack) {
            reply_nack_ = true;
        }
    }
}

const char *state_name(State state) noexcept
{
    switch (state) {
    case State::kOff:
        return "off";
    case State::kPowerUp:
        return "power-up";
    case State::kProbing:
        return "probing";
    case State::kConfiguring:
        return "configuring";
    case State::kRunning:
        return "running";
    case State::kFailed:
        return "failed";
    default:
        return "unknown";
    }
}

} // namespace gnss
