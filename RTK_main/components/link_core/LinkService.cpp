// 链路服务：传输事件 → 每会话解析器 → LinkManager → CommandRouter → 传输写回（0x40 推送由 AlarmService 组帧）。
// 线程安全：否，所有方法只由链路任务调用（引用对象无锁）；非 ISR、不阻塞。
// 堆：不分配（槽位与应答缓冲都是成员）。

#include "core/LinkService.hpp"
#include "core/TraceCodes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace core {

namespace {

/** @brief 判断动作表里是否含指定句柄。
 *  @param act 待查的动作表。
 *  @param handle 目标会话句柄。
 *  @return true 命中；false 不在表中。 */
bool actionsContain(const Actions &act, SessionHandle handle) noexcept
{
    for (const SessionHandle entry : act.view()) {
        if (entry == handle) {
            return true;
        }
    }
    return false;
}

} // namespace

LinkService::LinkService(Transport &transport, LinkManager &link, CommandRouter &router,
                         const services::AlarmService &alarm) noexcept
    : transport_(transport), link_(link), router_(router), alarm_(alarm)
{}

void LinkService::cancelDeferred(SessionHandle handle) noexcept
{
    for (DeferredEntry &entry : deferred_) {
        if (!entry.active || (entry.handle != handle)) {
            continue;
        }
        const DeferredHook hook = entry.hook;
        const ReplyCredit credit = entry.credit;
        const bool hasCredit = entry.hasCredit;
        entry = DeferredEntry{};
        if (hasCredit) {
            transport_.cancelReply(credit);
        }
        if (hook.cancel != nullptr) {
            hook.cancel(hook.ctx);
        }
    }
}

void LinkService::registerDeferred(SessionHandle handle, uint16_t msgId, const ReplyCredit &credit,
                                   const DeferredHook &hook) noexcept
{
    if (hook.poll != nullptr) {
        for (DeferredEntry &entry : deferred_) {
            if (!entry.active) {
                entry.active = true;
                entry.handle = handle;
                entry.msgId = msgId;
                entry.credit = credit;
                entry.hasCredit = transport_.supportsCredit();
                entry.hook = hook;
                return;
            }
        }
    }
    if (transport_.supportsCredit()) {
        transport_.cancelReply(credit);
    }
    if (hook.cancel != nullptr) {
        hook.cancel(hook.ctx);
    }
}

void LinkService::pollDeferred(uint32_t nowMs) noexcept
{
    for (DeferredEntry &entry : deferred_) {
        if (!entry.active) {
            continue;
        }
        reply_.clear();
        if (!entry.hook.poll(entry.hook.ctx, nowMs, reply_)) {
            continue;
        }
        const SessionHandle handle = entry.handle;
        const uint16_t msgId = entry.msgId;
        const ReplyCredit credit = entry.credit;
        const bool hasCredit = entry.hasCredit;
        entry = DeferredEntry{};
        if (reply_.len > 0U) {
            sendReply(handle, credit, msgId);
        } else if (hasCredit) {
            transport_.cancelReply(credit);
        }
    }
}

void LinkService::apply(const Actions &act) noexcept
{
    for (const SessionHandle handle : act.view()) {
        cancelDeferred(handle);
        ++stats_.disconnects;
        const fw::Status status = transport_.disconnect(handle);
        // 断开失败不回滚本地状态：传输层随后仍会报告关闭事件，onClose 对未知句柄幂等。
        static_cast<void>(status);

        Slot *slot = findSlot(handle);
        if (slot != nullptr) {
            retired_crc_errors_ += slot->parser.crcErrors();
            retired_length_errors_ += slot->parser.lengthErrors();
            stats_.dispatchCancelled += slot->pending ? 1U : 0U;
            slot->pending = false;
            slot->inUse = false;
            slot->handle = 0;
        }
    }
}

LinkService::Slot *LinkService::findSlot(SessionHandle handle) noexcept
{
    for (Slot &slot : slots_) {
        if (slot.inUse && (slot.handle == handle)) {
            return &slot;
        }
    }
    return nullptr;
}

