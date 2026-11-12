// rtk_spp_peer 重连状态机 C/C++ 桥接实现（封装 btrc::Reconnector，仅由 worker 任务单线程调用）。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "reconnect_bridge.h"

#include <algorithm>
#include <cstdint>

#include "bt_reconnect/reconnect.hpp"

namespace {

static_assert(PEER_RC_STATE_IDLE == static_cast<uint8_t>(btrc::State::kIdle));
static_assert(PEER_RC_STATE_DISCOVERING == static_cast<uint8_t>(btrc::State::kDiscovering));
static_assert(PEER_RC_STATE_CONNECTING == static_cast<uint8_t>(btrc::State::kConnecting));
static_assert(PEER_RC_STATE_CONNECTED == static_cast<uint8_t>(btrc::State::kConnected));
static_assert(PEER_RC_STATE_BACKOFF == static_cast<uint8_t>(btrc::State::kBackoff));
static_assert(PEER_RC_STATE_STOPPED == static_cast<uint8_t>(btrc::State::kStopped));

constinit btrc::Reconnector s_reconnector{};

peer_rc_action_t to_c_action(const btrc::Action &act) noexcept
{
    peer_rc_action_t out{};
    switch (act.type) {
    case btrc::ActionType::kStartSdp:
        out.type = PEER_RC_ACT_START_SDP;
        out.scn = 0U;
        break;
    case btrc::ActionType::kConnect:
        out.type = PEER_RC_ACT_CONNECT;
        out.scn = act.scn;
        break;
    case btrc::ActionType::kNone:
    default:
        out.type = PEER_RC_ACT_NONE;
        out.scn = 0U;
        break;
    }
    return out;
}

peer_rc_state_t to_c_state(btrc::State state) noexcept
{
    switch (state) {
    case btrc::State::kIdle:
        return PEER_RC_STATE_IDLE;
    case btrc::State::kDiscovering:
        return PEER_RC_STATE_DISCOVERING;
    case btrc::State::kConnecting:
        return PEER_RC_STATE_CONNECTING;
    case btrc::State::kConnected:
        return PEER_RC_STATE_CONNECTED;
    case btrc::State::kBackoff:
        return PEER_RC_STATE_BACKOFF;
    case btrc::State::kStopped:
        return PEER_RC_STATE_STOPPED;
    default:
        return PEER_RC_STATE_IDLE;
    }
}

} // namespace

extern "C" void peer_rc_init(uint32_t seed)
{
    btrc::Config cfg{};
    // 单次尝试 6 s 无结果按失败处理：留出 LSTO(5 s) 余量，同时远小于协议栈 ~20 s 的 0x08 卡死
    cfg.attempt_timeout_ms = 6000U;
    static_cast<void>(s_reconnector.configure(cfg, seed));
}

extern "C" void peer_rc_set_target(const uint8_t addr[PEER_RC_ADDR_LEN])
{
    if (addr == nullptr) {
        return;
    }
    btrc::Addr target{};
    std::copy_n(addr, btrc::kAddrLen, target.begin());
    s_reconnector.set_target(target);
}

extern "C" bool peer_rc_has_target(void)
{
    return s_reconnector.has_target();
}

extern "C" bool peer_rc_get_target(uint8_t out_addr[PEER_RC_ADDR_LEN])
{
    if ((out_addr == nullptr) || (!s_reconnector.has_target())) {
        return false;
    }
    const btrc::Addr &target = s_reconnector.target();
    std::copy_n(target.begin(), btrc::kAddrLen, out_addr);
    return true;
}

extern "C" bool peer_rc_start(uint32_t now_ms, peer_rc_action_t *out)
{
    btrc::Action act{};
    const bool ok = s_reconnector.start(now_ms, act);
    if (out != nullptr) {
        *out = to_c_action(act);
    }
    return ok;
}

extern "C" void peer_rc_stop(void)
{
    s_reconnector.stop();
}

extern "C" peer_rc_action_t peer_rc_poll(uint32_t now_ms)
{
    return to_c_action(s_reconnector.poll(now_ms));
}

extern "C" peer_rc_action_t peer_rc_on_sdp_result(bool ok, uint8_t scn, uint32_t now_ms)
{
    return to_c_action(s_reconnector.on_sdp_result(ok, scn, now_ms));
}

extern "C" peer_rc_action_t peer_rc_on_open(uint32_t now_ms)
{
    return to_c_action(s_reconnector.on_open(now_ms));
}

extern "C" peer_rc_action_t peer_rc_on_connect_failed(uint32_t now_ms)
{
    return to_c_action(s_reconnector.on_connect_failed(now_ms));
}

extern "C" peer_rc_action_t peer_rc_on_close(uint32_t now_ms)
{
    return to_c_action(s_reconnector.on_close(now_ms));
}

extern "C" void peer_rc_get_status(uint32_t now_ms, peer_rc_status_t *out)
{
    if (out == nullptr) {
        return;
    }
    const btrc::Stats &st = s_reconnector.stats();
    out->state = to_c_state(s_reconnector.state());
    out->next_attempt_in_ms = s_reconnector.next_attempt_in(now_ms);
    out->attempts = st.attempts;
    out->sdp_queries = st.sdp_queries;
    out->failures = st.failures;
    out->connects = st.connects;
    out->disconnects = st.disconnects;
    out->last_delay_ms = st.last_delay_ms;
    out->has_target = s_reconnector.has_target();
    if (out->has_target) {
        const btrc::Addr &target = s_reconnector.target();
        std::copy_n(target.begin(), btrc::kAddrLen, out->target);
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
