// 链路服务：传输事件 → 每会话解析器 → LinkManager → CommandRouter → 传输写回。
// 纯逻辑，经 Transport 接口访问硬件；所有方法只由链路任务调用。
// ADR-0007 J2b：分发改由 CommandRouter（服务路由表）完成，0x40 推送帧由 AlarmService 组帧；
// 白名单脏标志、心跳记录、断开动作与错误计数逻辑与 J2a 之前逐行一致。

#include "core/LinkService.hpp"
#include "core/TraceCodes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace core {

namespace {

bool actions_contain(const Actions &act, SessionHandle handle) noexcept
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

void LinkService::apply(const Actions &act) noexcept
{
    for (const SessionHandle handle : act.view()) {
        ++stats_.disconnects;
        const fw::Status status = transport_.disconnect(handle);
        // 断开失败不回滚本地状态：传输层随后仍会报告关闭事件，on_close 对未知句柄幂等。
        static_cast<void>(status);

        Slot *slot = find_slot(handle);
        if (slot != nullptr) {
            retired_crc_errors_ += slot->parser.crc_errors();
            retired_length_errors_ += slot->parser.length_errors();
            stats_.dispatch_cancelled += slot->pending ? 1U : 0U;
            slot->pending = false;
            slot->in_use = false;
            slot->handle = 0;
        }
    }
}

LinkService::Slot *LinkService::find_slot(SessionHandle handle) noexcept
{
    for (Slot &slot : slots_) {
        if (slot.in_use && (slot.handle == handle)) {
            return &slot;
        }
    }
    return nullptr;
}

void LinkService::on_open(SessionHandle handle, const BtAddr &addr, uint32_t now_ms) noexcept
{
    Actions act;
    const Role role = link_.on_open(handle, addr, now_ms, act);
    apply(act);
    if (role == Role::kNone) {
        return;
    }

    for (Slot &slot : slots_) {
        if (!slot.in_use) {
            // 新槽：整个解析器重建，保证错误计数从 0 开始（旧计数已累加到 retired_*）。
            slot = Slot{};
            slot.in_use = true;
            slot.handle = handle;
            return;
        }
    }

    // 会话表与解析器槽容量一致，理论上不可达；保守断开，避免占用会话却无法解析。
    ++stats_.disconnects;
    const fw::Status status = transport_.disconnect(handle);
    static_cast<void>(status);
}

void LinkService::on_close(SessionHandle handle) noexcept
{
    link_.on_close(handle);

    Slot *slot = find_slot(handle);
    if (slot == nullptr) {
        return;
    }
    retired_crc_errors_ += slot->parser.crc_errors();
    retired_length_errors_ += slot->parser.length_errors();
    stats_.dispatch_cancelled += slot->pending ? 1U : 0U;
    slot->pending = false;
    slot->in_use = false;
    slot->handle = 0;
}

void LinkService::send_reply(SessionHandle handle, const ReplyCredit &credit,
                             uint16_t msg_id) noexcept
{
    const fw::Status status = transport_.supports_credit()
                                  ? transport_.commit_reply(credit, reply_.bytes())
                                  : transport_.write(handle, reply_.bytes(), kWriteTimeoutMs);
    if (is_ok(status)) {
        ++stats_.replies_tx; // 接纳，不表示 WRITE_EVT 或应用 ACK
        trace_proto(ProtoTrace::kReply, handle, msg_id,
                    static_cast<uint32_t>(status)); // 枚举范围<=255
    } else {
        ++stats_.write_errors;
        trace_proto(ProtoTrace::kReplyError, handle, static_cast<uint32_t>(status),
                    msg_id); // 枚举范围<=255
    }
}

bool LinkService::dispatch_pending(Slot &slot, uint32_t now_ms) noexcept
{
    ReplyCredit credit{};
    const SessionHandle handle = slot.handle;
    if (transport_.supports_credit()) {
        const fw::Status status = transport_.reserve_reply(handle, credit);
        if (!is_ok(status)) {
            if ((status == fw::Status::kTimeout) || (status == fw::Status::kNoSpace) ||
                (status == fw::Status::kBusy)) {
                return false;
            }
            ++stats_.write_errors;
            ++stats_.disconnects;
            const auto disconnected = transport_.disconnect(handle);
            static_cast<void>(disconnected);
            on_close(handle);
            return true;
        }
    }
    // 只有容量已固定才推进角色/路由；清 pending 后绝不因提交失败重跑副作用。
    slot.pending = false;
    const Role role_before = link_.role(handle);
    Actions act;
    const Role role_after =
        link_.on_frame(handle, slot.frame.sender, slot.frame.msg_id, now_ms, act);
    const bool current_disconnected = actions_contain(act, handle);
    apply(act);
    if (current_disconnected || (role_after == Role::kNone)) {
        if (transport_.supports_credit()) {
            transport_.cancel_reply(credit);
        }
        return true;
    }
    const DispatchResult result = router_.handle(role_after, slot.frame, now_ms, reply_);
    if (result == DispatchResult::kReply) {
        send_reply(handle, credit, slot.frame.msg_id);
    } else if (transport_.supports_credit()) {
        transport_.cancel_reply(credit);
    }
    if (reply_.heartbeat.has_value()) {
        last_hb_ = reply_.heartbeat;
    }
    if (reply_.whitelist_changed ||
        ((role_after == Role::kAlarm) && (role_before != Role::kAlarm))) {
        whitelist_dirty_ = true;
    }
    return true;
}

bool LinkService::paused(SessionHandle handle) const noexcept
{
    for (const auto &slot : slots_) {
        if (slot.in_use && (slot.handle == handle)) {
            return slot.pending;
        }
    }
    return false;
}

void LinkService::resume(SessionHandle handle, uint32_t now_ms) noexcept
{
    Slot *slot = find_slot(handle);
    if ((slot != nullptr) && slot->pending && transport_.reply_ready(handle)) {
        static_cast<void>(dispatch_pending(*slot, now_ms));
    }
}

size_t LinkService::on_data(SessionHandle handle, std::span<const uint8_t> data,
                            uint32_t now_ms) noexcept
{
    Slot *slot = find_slot(handle);
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
            trace_proto(ProtoTrace::kBadCrc, handle, slot->parser.crc_errors());
        } else if (parsed == proto::ParseResult::kLengthError) {
            trace_proto(ProtoTrace::kBadLength, handle, slot->parser.length_errors());
        } else if (parsed == proto::ParseResult::kFrame) {
            ++stats_.frames_rx;
            // msg_id/type 扩宽无损，payload<=256；仅用于追踪摘要。
            trace_proto(ProtoTrace::kFrame, handle,
                        (static_cast<uint32_t>(slot->frame.msg_id) << 8U) | slot->frame.type,
                        static_cast<uint32_t>(slot->frame.payload.size()));
            slot->pending = true;
            if (!dispatch_pending(*slot, now_ms)) {
                ++stats_.dispatch_pauses;
                return consumed;
            }
            if (!slot->in_use) {
                return data.size();
            }
        }
    }
    return consumed;
}

