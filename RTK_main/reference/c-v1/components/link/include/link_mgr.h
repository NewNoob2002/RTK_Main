/**
 * @file    link_mgr.h
 * @brief   SPP 会话角色与策略管理（纯 C，无 IDF 依赖）。
 *
 * 决定每个 SPP 会话是手机还是报警器、是否允许接入、何时断开。
 * 本模块不调用蓝牙 API，只通过 link_actions_t 告诉调用方要断开哪些句柄。
 * 时间由调用方以 now_ms 传入，所有时间比较使用无符号差值以容忍回绕。
 * 冻结接口，实现方不得修改。
 */

#ifndef LINK_MGR_H
#define LINK_MGR_H

#include <stdbool.h>
#include <stdint.h>

#define LINK_MAX_SESSIONS (2U)           /* 与 BR/EDR ACL 上限一致 */
#define LINK_MAX_ALARMS (4U)             /* 白名单容量 */
#define LINK_ALARM_RX_TIMEOUT_MS (3000U) /* ALARM 会话无有效帧即断开 */
#define LINK_PENDING_TIMEOUT_MS (5000U)  /* PENDING 会话等待 HELLO 的时限 */
#define LINK_PAIR_WINDOW_MS (60000U)     /* 配对窗口 */
#define LINK_ADDR_LEN (6U)
#define LINK_HELLO_MSG_ID (0x0041U)
#define LINK_HELLO_SENDER (0x03U)

typedef enum
{
    LINK_ROLE_NONE = 0, /* 句柄不存在 */
    LINK_ROLE_PHONE,
    LINK_ROLE_ALARM,
    LINK_ROLE_PENDING /* 已有手机时新接入的未知设备，等待 HELLO */
} link_role_t;

typedef enum
{
    LINK_POLICY_CONCURRENT = 0, /* 手机与报警器可同时在线 */
    LINK_POLICY_PHONE_PREEMPT   /* 手机优先：手机接入时踢掉报警器；
                                   手机在线时拒绝报警器 */
} link_policy_t;

typedef struct
{
    uint8_t addr[LINK_ADDR_LEN];
} link_addr_t;

typedef struct
{
    bool        b_in_use;
    uint32_t    handle;
    link_role_t role;
    link_addr_t addr;
    uint32_t    since_ms;   /* 进入当前角色的时间 */
    uint32_t    last_rx_ms; /* 最近一次有效帧时间 */
} link_session_t;

typedef struct
{
    link_session_t sessions[LINK_MAX_SESSIONS];
    link_addr_t    alarms[LINK_MAX_ALARMS];
    uint8_t        alarm_count;
    link_policy_t  policy;
    bool           b_pairing;
    uint32_t       pair_start_ms;
} link_mgr_t;

/* 本次调用要求调用方断开的句柄（按产生顺序）。 */
typedef struct
{
    uint32_t handles[LINK_MAX_SESSIONS + 1U];
    uint8_t  count;
} link_actions_t;

/** p_alarms 可为 NULL（count 必须为 0）；count 超过容量时只取前 LINK_MAX_ALARMS
 * 个。 */
void link_mgr_init (link_mgr_t * p_mgr, link_policy_t policy,
                    link_addr_t const * p_alarms, uint8_t count);

/**
 * 新 SPP 连接建立。返回分配的角色；返回 LINK_ROLE_NONE 表示拒绝，
 * 且 p_act 中包含该 handle。p_act 每次调用先清零。规则（按顺序）：
 * 1. 参数为 NULL 或 handle 已存在：拒绝。
 * 2. 会话表已满：拒绝。
 * 3. 地址在白名单：
 *    - PHONE_PREEMPT 且已有 PHONE：拒绝；
 *    - 否则 ALARM。
 * 4. 地址不在白名单：
 *    - 当前无 PHONE：PHONE；PHONE_PREEMPT 时同时把所有 ALARM 句柄放入 p_act；
 *    - 已有 PHONE 且配对窗口打开：PENDING；
 *    - 否则拒绝。
 * 新会话的 since_ms 与 last_rx_ms 都置为 now_ms。
 * 放入 p_act 的已有会话（被抢占的 ALARM）立即从会话表移除。
 */
link_role_t link_mgr_on_open (link_mgr_t * p_mgr, uint32_t handle,
                              link_addr_t const * p_addr, uint32_t now_ms,
                              link_actions_t * p_act);

/** 连接关闭（无论哪一方发起）。未知句柄忽略。 */
void link_mgr_on_close (link_mgr_t * p_mgr, uint32_t handle);

/**
 * 某会话收到一帧有效帧（CRC 正确）。返回处理后的角色。p_act 每次先清零。
 * - 刷新 last_rx_ms。
 * - 若 msg_id == LINK_HELLO_MSG_ID 且 sender == LINK_HELLO_SENDER：
 *   - ALARM：无变化；
 *   - PHONE 或 PENDING 且配对窗口打开：改为 ALARM，地址加入白名单
 *     （已存在不重复；白名单满时拒绝：断开该句柄，返回 LINK_ROLE_NONE
 *     前先从会话表移除），since_ms = now_ms；
 *   - PHONE 或 PENDING 且配对窗口未打开：断开该句柄（未授权报警器）。
 *   以上断开的情况：句柄放入 p_act，会话移除，返回 LINK_ROLE_NONE。
 * - 其他帧来自 PENDING：断开（不允许第二部手机），返回 LINK_ROLE_NONE。
 * - 未知句柄：返回 LINK_ROLE_NONE，p_act 为空。
 */
link_role_t link_mgr_on_frame (link_mgr_t * p_mgr, uint32_t handle,
                               uint8_t sender, uint16_t msg_id, uint32_t now_ms,
                               link_actions_t * p_act);

/**
 * 周期调用（建议 100 ms）。p_act 每次先清零。
 * - ALARM 会话 now - last_rx_ms > LINK_ALARM_RX_TIMEOUT_MS：断开。
 * - PENDING 会话 now - since_ms > LINK_PENDING_TIMEOUT_MS：断开。
 * - 配对窗口 now - pair_start_ms >= LINK_PAIR_WINDOW_MS：关闭窗口。
 * 被断开的会话立即从会话表移除（之后的 on_close 被忽略）。
 */
void link_mgr_tick (link_mgr_t * p_mgr, uint32_t now_ms,
                    link_actions_t * p_act);

link_role_t link_mgr_role (link_mgr_t const * p_mgr, uint32_t handle);

/** 把指定角色的句柄写入 p_out（最多 max 个），返回写入数量。 */
uint8_t link_mgr_handles (link_mgr_t const * p_mgr, link_role_t role,
                          uint32_t * p_out, uint8_t max);

/** 打开配对窗口（重复调用重新计时）。 */
void link_mgr_pair_open (link_mgr_t * p_mgr, uint32_t now_ms);
bool link_mgr_is_pairing (link_mgr_t const * p_mgr);

/** 清空白名单。不影响已连接会话的角色。 */
void link_mgr_whitelist_clear (link_mgr_t * p_mgr);

/** 读取白名单，返回写入数量。 */
uint8_t link_mgr_whitelist_get (link_mgr_t const * p_mgr, link_addr_t * p_out,
                                uint8_t max);

#endif /* LINK_MGR_H */

/*** end of file ***/
