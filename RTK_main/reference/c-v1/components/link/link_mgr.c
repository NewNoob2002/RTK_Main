/**
 * @file    link_mgr.c
 * @brief   SPP 会话角色与策略管理（纯 C，无 IDF 依赖）。
 *
 * 规则编号与 components/link/include/link_mgr.h 中的注释一一对应。
 * 本模块不调用蓝牙 API，只通过 link_actions_t 报告需要断开的句柄。
 * 所有时间比较使用无符号差值，容忍 32 位毫秒回绕。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <stddef.h>

#include "link_mgr.h"

#define LINK_ACT_CAPACITY (LINK_MAX_SESSIONS + 1U)

static bool
addr_equal (link_addr_t const * p_a, link_addr_t const * p_b)
{
    for (uint32_t idx = 0U; idx < LINK_ADDR_LEN; idx++)
    {
        if (p_a->addr[idx] != p_b->addr[idx])
        {
            return false;
        }
    }
    return true;
}

static link_session_t *
find_session (link_mgr_t * p_mgr, uint32_t handle)
{
    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        link_session_t * p_session = &p_mgr->sessions[idx];

        if ((p_session->b_in_use) && (p_session->handle == handle))
        {
            return p_session;
        }
    }
    return NULL;
}

static link_session_t *
alloc_session (link_mgr_t * p_mgr)
{
    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        if (!p_mgr->sessions[idx].b_in_use)
        {
            return &p_mgr->sessions[idx];
        }
    }
    return NULL;
}

static bool
has_phone (link_mgr_t const * p_mgr)
{
    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        link_session_t const * p_session = &p_mgr->sessions[idx];

        if ((p_session->b_in_use) && (LINK_ROLE_PHONE == p_session->role))
        {
            return true;
        }
    }
    return false;
}

static void
action_add (link_actions_t * p_act, uint32_t handle)
{
    /* count ≤ LINK_MAX_SESSIONS+1，远小于 uint32 上限 */
    uint32_t count = (uint32_t) p_act->count;

    if (count < LINK_ACT_CAPACITY)
    {
        p_act->handles[count] = handle;
        /* count+1 ≤ 3，u8 截断安全 */
        p_act->count = (uint8_t) (count + 1U);
    }
}

static void
action_clear (link_actions_t * p_act)
{
    p_act->count = 0U;
}

static bool
addr_in_whitelist (link_mgr_t const * p_mgr, link_addr_t const * p_addr)
{
    /* alarm_count ≤ LINK_MAX_ALARMS，转换为 uint32 仅扩宽 */
    uint32_t count = (uint32_t) p_mgr->alarm_count;

    for (uint32_t idx = 0U; idx < count; idx++)
    {
        if (addr_equal(&p_mgr->alarms[idx], p_addr))
        {
            return true;
        }
    }
    return false;
}

/* 追加白名单；已存在视为成功，容量不足返回 false。 */
static bool
whitelist_add (link_mgr_t * p_mgr, link_addr_t const * p_addr)
{
    /* alarm_count ≤ LINK_MAX_ALARMS，转换为 uint32 仅扩宽 */
    uint32_t count = (uint32_t) p_mgr->alarm_count;

    if (addr_in_whitelist(p_mgr, p_addr))
    {
        return true;
    }
    if (count >= LINK_MAX_ALARMS)
    {
        return false;
    }

    p_mgr->alarms[count] = *p_addr;
    /* count+1 ≤ LINK_MAX_ALARMS(4)，u8 截断安全 */
    p_mgr->alarm_count = (uint8_t) (count + 1U);
    return true;
}

void
link_mgr_init (link_mgr_t * p_mgr, link_policy_t policy,
               link_addr_t const * p_alarms, uint8_t count)
{
    uint32_t limit = 0U;

    if (NULL == p_mgr)
    {
        return;
    }

    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        link_session_t * p_session = &p_mgr->sessions[idx];

        p_session->b_in_use   = false;
        p_session->handle     = 0U;
        p_session->role       = LINK_ROLE_NONE;
        p_session->since_ms   = 0U;
        p_session->last_rx_ms = 0U;
        for (uint32_t byte = 0U; byte < LINK_ADDR_LEN; byte++)
        {
            p_session->addr.addr[byte] = 0U;
        }
    }
    for (uint32_t idx = 0U; idx < LINK_MAX_ALARMS; idx++)
    {
        for (uint32_t byte = 0U; byte < LINK_ADDR_LEN; byte++)
        {
            p_mgr->alarms[idx].addr[byte] = 0U;
        }
    }

    p_mgr->alarm_count   = 0U;
    p_mgr->policy        = policy;
    p_mgr->b_pairing     = false;
    p_mgr->pair_start_ms = 0U;

    if (NULL == p_alarms)
    {
        return;
    }

    /* count ≤ 255，转换为 uint32 仅扩宽 */
    limit = (uint32_t) count;
    if (limit > LINK_MAX_ALARMS)
    {
        limit = LINK_MAX_ALARMS;
    }
    for (uint32_t idx = 0U; idx < limit; idx++)
    {
        p_mgr->alarms[idx] = p_alarms[idx];
    }
    /* limit ≤ LINK_MAX_ALARMS(4)，u8 截断安全 */
    p_mgr->alarm_count = (uint8_t) limit;
}