void LinkService::on_rx_dropped(uint32_t count) noexcept
{
    stats_.rx_dropped += count;
}

void LinkService::tick(uint32_t now_ms) noexcept
{
    Actions act;
    link_.tick(now_ms, act);
    apply(act);

    const bool due = (!pushed_once_) ||
                     (static_cast<uint32_t>(now_ms - last_push_ms_) >= proto::alarm::kPushPeriodMs);
    if (!due) {
        return;
    }

    pushed_once_ = true;
    last_push_ms_ = now_ms;

    std::array<SessionHandle, kMaxSessions> handles{};
    const size_t count = link_.handles(Role::kAlarm, handles);
    if (count == 0) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (!alarm_.build_status_push(push_seq_, now_ms, reply_)) {
            trace_alarm(AlarmTrace::kPushFail, handles[i], 0);
            continue;
        }
        const fw::Status status = transport_.push_latest(handles[i], reply_.bytes());
        if (is_ok(status)) {
            ++stats_.pushes_tx;
        } else {
            ++stats_.write_errors;
            trace_alarm(AlarmTrace::kPushFail, handles[i], static_cast<uint32_t>(status));
        }
    }
    ++push_seq_;
}

bool LinkService::consume_whitelist_dirty() noexcept
{
    const bool dirty = whitelist_dirty_;
    whitelist_dirty_ = false;
    return dirty;
}

std::optional<proto::alarm::Heartbeat> LinkService::last_heartbeat() const noexcept
{
    return last_hb_;
}

const LinkStats &LinkService::stats() const noexcept
{
    return stats_;
}

uint32_t LinkService::parser_crc_errors() const noexcept
{
    uint32_t total = retired_crc_errors_;
    for (const Slot &slot : slots_) {
        if (slot.in_use) {
            total += slot.parser.crc_errors();
        }
    }
    return total;
}

uint32_t LinkService::parser_length_errors() const noexcept
{
    uint32_t total = retired_length_errors_;
    for (const Slot &slot : slots_) {
        if (slot.in_use) {
            total += slot.parser.length_errors();
        }
    }
    return total;
}

} // namespace core