void LinkService::onOpen(SessionHandle handle, const BtAddr &addr, uint32_t nowMs) noexcept
{
    Actions act;
    const Role role = link_.onOpen(handle, addr, nowMs, act);
    apply(act);
    if (role == Role::kNone) {
        return;
    }

    for (Slot &slot : slots_) {
        if (!slot.inUse) {
            // 新槽：整个解析器重建，保证错误计数从 0 开始（旧计数已累加到 retired_*）。
            slot = Slot{};
            slot.inUse = true;
            slot.handle = handle;
            return;
        }
    }

    // 会话表与解析器槽容量一致，理论上不可达；保守断开，避免占用会话却无法解析。
    ++stats_.disconnects;
    const fw::Status status = transport_.disconnect(handle);
    static_cast<void>(status);
}

void LinkService::onClose(SessionHandle handle) noexcept
{
    cancelDeferred(handle);
    link_.onClose(handle);

    Slot *slot = findSlot(handle);
    if (slot == nullptr) {
        return;
    }
    retired_crc_errors_ += slot->parser.crcErrors();
    retired_length_errors_ += slot->parser.lengthErrors();
    stats_.dispatchCancelled += slot->pending ? 1U : 0U;
    slot->pending = false;
    slot->inUse = false;
    slot->handle = 0;
}

void LinkService::sendReply(SessionHandle handle, const ReplyCredit &credit,
                            uint16_t msgId) noexcept
{
    const fw::Status status = transport_.supportsCredit()
                                  ? transport_.commitReply(credit, reply_.bytes())
                                  : transport_.write(handle, reply_.bytes(), kWriteTimeoutMs);
    if (isOk(status)) {
        ++stats_.repliesTx; // 接纳，不表示 WRITE_EVT 或应用 ACK
        traceProto(ProtoTrace::kReply, handle, msgId,
                   static_cast<uint32_t>(status)); // 枚举范围<=255
    } else {
        ++stats_.writeErrors;
        traceProto(ProtoTrace::kReplyError, handle, static_cast<uint32_t>(status),
                   msgId); // 枚举范围<=255
    }
}

/**
 * @brief 为协议帧预留额度并派发，PPP 完成前保留该额度。
 * @param slot: 已收到完整帧的会话槽。
 * @param nowMs: 当前单调时钟。
 * @retval true 请求已执行或已取消。
 * @retval false 额度不足，等待后续恢复。
 */
bool LinkService::dispatchPending(Slot &slot, uint32_t nowMs) noexcept
{
    ReplyCredit credit{};
    const SessionHandle handle = slot.handle;
    if (transport_.supportsCredit()) {
        const fw::Status status = transport_.reserveReply(handle, credit);
        if (!isOk(status)) {
            if ((status == fw::Status::kTimeout) || (status == fw::Status::kNoSpace) ||
                (status == fw::Status::kBusy)) {
                return false;
            }
            ++stats_.writeErrors;
            ++stats_.disconnects;
            const auto disconnected = transport_.disconnect(handle);
            static_cast<void>(disconnected);
            onClose(handle);
            return true;
        }
    }
    // 只有容量已固定才推进角色/路由；清 pending 后绝不因提交失败重跑副作用。
    slot.pending = false;
    const Role roleBefore = link_.role(handle);
    Actions act;
    const Role roleAfter = link_.onFrame(handle, slot.frame.sender, slot.frame.msgId, nowMs, act);
    const bool currentDisconnected = actionsContain(act, handle);
    apply(act);
    if (currentDisconnected || (roleAfter == Role::kNone)) {
        if (transport_.supportsCredit()) {
            transport_.cancelReply(credit);
        }
        return true;
    }
    const DispatchResult result = router_.handle(roleAfter, slot.frame, nowMs, reply_);
    if (result == DispatchResult::kReply) {
        sendReply(handle, credit, slot.frame.msgId);
    } else if (result == DispatchResult::kDeferred) {
        registerDeferred(handle, slot.frame.msgId, credit, reply_.deferred);
    } else if (transport_.supportsCredit()) {
        transport_.cancelReply(credit);
    }
    if (reply_.heartbeat.has_value()) {
        last_hb_ = reply_.heartbeat;
    }
    if (reply_.whitelistChanged || ((roleAfter == Role::kAlarm) && (roleBefore != Role::kAlarm))) {
        whitelist_dirty_ = true;
    }
    return true;
}

