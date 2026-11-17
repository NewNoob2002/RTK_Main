// rtk_spp_peer 重连状态机 C/C++ 桥接实现（封装 reconnect::Reconnector，仅由 worker 任务单线程调用）。

#include "ReconnectBridge.h"

#include <algorithm>
#include <cstdint>

#include "reconnect/Reconnect.hpp"

namespace {

static_assert(PEER_RC_STATE_IDLE == static_cast<uint8_t>(reconnect::State::kIdle));
static_assert(PEER_RC_STATE_DISCOVERING == static_cast<uint8_t>(reconnect::State::kDiscovering));
static_assert(PEER_RC_STATE_CONNECTING == static_cast<uint8_t>(reconnect::State::kConnecting));
static_assert(PEER_RC_STATE_CONNECTED == static_cast<uint8_t>(reconnect::State::kConnected));
static_assert(PEER_RC_STATE_BACKOFF == static_cast<uint8_t>(reconnect::State::kBackoff));
static_assert(PEER_RC_STATE_STOPPED == static_cast<uint8_t>(reconnect::State::kStopped));

constinit reconnect::Reconnector s_reconnector{};

peer_rc_action_t toCAction(const reconnect::Action &act) noexcept
{
    peer_rc_action_t out{};
    switch (act.type) {
    case reconnect::ActionType::kStartSdp:
        out.type = PEER_RC_ACT_START_SDP;
        out.scn = 0U;
        break;
    case reconnect::ActionType::kConnect:
        out.type = PEER_RC_ACT_CONNECT;
        out.scn = act.scn;
        break;
    case reconnect::ActionType::kNone:
    default:
        out.type = PEER_RC_ACT_NONE;
        out.scn = 0U;
        break;
    }
    return out;
}

peer_rc_state_t toCState(reconnect::State state) noexcept
{
    switch (state) {
    case reconnect::State::kIdle:
        return PEER_RC_STATE_IDLE;
    case reconnect::State::kDiscovering:
        return PEER_RC_STATE_DISCOVERING;
    case reconnect::State::kConnecting:
        return PEER_RC_STATE_CONNECTING;
    case reconnect::State::kConnected:
        return PEER_RC_STATE_CONNECTED;
    case reconnect::State::kBackoff:
        return PEER_RC_STATE_BACKOFF;
    case reconnect::State::kStopped:
        return PEER_RC_STATE_STOPPED;
    default:
        return PEER_RC_STATE_IDLE;
    }
}

} // namespace

extern "C" void peer_rc_init(uint32_t seed)
{
    reconnect::Config cfg{};
    // 单次尝试 6 s 无结果按失败处理：留出 LSTO(5 s) 余量，同时远小于协议栈 ~20 s 的 0x08 卡死
    cfg.attemptTimeoutMs = 6000U;
    static_cast<void>(s_reconnector.configure(cfg, seed));
}

extern "C" void peer_rc_set_target(const uint8_t addr[PEER_RC_ADDR_LEN])
{
    if (addr == nullptr) {
        return;
    }
    reconnect::Addr target{};
    std::copy_n(addr, reconnect::kAddrLen, target.begin());
    s_reconnector.setTarget(target);
}

extern "C" bool peer_rc_has_target(void)
{
    return s_reconnector.hasTarget();
}

extern "C" bool peer_rc_get_target(uint8_t outAddr[PEER_RC_ADDR_LEN])
{
    if ((outAddr == nullptr) || (!s_reconnector.hasTarget())) {
        return false;
    }
    const reconnect::Addr &target = s_reconnector.target();
    std::copy_n(target.begin(), reconnect::kAddrLen, outAddr);
    return true;
}

extern "C" bool peer_rc_start(uint32_t nowMs, peer_rc_action_t *out)
{
    reconnect::Action act{};
    const bool ok = s_reconnector.start(nowMs, act);
    if (out != nullptr) {
        *out = toCAction(act);
    }
    return ok;
}

extern "C" void peer_rc_stop(void)
{
    s_reconnector.stop();
}

extern "C" peer_rc_action_t peer_rc_poll(uint32_t nowMs)
{
    return toCAction(s_reconnector.poll(nowMs));
}

extern "C" peer_rc_action_t peer_rc_on_sdp_result(bool ok, uint8_t scn, uint32_t nowMs)
{
    return toCAction(s_reconnector.onSdpResult(ok, scn, nowMs));
}

extern "C" peer_rc_action_t peer_rc_on_open(uint32_t nowMs)
{
    return toCAction(s_reconnector.onOpen(nowMs));
}

extern "C" peer_rc_action_t peer_rc_on_connect_failed(uint32_t nowMs)
{
    return toCAction(s_reconnector.onConnectFailed(nowMs));
}

extern "C" peer_rc_action_t peer_rc_on_close(uint32_t nowMs)
{
    return toCAction(s_reconnector.onClose(nowMs));
}

extern "C" void peer_rc_get_status(uint32_t nowMs, peer_rc_status_t *out)
{
    if (out == nullptr) {
        return;
    }
    const reconnect::Stats &st = s_reconnector.stats();
    out->state = toCState(s_reconnector.state());
    out->next_attempt_in_ms = s_reconnector.nextAttemptIn(nowMs);
    out->attempts = st.attempts;
    out->sdp_queries = st.sdpQueries;
    out->failures = st.failures;
    out->connects = st.connects;
    out->disconnects = st.disconnects;
    out->last_delay_ms = st.lastDelayMs;
    out->has_target = s_reconnector.hasTarget();
    if (out->has_target) {
        const reconnect::Addr &target = s_reconnector.target();
        std::copy_n(target.begin(), reconnect::kAddrLen, out->target);
    } else {
        std::fill_n(out->target, PEER_RC_ADDR_LEN, 0U);
    }
}

extern "C" const char *peer_rc_state_name(peer_rc_state_t state)
{
    switch (state) {
    case PEER_RC_STATE_IDLE:
        return "idle";
    case PEER_RC_STATE_DISCOVERING:
        return "discovering";
    case PEER_RC_STATE_CONNECTING:
        return "connecting";
    case PEER_RC_STATE_CONNECTED:
        return "connected";
    case PEER_RC_STATE_BACKOFF:
        return "backoff";
    case PEER_RC_STATE_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}
