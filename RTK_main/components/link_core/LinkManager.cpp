// SPP 会话角色与策略管理：准入、超时、配对窗口与白名单（规则 1–4 见 LinkManager.hpp）。
// 不调用传输层，待断开句柄经 Actions 返回；仅链路任务调用，非 ISR。
// 不分配堆、不阻塞；时间比较用无符号差值以容忍 32 位毫秒回绕。

#include "core/LinkManager.hpp"
#include "core/TraceCodes.hpp"
#include "private_include/PairWindow.hpp"

namespace core {

namespace {

constinit const LinkManager *s_pair_owner = nullptr;
constinit PairWindow *s_pair_window = nullptr;

/** @brief 查询已绑定的窗口。 @param manager: 链路管理器。 @retval 窗口地址或 nullptr。 @note 仅链路任务。 */
PairWindow *pairWindow(const LinkManager &manager) noexcept
{
    return s_pair_owner == &manager ? s_pair_window : nullptr;
}

/** @brief 在帧处理时判断窗口。 @param manager: 链路管理器。 @param nowMs: 当前时间。 @retval true 窗口打开。 */
bool pairOpenAt(const LinkManager &manager, uint32_t nowMs) noexcept
{
    const auto *window = pairWindow(manager);
    return window != nullptr ? window->snapshot(nowMs).remainingMs != 0U : manager.isPairing();
}

/** @brief 把 Role 转成追踪事件参数（uint32_t），避免窄化告警。
 *  @param role 待转换角色。
 *  @return 角色的枚举数值。 */
constexpr uint32_t roleValue(Role role) noexcept
{
    return static_cast<uint32_t>(role);
}

/** @brief 把 LinkReason 转成追踪事件参数（uint32_t）。
 *  @param reason 判定依据。
 *  @return 枚举数值。 */
constexpr uint32_t reasonValue(LinkReason reason) noexcept
{
    return static_cast<uint32_t>(reason);
}

/** @brief 向 Actions 追加一个待断开句柄，按产生顺序排列。
 *  @param[out] act 动作输出；容量用尽时静默丢弃该句柄（调用方保证不溢出）。
 *  @param handle 待断开的会话句柄。 */
void actionAdd(Actions &act, SessionHandle handle) noexcept
{
    if (act.count < act.handles.size()) {
        act.handles[act.count] = handle;
        ++act.count;
    }
}

/** @brief 线性查找地址是否在白名单视图中。
 *  @param list 地址视图（调用方保证范围有效）。
 *  @param addr 待查地址。
 *  @return true 存在；false 不存在或 list 为空。 */
bool containsAddr(std::span<const BtAddr> list, const BtAddr &addr) noexcept
{
    for (const BtAddr &entry : list) {
        if (entry == addr) {
            return true;
        }
    }
    return false;
}

/** @brief 判断 nowMs 距 thenMs 是否严格超过 limitMs（回绕安全）。
 *  @param nowMs 当前时间（ms）。
 *  @param thenMs 计时起点（ms）。
 *  @param limitMs 时限（ms）。
 *  @return true 表示已超过；相等不算超时。 */
bool elapsedExceeds(uint32_t nowMs, uint32_t thenMs, uint32_t limitMs) noexcept
{
    return static_cast<uint32_t>(nowMs - thenMs) > limitMs;
}

/** @brief 判断 nowMs 距 thenMs 是否达到 limitMs（回绕安全）。
 *  @param nowMs 当前时间（ms）。
 *  @param thenMs 计时起点（ms）。
 *  @param limitMs 时限（ms）。
 *  @return true 表示已到达（含相等），用于配对窗口关闭。 */
bool elapsedReaches(uint32_t nowMs, uint32_t thenMs, uint32_t limitMs) noexcept
{
    return static_cast<uint32_t>(nowMs - thenMs) >= limitMs;
}

} // namespace

void PairWindow::set(uint32_t nowMs, uint32_t durationMs, bool confirm) noexcept
{
    const fw::LockGuard guard(lock_);
    start_ms_ = nowMs;
    duration_ms_ = durationMs;
    confirm_ = confirm;
}

PairWindowStatus PairWindow::snapshot(uint32_t nowMs) const noexcept
{
    const fw::LockGuard guard(lock_);
    const uint32_t elapsed = nowMs - start_ms_;
    if (elapsed >= duration_ms_) {
        return {};
    }
    return {duration_ms_ - elapsed, confirm_};
}

bool PairWindow::active() const noexcept
{
    const fw::LockGuard guard(lock_);
    return duration_ms_ != 0U;
}

bool PairWindow::expire(uint32_t nowMs) noexcept
{
    const fw::LockGuard guard(lock_);
    if (duration_ms_ == 0U || nowMs - start_ms_ < duration_ms_) {
        return false;
    }
    duration_ms_ = 0U;
    return true;
}

void bindPairWindow(const LinkManager &manager, PairWindow *window) noexcept
{
    s_pair_owner = &manager;
    s_pair_window = window;
}

/** @brief 查询白名单写入准入。 @param manager: 链路管理器。 @retval true 可写入。 @note 仅 kLink。 */
bool admitWhitelistWrite(const LinkManager &manager) noexcept
{
    const auto *window = pairWindow(manager);
    return window == nullptr || window->admitWhitelistWrite();
}

void openPairWindow(PairWindow &window, uint32_t nowMs, uint32_t durationMs, bool confirm) noexcept
{
    window.set(nowMs, durationMs, confirm);
    traceLink(LinkTrace::kPairOpen, 0, durationMs);
}

void closePairWindow(PairWindow &window, uint32_t nowMs) noexcept
{
    window.set(nowMs, 0U);
    traceLink(LinkTrace::kPairClose, 0, 0U);
}

/**
 * @brief 清空会话表与白名单，再按 policy 载入 alarms。
 * @param policy 准入策略（kConcurrent / kPhonePreempt）。
 * @param alarms 初始报警器地址；超出 kMaxAlarms 的部分忽略，其余项置零。
 * @note 启动阶段单线程调用一次，非 ISR、不分配、不阻塞；会话表一并清空（等价于丢弃全部在线会话）。
 */
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

// 准入规则 1→4 顺序即内容，拆开需传 act/policy/白名单/手机在线四五个状态（ADR-0013 §2.4）。
// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
Role LinkManager::onOpen(SessionHandle handle, const BtAddr &addr, uint32_t nowMs,
                         Actions &act) noexcept
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
    } else if (pairOpenAt(*this, nowMs)) {
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
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)

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
        if (!pairOpenAt(*this, nowMs)) {
            // 配对窗口外的 HELLO：未授权报警器，断开。
            actionAdd(act, handle);
            found->inUse = false;
            traceLink(LinkTrace::kClose, handle, roleValue(found->role),
                      reasonValue(LinkReason::kOther));
            return Role::kNone;
        }
        if (!containsAddr(std::span<const BtAddr>(alarms_.data(), alarm_count_), found->addr)) {
            // 白名单已满：拒绝并断开。
            if (alarm_count_ >= kMaxAlarms || !admitWhitelistWrite(*this)) {
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

    if (auto *window = pairWindow(*this); window != nullptr) {
        if (window->expire(nowMs)) {
            traceLink(LinkTrace::kPairClose, 0, 1U);
        }
    } else if (pairing_ && elapsedReaches(nowMs, pair_start_ms_, kPairWindowMs)) {
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
    if (auto *window = pairWindow(*this); window != nullptr) {
        openPairWindow(*window, nowMs, kPairWindowMs, false);
        return;
    }
    pairing_ = true;
    pair_start_ms_ = nowMs;
    traceLink(LinkTrace::kPairOpen, 0, static_cast<uint32_t>(kPairWindowMs));
}

bool LinkManager::isPairing() const noexcept
{
    if (const auto *window = pairWindow(*this); window != nullptr) {
        return window->active();
    }
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
