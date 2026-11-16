// rtk_spp_peer 重连状态机 C 桥接接口（供 Peer.c 调用 reconnect::Reconnector）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PEER_RC_ADDR_LEN 6U

typedef enum {
    PEER_RC_STATE_IDLE = 0,
    PEER_RC_STATE_DISCOVERING = 1,
    PEER_RC_STATE_CONNECTING = 2,
    PEER_RC_STATE_CONNECTED = 3,
    PEER_RC_STATE_BACKOFF = 4,
    PEER_RC_STATE_STOPPED = 5,
} peer_rc_state_t;

typedef enum {
    PEER_RC_ACT_NONE = 0,
    PEER_RC_ACT_START_SDP = 1,
    PEER_RC_ACT_CONNECT = 2,
} peer_rc_action_type_t;

typedef struct {
    peer_rc_action_type_t type;
    uint8_t scn;
} peer_rc_action_t;

typedef struct {
    peer_rc_state_t state;
    uint32_t next_attempt_in_ms;
    uint32_t attempts;
    uint32_t sdp_queries;
    uint32_t failures;
    uint32_t connects;
    uint32_t disconnects;
    uint32_t last_delay_ms;
    bool has_target;
    uint8_t target[PEER_RC_ADDR_LEN];
} peer_rc_status_t;

/** 初始化配置与随机种子；仅由 worker 单线程调用。 */
void peer_rc_init(uint32_t seed);
void peer_rc_set_target(const uint8_t addr[PEER_RC_ADDR_LEN]);
bool peer_rc_has_target(void);
bool peer_rc_get_target(uint8_t out_addr[PEER_RC_ADDR_LEN]);
bool peer_rc_start(uint32_t now_ms, peer_rc_action_t *out);
void peer_rc_stop(void);
peer_rc_action_t peer_rc_poll(uint32_t now_ms);
peer_rc_action_t peer_rc_on_sdp_result(bool ok, uint8_t scn, uint32_t now_ms);
peer_rc_action_t peer_rc_on_open(uint32_t now_ms);
peer_rc_action_t peer_rc_on_connect_failed(uint32_t now_ms);
peer_rc_action_t peer_rc_on_close(uint32_t now_ms);
void peer_rc_get_status(uint32_t now_ms, peer_rc_status_t *out);
const char *peer_rc_state_name(peer_rc_state_t state);

#ifdef __cplusplus
}
#endif