link_role_t
link_mgr_on_open (link_mgr_t * p_mgr, uint32_t handle,
                  link_addr_t const * p_addr, uint32_t now_ms,
                  link_actions_t * p_act)
{
    link_session_t * p_session = NULL;
    link_role_t      role      = LINK_ROLE_NONE;

    if ((NULL == p_mgr) || (NULL == p_addr) || (NULL == p_act))
    {
        return LINK_ROLE_NONE;
    }
    action_clear(p_act);

    /* 规则 1：重复句柄拒绝。 */
    if (NULL != find_session(p_mgr, handle))
    {
        action_add(p_act, handle);
        return LINK_ROLE_NONE;
    }

    /* 规则 2：会话表已满拒绝。 */
    p_session = alloc_session(p_mgr);
    if (NULL == p_session)
    {
        action_add(p_act, handle);
        return LINK_ROLE_NONE;
    }

    if (addr_in_whitelist(p_mgr, p_addr))
    {
        /* 规则 3。 */
        if ((LINK_POLICY_PHONE_PREEMPT == p_mgr->policy) && has_phone(p_mgr))
        {
            action_add(p_act, handle);
            return LINK_ROLE_NONE;
        }
        role = LINK_ROLE_ALARM;
    }
    else if (!has_phone(p_mgr))
    {
        /* 规则 4 第一支：无手机时未知设备作为手机。 */
        role = LINK_ROLE_PHONE;
        if (LINK_POLICY_PHONE_PREEMPT == p_mgr->policy)
        {
            for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
            {
                link_session_t * p_old = &p_mgr->sessions[idx];

                if ((p_old->b_in_use) && (LINK_ROLE_ALARM == p_old->role))
                {
                    action_add(p_act, p_old->handle);
                    p_old->b_in_use = false;
                }
            }
        }
    }
    else if (p_mgr->b_pairing)
    {
        /* 规则 4 第二支：手机在线且配对窗口打开。 */
        role = LINK_ROLE_PENDING;
    }
    else
    {
        action_add(p_act, handle);
        return LINK_ROLE_NONE;
    }

    p_session->b_in_use   = true;
    p_session->handle     = handle;
    p_session->role       = role;
    p_session->addr       = *p_addr;
    p_session->since_ms   = now_ms;
    p_session->last_rx_ms = now_ms;
    return role;
}

void
link_mgr_on_close (link_mgr_t * p_mgr, uint32_t handle)
{
    link_session_t * p_session = NULL;

    if (NULL == p_mgr)
    {
        return;
    }

    p_session = find_session(p_mgr, handle);
    if (NULL != p_session)
    {
        p_session->b_in_use = false;
    }
}

link_role_t
link_mgr_on_frame (link_mgr_t * p_mgr, uint32_t handle, uint8_t sender,
                   uint16_t msg_id, uint32_t now_ms, link_actions_t * p_act)
{
    link_session_t * p_session = NULL;

    if ((NULL == p_mgr) || (NULL == p_act))
    {
        return LINK_ROLE_NONE;
    }
    action_clear(p_act);

    p_session = find_session(p_mgr, handle);
    if (NULL == p_session)
    {
        return LINK_ROLE_NONE;
    }

    p_session->last_rx_ms = now_ms;

    if ((LINK_HELLO_MSG_ID == msg_id) && (LINK_HELLO_SENDER == sender))
    {
        if (LINK_ROLE_ALARM == p_session->role)
        {
            return LINK_ROLE_ALARM;
        }
        if (!p_mgr->b_pairing)
        {
            /* 配对窗口外收到 HELLO：未授权报警器，断开。 */
            action_add(p_act, handle);
            p_session->b_in_use = false;
            return LINK_ROLE_NONE;
        }
        if (!whitelist_add(p_mgr, &p_session->addr))
        {
            /* 白名单已满：拒绝并断开。 */
            action_add(p_act, handle);
            p_session->b_in_use = false;
            return LINK_ROLE_NONE;
        }
        p_session->role     = LINK_ROLE_ALARM;
        p_session->since_ms = now_ms;
        return LINK_ROLE_ALARM;
    }

    if (LINK_ROLE_PENDING == p_session->role)
    {
        /* 等待 HELLO 的会话收到其他帧：不允许第二部手机，断开。 */
        action_add(p_act, handle);
        p_session->b_in_use = false;
        return LINK_ROLE_NONE;
    }

    return p_session->role;
}

