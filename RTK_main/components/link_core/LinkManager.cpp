// SPP 会话角色与策略管理（phone_protocol.md §2/§5，LinkManager.hpp 规则 1–4）。
// 不调用传输层；需要断开的句柄经 Actions 返回给 LinkService。
// 所有时间比较用无符号差值，容忍 32 位毫秒回绕。

#include "core/LinkManager.hpp"
#include "core/TraceCodes.hpp"

namespace core {

namespace {

// 事件参数统一用 uint32_t；底层类型为 uint8_t 的枚举转宽不收窄。
constexpr uint32_t roleValue(Role role) noexcept
{
    return static_cast<uint32_t>(role);
}

constexpr uint32_t reasonValue(LinkReason reason) noexcept
{
    return static_cast<uint32_t>(reason);
}

void actionAdd(Actions &act, SessionHandle handle) noexcept
{
    if (act.count < act.handles.size()) {
        act.handles[act.count] = handle;
        ++act.count;
    }
}

bool containsAddr(std::span<const BtAddr> list, const BtAddr &addr) noexcept
{
    for (const BtAddr &entry : list) {
        if (entry == addr) {
            return true;
        }
    }
    return false;
}

bool elapsedExceeds(uint32_t nowMs, uint32_t thenMs, uint32_t limitMs) noexcept
{
    return static_cast<uint32_t>(nowMs - thenMs) > limitMs;
}

bool elapsedReaches(uint32_t nowMs, uint32_t thenMs, uint32_t limitMs) noexcept
{
    return static_cast<uint32_t>(nowMs - thenMs) >= limitMs;
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

/**
 * @brief  连接建立：按准入规则决定角色并向链路回写动作
 * @param  handle: 新连接的会话句柄
 * @param  addr: 对端地址
 * @param  nowMs: 当前时间（ms）
 * @param[out] act: 对链路的动作输出
 * @retval 仲裁出的角色
 * @note   不拆（S4 §2.1）：准入规则 1→4 的顺序即内容，拆开要传 act/policy/白名单/手机在线四五个状态。
 */
Role LinkManager::onOpen(SessionHandle handle, // NOLINT(readability-function-*): 见不拆说明
                         const BtAddr &addr, uint32_t nowMs, Actions &act) noexcept
{
    act.clear();

    size_t freeIdx = kMaxSessions;
    for (size_t i = 0; i < kMaxSessions; ++i) {
        const Session &session = sessions_[i];
        if (session.inUse) {
            // 规则 1：重复句柄拒绝。
            if (session.handle == handle) {
                actionAdd(act, handle);
                return Role::kNone;
            }
        } else if (freeIdx == kMaxSessions) {
            freeIdx = i;
        }
    }

    // 规则 2：会话表已满。
    if (freeIdx == kMaxSessions) {
        actionAdd(act, handle);
        return Role::kNone;
    }

    const bool whitelisted =
        containsAddr(std::span<const BtAddr>(alarms_.data(), alarm_count_), addr);
    bool phoneOnline = false;
    for (const Session &session : sessions_) {
        if (session.inUse && (session.role == Role::kPhone)) {
            phoneOnline = true;
            break;
        }
    }

    Role role = Role::kNone;
    if (whitelisted) {
        // 规则 3：白名单设备是 ALARM；手机优先策略下已有 PHONE 时拒绝。
        if ((policy_ == Policy::kPhonePreempt) && phoneOnline) {
            actionAdd(act, handle);
            return Role::kNone;
        }
        role = Role::kAlarm;
        traceLink(LinkTrace::kOpen, handle, roleValue(role), reasonValue(LinkReason::kWhitelisted));
    } else if (!phoneOnline) {
        // 规则 4 第一支：无 PHONE 时未知设备作为 PHONE。
        role = Role::kPhone;
        traceLink(LinkTrace::kOpen, handle, roleValue(role), reasonValue(LinkReason::kFirstPhone));
        if (policy_ == Policy::kPhonePreempt) {
            for (Session &session : sessions_) {
                if (session.inUse && (session.role == Role::kAlarm)) {
                    const uint32_t victim = session.handle;
                    actionAdd(act, session.handle);
                    session.inUse = false;
                    traceLink(LinkTrace::kKick, victim, roleValue(Role::kAlarm),
                              reasonValue(LinkReason::kPolicyPreempt));
                }
            }
        }
    } else if (pairing_) {
        // 规则 4 第二支：手机在线且配对窗口打开，等待 HELLO。
        role = Role::kPending;
        traceLink(LinkTrace::kOpen, handle, roleValue(role),
                  reasonValue(LinkReason::kPendingHello));
    } else {
        actionAdd(act, handle);
        return Role::kNone;
    }

    Session &session = sessions_[freeIdx];
    session.inUse = true;
    session.handle = handle;
    session.role = role;
    session.addr = addr;
    session.sinceMs = nowMs;
    session.lastRxMs = nowMs;
    return role;
}

void LinkManager::onClose(SessionHandle handle) noexcept
{
    for (Session &session : sessions_) {
        if (session.inUse && (session.handle == handle)) {
            traceLink(LinkTrace::kClose, handle, roleValue(session.role));
            session.inUse = false;
            return;
        }
    }
}

Role LinkManager::onFrame(SessionHandle handle, uint8_t sender, uint16_t msgId, uint32_t nowMs,
                          Actions &act) noexcept
{
    act.clear();

    Session *found = nullptr;
    for (Session &session : sessions_) {
        if (session.inUse && (session.handle == handle)) {
            found = &session;
            break;
        }
    }
    if (found == nullptr) {
        return Role::kNone;
    }

    found->lastRxMs = nowMs;

    if ((msgId == kHelloMsgId) && (sender == kHelloSender)) {
        if (found->role == Role::kAlarm) {
            return Role::kAlarm;
        }
        if (!pairing_) {
            // 配对窗口外的 HELLO：未授权报警器，断开。
            actionAdd(act, handle);
            found->inUse = false;
            traceLink(LinkTrace::kClose, handle, roleValue(found->role),
                      reasonValue(LinkReason::kOther));
            return Role::kNone;
        }
        if (!containsAddr(std::span<const BtAddr>(alarms_.data(), alarm_count_), found->addr)) {
            // 白名单已满：拒绝并断开。
            if (alarm_count_ >= kMaxAlarms) {
                actionAdd(act, handle);
                const Role rejected = found->role;
                found->inUse = false;
                traceLink(LinkTrace::kClose, handle, roleValue(rejected),
                          reasonValue(LinkReason::kNoSlot));
                return Role::kNone;
            }
            alarms_[alarm_count_] = found->addr;
            ++alarm_count_;
            traceLink(LinkTrace::kWhitelistAdd, handle, static_cast<uint32_t>(alarm_count_));
        }
        const uint32_t previous = roleValue(found->role);
        found->role = Role::kAlarm;
        found->sinceMs = nowMs;
        traceLink(LinkTrace::kRoleChange, handle, previous, roleValue(found->role));
        return Role::kAlarm;
    }

    if (found->role == Role::kPending) {
        // 等待 HELLO 的会话收到其他帧：不允许第二部手机，断开。
        actionAdd(act, handle);
        found->inUse = false;
        return Role::kNone;
    }

    return found->role;
}

void LinkManager::tick(uint32_t nowMs, Actions &act) noexcept
{
    act.clear();

    for (Session &session : sessions_) {
        if (!session.inUse) {
            continue;
        }
        if (session.role == Role::kAlarm) {
            if (elapsedExceeds(nowMs, session.lastRxMs, kAlarmRxTimeoutMs)) {
                traceLink(LinkTrace::kAlarmRxTimeout, session.handle,
                          static_cast<uint32_t>(nowMs - session.lastRxMs));
                actionAdd(act, session.handle);
                session.inUse = false;
            }
        } else if (session.role == Role::kPending) {
            if (elapsedExceeds(nowMs, session.sinceMs, kPendingTimeoutMs)) {
                traceLink(LinkTrace::kPendingTimeout, session.handle,
                          static_cast<uint32_t>(nowMs - session.sinceMs));
                actionAdd(act, session.handle);
                session.inUse = false;
            }
        } else {
            // PHONE 不因接收超时断开。
        }
    }

    if (pairing_ && elapsedReaches(nowMs, pair_start_ms_, kPairWindowMs)) {
        pairing_ = false;
        traceLink(LinkTrace::kPairClose, 0, 1);
    }
}

Role LinkManager::role(SessionHandle handle) const noexcept
{
    for (const Session &session : sessions_) {
        if (session.inUse && (session.handle == handle)) {
            return session.role;
        }
    }
    return Role::kNone;
}

size_t LinkManager::handles(Role role, std::span<SessionHandle> out) const noexcept
{
    size_t count = 0;
    for (const Session &session : sessions_) {
        if (session.inUse && (session.role == role) && (count < out.size())) {
            out[count] = session.handle;
            ++count;
        }
    }
    return count;
}

bool LinkManager::hasRole(Role role) const noexcept
{
    for (const Session &session : sessions_) {
        if (session.inUse && (session.role == role)) {
            return true;
        }
    }
    return false;
}

void LinkManager::pairOpen(uint32_t nowMs) noexcept
{
    pairing_ = true;
    pair_start_ms_ = nowMs;
    traceLink(LinkTrace::kPairOpen, 0, static_cast<uint32_t>(kPairWindowMs));
}

bool LinkManager::isPairing() const noexcept
{
    return pairing_;
}

void LinkManager::whitelistClear() noexcept
{
    alarm_count_ = 0;
    traceLink(LinkTrace::kWhitelistClear, 0);
}

WhitelistEdit LinkManager::whitelistAdd(const BtAddr &addr) noexcept
{
    if (containsAddr(std::span<const BtAddr>(alarms_.data(), alarm_count_), addr)) {
        return WhitelistEdit::kUnchanged;
    }
    if (alarm_count_ >= kMaxAlarms) {
        return WhitelistEdit::kFull;
    }
    alarms_[alarm_count_] = addr;
    ++alarm_count_;
    traceLink(LinkTrace::kWhitelistAdd, 0, static_cast<uint32_t>(alarm_count_));
    return WhitelistEdit::kDone;
}

WhitelistEdit LinkManager::whitelistRemove(const BtAddr &addr) noexcept
{
    for (size_t i = 0; i < alarm_count_; ++i) {
        if (alarms_[i] == addr) {
            // 后续条目前移，保持持久化顺序稳定。
            for (size_t j = i + 1U; j < alarm_count_; ++j) {
                alarms_[j - 1U] = alarms_[j];
            }
            --alarm_count_;
            alarms_[alarm_count_] = BtAddr{};
            traceLink(LinkTrace::kWhitelistRemove, 0, static_cast<uint32_t>(alarm_count_));
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
