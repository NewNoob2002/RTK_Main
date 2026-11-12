// SPP 会话角色与策略管理（phone_protocol.md §2/§5，link_manager.hpp 规则 1–4）。
// 不调用传输层；需要断开的句柄经 Actions 返回给 LinkService。
// 所有时间比较用无符号差值，容忍 32 位毫秒回绕。

#include "core/link_manager.hpp"
#include "core/trace_codes.hpp"

namespace core {

namespace {

// 事件参数统一用 uint32_t；底层类型为 uint8_t 的枚举转宽不收窄。
constexpr uint32_t role_value(Role role) noexcept
{
    return static_cast<uint32_t>(role);
}

constexpr uint32_t reason_value(LinkReason reason) noexcept
{
    return static_cast<uint32_t>(reason);
}

void action_add(Actions &act, SessionHandle handle) noexcept
{
    if (act.count < act.handles.size()) {
        act.handles[act.count] = handle;
        ++act.count;
    }
}

bool contains_addr(std::span<const BtAddr> list, const BtAddr &addr) noexcept
{
    for (const BtAddr &entry : list) {
        if (entry == addr) {
            return true;
        }
    }
    return false;
}

bool elapsed_exceeds(uint32_t now_ms, uint32_t then_ms, uint32_t limit_ms) noexcept
{
    return static_cast<uint32_t>(now_ms - then_ms) > limit_ms;
}

bool elapsed_reaches(uint32_t now_ms, uint32_t then_ms, uint32_t limit_ms) noexcept
{
    return static_cast<uint32_t>(now_ms - then_ms) >= limit_ms;
}

} // namespace

void LinkManager::init(Policy policy, std::span<const BtAddr> alarms) noexcept
{
    sessions_.fill(Session{});
    alarms_.fill(BtAddr{});

    const size_t limit = (alarms.size() < kMaxAlarms) ? alarms.size() : kMaxAlarms;
    for (size_t i = 0; i < limit; ++i) {
        alarms_[i] = alarms[i];
    }
    alarm_count_ = limit;
    policy_ = policy;
    pairing_ = false;
    pair_start_ms_ = 0;
}

Role LinkManager::on_open(SessionHandle handle, const BtAddr &addr, uint32_t now_ms, Actions &act) noexcept
{
    act.clear();

    size_t free_idx = kMaxSessions;
    for (size_t i = 0; i < kMaxSessions; ++i) {
        const Session &session = sessions_[i];
        if (session.in_use) {
            // 规则 1：重复句柄拒绝。
            if (session.handle == handle) {
                action_add(act, handle);
                return Role::kNone;
            }
        } else if (free_idx == kMaxSessions) {
            free_idx = i;
        }
    }

    // 规则 2：会话表已满。
    if (free_idx == kMaxSessions) {
        action_add(act, handle);
        return Role::kNone;
    }

    const bool whitelisted = contains_addr(std::span<const BtAddr>(alarms_.data(), alarm_count_), addr);
    bool phone_online = false;
    for (const Session &session : sessions_) {
        if (session.in_use && (session.role == Role::kPhone)) {
            phone_online = true;
            break;
        }
    }

    Role role = Role::kNone;
    if (whitelisted) {
        // 规则 3：白名单设备是 ALARM；手机优先策略下已有 PHONE 时拒绝。
        if ((policy_ == Policy::kPhonePreempt) && phone_online) {
            action_add(act, handle);
            return Role::kNone;
        }
        role = Role::kAlarm;
        trace_link(LinkTrace::kOpen, handle, role_value(role), reason_value(LinkReason::kWhitelisted));
    } else if (!phone_online) {
        // 规则 4 第一支：无 PHONE 时未知设备作为 PHONE。
        role = Role::kPhone;
        trace_link(LinkTrace::kOpen, handle, role_value(role), reason_value(LinkReason::kFirstPhone));
        if (policy_ == Policy::kPhonePreempt) {
            for (Session &session : sessions_) {
                if (session.in_use && (session.role == Role::kAlarm)) {
                    const uint32_t victim = session.handle;
                    action_add(act, session.handle);
                    session.in_use = false;
                    trace_link(LinkTrace::kKick, victim, role_value(Role::kAlarm),
                               reason_value(LinkReason::kPolicyPreempt));
                }
            }
        }
    } else if (pairing_) {
        // 规则 4 第二支：手机在线且配对窗口打开，等待 HELLO。
        role = Role::kPending;
        trace_link(LinkTrace::kOpen, handle, role_value(role), reason_value(LinkReason::kPendingHello));
    } else {
        action_add(act, handle);
        return Role::kNone;
    }

    Session &session = sessions_[free_idx];
    session.in_use = true;
    session.handle = handle;
    session.role = role;
    session.addr = addr;
    session.since_ms = now_ms;
    session.last_rx_ms = now_ms;
    return role;
}

void LinkManager::on_close(SessionHandle handle) noexcept
{
    for (Session &session : sessions_) {
        if (session.in_use && (session.handle == handle)) {
            trace_link(LinkTrace::kClose, handle, role_value(session.role));
            session.in_use = false;
            return;
        }
    }
}

Role LinkManager::on_frame(SessionHandle handle, uint8_t sender, uint16_t msg_id, uint32_t now_ms,
                           Actions &act) noexcept
{
    act.clear();

    Session *found = nullptr;
    for (Session &session : sessions_) {
        if (session.in_use && (session.handle == handle)) {
            found = &session;
            break;
        }
    }
    if (found == nullptr) {
        return Role::kNone;
    }

    found->last_rx_ms = now_ms;

    if ((msg_id == kHelloMsgId) && (sender == kHelloSender)) {
        if (found->role == Role::kAlarm) {
            return Role::kAlarm;
        }
        if (!pairing_) {
            // 配对窗口外的 HELLO：未授权报警器，断开。
            action_add(act, handle);
            found->in_use = false;
            trace_link(LinkTrace::kClose, handle, role_value(found->role), reason_value(LinkReason::kOther));
            return Role::kNone;
        }
        if (!contains_addr(std::span<const BtAddr>(alarms_.data(), alarm_count_), found->addr)) {
            // 白名单已满：拒绝并断开。
            if (alarm_count_ >= kMaxAlarms) {
                action_add(act, handle);
                const Role rejected = found->role;
                found->in_use = false;
                trace_link(LinkTrace::kClose, handle, role_value(rejected), reason_value(LinkReason::kNoSlot));
                return Role::kNone;
            }
            alarms_[alarm_count_] = found->addr;
            ++alarm_count_;
            trace_link(LinkTrace::kWhitelistAdd, handle, static_cast<uint32_t>(alarm_count_));
        }
        const uint32_t previous = role_value(found->role);
        found->role = Role::kAlarm;
        found->since_ms = now_ms;
        trace_link(LinkTrace::kRoleChange, handle, previous, role_value(found->role));
        return Role::kAlarm;
    }

    if (found->role == Role::kPending) {
        // 等待 HELLO 的会话收到其他帧：不允许第二部手机，断开。
        action_add(act, handle);
        found->in_use = false;
        return Role::kNone;
    }

    return found->role;
}

void LinkManager::tick(uint32_t now_ms, Actions &act) noexcept
{
    act.clear();

    for (Session &session : sessions_) {
        if (!session.in_use) {
            continue;
        }
        if (session.role == Role::kAlarm) {
            if (elapsed_exceeds(now_ms, session.last_rx_ms, kAlarmRxTimeoutMs)) {
                trace_link(LinkTrace::kAlarmRxTimeout, session.handle,
                           static_cast<uint32_t>(now_ms - session.last_rx_ms));
                action_add(act, session.handle);
                session.in_use = false;
            }
        } else if (session.role == Role::kPending) {
            if (elapsed_exceeds(now_ms, session.since_ms, kPendingTimeoutMs)) {
                trace_link(LinkTrace::kPendingTimeout, session.handle,
                           static_cast<uint32_t>(now_ms - session.since_ms));
                action_add(act, session.handle);
                session.in_use = false;
            }
        } else {
            // PHONE 不因接收超时断开。
        }
    }

    if (pairing_ && elapsed_reaches(now_ms, pair_start_ms_, kPairWindowMs)) {
        pairing_ = false;
        trace_link(LinkTrace::kPairClose, 0, 1);
    }
}

Role LinkManager::role(SessionHandle handle) const noexcept
{
    for (const Session &session : sessions_) {
        if (session.in_use && (session.handle == handle)) {
            return session.role;
        }
    }
    return Role::kNone;
}

size_t LinkManager::handles(Role role, std::span<SessionHandle> out) const noexcept
{
    size_t count = 0;
    for (const Session &session : sessions_) {
        if (session.in_use && (session.role == role) && (count < out.size())) {
            out[count] = session.handle;
            ++count;
        }
    }
    return count;
}

bool LinkManager::has_role(Role role) const noexcept
{
    for (const Session &session : sessions_) {
        if (session.in_use && (session.role == role)) {
            return true;
        }
    }
    return false;
}

void LinkManager::pair_open(uint32_t now_ms) noexcept
{
    pairing_ = true;
    pair_start_ms_ = now_ms;
    trace_link(LinkTrace::kPairOpen, 0, static_cast<uint32_t>(kPairWindowMs));
}

bool LinkManager::is_pairing() const noexcept
{
    return pairing_;
}

void LinkManager::whitelist_clear() noexcept
{
    alarm_count_ = 0;
    trace_link(LinkTrace::kWhitelistClear, 0);
}

WhitelistEdit LinkManager::whitelist_add(const BtAddr &addr) noexcept
{
    if (contains_addr(std::span<const BtAddr>(alarms_.data(), alarm_count_), addr)) {
        return WhitelistEdit::kUnchanged;
    }
    if (alarm_count_ >= kMaxAlarms) {
        return WhitelistEdit::kFull;
    }
    alarms_[alarm_count_] = addr;
    ++alarm_count_;
    trace_link(LinkTrace::kWhitelistAdd, 0, static_cast<uint32_t>(alarm_count_));
    return WhitelistEdit::kDone;
}

WhitelistEdit LinkManager::whitelist_remove(const BtAddr &addr) noexcept
{
    for (size_t i = 0; i < alarm_count_; ++i) {
        if (alarms_[i] == addr) {
            // 后续条目前移，保持持久化顺序稳定。
            for (size_t j = i + 1U; j < alarm_count_; ++j) {
                alarms_[j - 1U] = alarms_[j];
            }
            --alarm_count_;
            alarms_[alarm_count_] = BtAddr{};
            trace_link(LinkTrace::kWhitelistRemove, 0, static_cast<uint32_t>(alarm_count_));
            return WhitelistEdit::kDone;
        }
    }
    return WhitelistEdit::kUnchanged;
}

std::span<const BtAddr> LinkManager::whitelist() const noexcept
{
    return {alarms_.data(), alarm_count_};
}

Policy LinkManager::policy() const noexcept
{
    return policy_;
}

} // namespace core