void
link_mgr_tick (link_mgr_t * p_mgr, uint32_t now_ms, link_actions_t * p_act)
{
    if ((NULL == p_mgr) || (NULL == p_act))
    {
        return;
    }
    action_clear(p_act);

    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        link_session_t * p_session = &p_mgr->sessions[idx];

        if (!p_session->b_in_use)
        {
            continue;
        }

        if (LINK_ROLE_ALARM == p_session->role)
        {
            if ((uint32_t) (now_ms - p_session->last_rx_ms) >
                LINK_ALARM_RX_TIMEOUT_MS)
            {
                action_add(p_act, p_session->handle);
                p_session->b_in_use = false;
            }
        }
        else if (LINK_ROLE_PENDING == p_session->role)
        {
            if ((uint32_t) (now_ms - p_session->since_ms) >
                LINK_PENDING_TIMEOUT_MS)
            {
                action_add(p_act, p_session->handle);
                p_session->b_in_use = false;
            }
        }
        else
        {
            /* PHONE 不因接收超时断开。 */
        }
    }

    if ((p_mgr->b_pairing) &&
        ((uint32_t) (now_ms - p_mgr->pair_start_ms) >= LINK_PAIR_WINDOW_MS))
    {
        p_mgr->b_pairing = false;
    }
}

link_role_t
link_mgr_role (link_mgr_t const * p_mgr, uint32_t handle)
{
    if (NULL == p_mgr)
    {
        return LINK_ROLE_NONE;
    }

    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        link_session_t const * p_session = &p_mgr->sessions[idx];

        if ((p_session->b_in_use) && (p_session->handle == handle))
        {
            return p_session->role;
        }
    }
    return LINK_ROLE_NONE;
}

uint8_t
link_mgr_handles (link_mgr_t const * p_mgr, link_role_t role, uint32_t * p_out,
                  uint8_t max)
{
    uint32_t count = 0U;
    /* max ≤ 255，转换为 uint32 仅扩宽 */
    uint32_t limit = (uint32_t) max;

    if ((NULL == p_mgr) || (NULL == p_out))
    {
        return 0U;
    }

    for (uint32_t idx = 0U; idx < LINK_MAX_SESSIONS; idx++)
    {
        link_session_t const * p_session = &p_mgr->sessions[idx];

        if ((p_session->b_in_use) && (p_session->role == role) &&
            (count < limit))
        {
            p_out[count] = p_session->handle;
            count++;
        }
    }

    /* count ≤ LINK_MAX_SESSIONS(2)，u8 截断安全 */
    return (uint8_t) count;
}

void
link_mgr_pair_open (link_mgr_t * p_mgr, uint32_t now_ms)
{
    if (NULL == p_mgr)
    {
        return;
    }

    p_mgr->b_pairing     = true;
    p_mgr->pair_start_ms = now_ms;
}

bool
link_mgr_is_pairing (link_mgr_t const * p_mgr)
{
    if (NULL == p_mgr)
    {
        return false;
    }
    return p_mgr->b_pairing;
}

void
link_mgr_whitelist_clear (link_mgr_t * p_mgr)
{
    if (NULL == p_mgr)
    {
        return;
    }

    p_mgr->alarm_count = 0U;
}

uint8_t
link_mgr_whitelist_get (link_mgr_t const * p_mgr, link_addr_t * p_out,
                        uint8_t max)
{
    uint32_t count = 0U;
    /* max ≤ 255，转换为 uint32 仅扩宽 */
    uint32_t limit = (uint32_t) max;

    if ((NULL == p_mgr) || (NULL == p_out))
    {
        return 0U;
    }

    /* alarm_count ≤ LINK_MAX_ALARMS，转换为 uint32 仅扩宽 */
    count = (uint32_t) p_mgr->alarm_count;
    if (count > limit)
    {
        count = limit;
    }
    for (uint32_t idx = 0U; idx < count; idx++)
    {
        p_out[idx] = p_mgr->alarms[idx];
    }

    /* count ≤ LINK_MAX_ALARMS(4)，u8 截断安全 */
    return (uint8_t) count;
}

/*** end of file ***/