bool LinkService::paused(SessionHandle handle) const noexcept
{
    for (const auto &slot : slots_) {
        if (slot.inUse && (slot.handle == handle)) {
            return slot.pending;
        }
    }
    return false;
}

void LinkService::resume(SessionHandle handle, uint32_t nowMs) noexcept
{
    Slot *slot = findSlot(handle);
    if ((slot != nullptr) && slot->pending && transport_.replyReady(handle)) {
        static_cast<void>(dispatchPending(*slot, nowMs));
    }
}

size_t LinkService::onData(SessionHandle handle, std::span<const uint8_t> data,
                           uint32_t nowMs) noexcept
{
    Slot *slot = findSlot(handle);
    if (slot == nullptr) {
        return data.size(); // 未知/已断开的尾部按契约丢弃
    }
    if (slot->pending) {
        return 0;
    }
    size_t consumed = 0;
    for (const uint8_t byte : data) {
        ++consumed;
        const proto::ParseResult parsed = slot->parser.feed(byte, slot->frame);
        if (parsed == proto::ParseResult::kCrcError) {
            traceProto(ProtoTrace::kBadCrc, handle, slot->parser.crcErrors());
        } else if (parsed == proto::ParseResult::kLengthError) {
            traceProto(ProtoTrace::kBadLength, handle, slot->parser.lengthErrors());
        } else if (parsed == proto::ParseResult::kFrame) {
            ++stats_.framesRx;
            // msgId/type 扩宽无损，payload<=256；仅用于追踪摘要。
            traceProto(ProtoTrace::kFrame, handle,
                       (static_cast<uint32_t>(slot->frame.msgId) << 8U) | slot->frame.type,
                       static_cast<uint32_t>(slot->frame.payload.size()));
            slot->pending = true;
            if (!dispatchPending(*slot, nowMs)) {
                ++stats_.dispatchPauses;
                return consumed;
            }
            if (!slot->inUse) {
                return data.size();
            }
        }
    }
    return consumed;
}

void LinkService::onRxDropped(uint32_t count) noexcept
{
    stats_.rxDropped += count;
}

void LinkService::tick(uint32_t nowMs) noexcept
{
    Actions act;
    link_.tick(nowMs, act);
    apply(act);
    pollDeferred(nowMs);

    const bool due = (!pushed_once_) ||
                     (static_cast<uint32_t>(nowMs - last_push_ms_) >= proto::alarm::kPushPeriodMs);
    if (!due) {
        return;
    }

    pushed_once_ = true;
    last_push_ms_ = nowMs;

    std::array<SessionHandle, kMaxSessions> handles{};
    const size_t count = link_.handles(Role::kAlarm, handles);
    if (count == 0) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (!alarm_.buildStatusPush(push_seq_, nowMs, reply_)) {
            traceAlarm(AlarmTrace::kPushFail, handles[i], 0);
            continue;
        }
        const fw::Status status = transport_.pushLatest(handles[i], reply_.bytes());
        if (isOk(status)) {
            ++stats_.pushesTx;
        } else {
            ++stats_.writeErrors;
            traceAlarm(AlarmTrace::kPushFail, handles[i], static_cast<uint32_t>(status));
        }
    }
    ++push_seq_;
}

bool LinkService::consumeWhitelistDirty() noexcept
{
    const bool dirty = whitelist_dirty_;
    whitelist_dirty_ = false;
    return dirty;
}

std::optional<proto::alarm::Heartbeat> LinkService::lastHeartbeat() const noexcept
{
    return last_hb_;
}

const LinkStats &LinkService::stats() const noexcept
{
    return stats_;
}

uint32_t LinkService::parserCrcErrors() const noexcept
{
    uint32_t total = retired_crc_errors_;
    for (const Slot &slot : slots_) {
        if (slot.inUse) {
            total += slot.parser.crcErrors();
        }
    }
    return total;
}

uint32_t LinkService::parserLengthErrors() const noexcept
{
    uint32_t total = retired_length_errors_;
    for (const Slot &slot : slots_) {
        if (slot.inUse) {
            total += slot.parser.lengthErrors();
        }
    }
    return total;
}

} // namespace core
