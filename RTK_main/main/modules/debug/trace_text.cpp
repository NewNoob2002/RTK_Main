// 追踪事件文本表与格式化（ADR-0009 §2.1）：把二进制事件翻成一行人可读文本。
// 线程：仅 DebugModule 输出任务调用（唯一格式化者），非 ISR；无堆、无阻塞、不调用日志。

#include "modules/debug/trace_text.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>

#include "core/trace_codes.hpp"
#include "def/rtcm_def.hpp"
#include "def/storage_def.hpp"
#include "def/system_def.hpp"
#include "spp_server.h"

namespace app::debug {

namespace {

// 字段表：每个事件码一项，键名与 trace_codes.hpp 的参数说明一一对应。
constexpr std::array<TraceField, 3U> kLinkOpen = {
    {{"h", TraceValue::kSession}, {"role", TraceValue::kRoleA}, {"why", TraceValue::kReasonB}}};
constexpr std::array<TraceField, 2U> kLinkClose = {{{"h", TraceValue::kSession}, {"role", TraceValue::kRoleA}}};
constexpr std::array<TraceField, 3U> kLinkKick = {
    {{"h", TraceValue::kSession}, {"role", TraceValue::kRoleA}, {"why", TraceValue::kReasonB}}};
constexpr std::array<TraceField, 2U> kLinkTimeout = {{{"h", TraceValue::kSession}, {"ms", TraceValue::kDecA}}};
constexpr std::array<TraceField, 3U> kLinkRoleChange = {
    {{"h", TraceValue::kSession}, {"from", TraceValue::kRoleA}, {"to", TraceValue::kRoleB}}};
constexpr std::array<TraceField, 1U> kLinkPairOpen = {{{"window_ms", TraceValue::kDecA}}};
constexpr std::array<TraceField, 1U> kLinkPairClose = {{{"timeout", TraceValue::kDecA}}};
constexpr std::array<TraceField, 1U> kLinkWhitelistAdd = {{{"count", TraceValue::kDecA}}};
constexpr std::array<TraceField, 0U> kLinkWhitelistClear = {{}};
constexpr std::array<TraceField, 1U> kLinkWhitelistRemove = {{{"count", TraceValue::kDecA}}};

constexpr std::array<TraceField, 3U> kProtoFrame = {
    {{"h", TraceValue::kSession}, {"id", TraceValue::kHexA}, {"len", TraceValue::kDecB}}};
constexpr std::array<TraceField, 3U> kProtoReply = {
    {{"h", TraceValue::kSession}, {"msg", TraceValue::kHexA}, {"result", TraceValue::kResultB}}};
constexpr std::array<TraceField, 2U> kProtoNoRoute = {{{"msg", TraceValue::kHexA}, {"role", TraceValue::kRoleB}}};
constexpr std::array<TraceField, 1U> kProtoBadCrc = {{{"dropped", TraceValue::kDecA}}};
constexpr std::array<TraceField, 1U> kProtoBadLength = {{{"declared", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kProtoReplyError = {{{"h", TraceValue::kSession}, {"err", TraceValue::kResultA}}};

constexpr std::array<TraceField, 2U> kAlarmState = {{{"state", TraceValue::kDecA}, {"level", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kAlarmOverride = {
    {{"set", TraceValue::kDecA}, {"state_level", TraceValue::kHexB}}};
constexpr std::array<TraceField, 3U> kAlarmPush = {
    {{"h", TraceValue::kSession}, {"state", TraceValue::kDecA}, {"level", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kAlarmPushFail = {{{"h", TraceValue::kSession}, {"err", TraceValue::kDecA}}};

constexpr std::array<TraceField, 2U> kStoreResult = {{{"result", TraceValue::kResultA}, {"ok", TraceValue::kDecB}}};
constexpr std::array<TraceField, 1U> kStoreDebounce = {{{"ms", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kStoreRetry = {{{"retry_ms", TraceValue::kDecA}, {"fails", TraceValue::kDecB}}};

constexpr std::array<TraceField, 2U> kExecOverrun = {{{"ms", TraceValue::kDecA}, {"budget", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kExecCallDrop = {
    {{"target", TraceValue::kDecA}, {"timeout_ms", TraceValue::kDecB}}};

constexpr std::array<TraceField, 3U> kRtcmFrame = {
    {{"h", TraceValue::kSession}, {"msg", TraceValue::kDecA}, {"len", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kRtcmReject = {{{"why", TraceValue::kDecA}, {"len", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kRtcmResync = {{{"h", TraceValue::kSession}, {"count", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kRtcmUnknown = {{{"h", TraceValue::kSession}, {"bytes", TraceValue::kDecA}}};

constexpr std::array<TraceField, 1U> kSppStart = {{{"scn", TraceValue::kDecA}}};
constexpr std::array<TraceField, 1U> kSppCong = {{{"cong", TraceValue::kDecA}}};
constexpr std::array<TraceField, 3U> kSppWriteDone = {
    {{"h", TraceValue::kSession}, {"bytes", TraceValue::kDecA}, {"ms", TraceValue::kDecB}}};
constexpr std::array<TraceField, 3U> kSppWriteErr = {
    {{"h", TraceValue::kSession}, {"err", TraceValue::kDecA}, {"bytes", TraceValue::kDecB}}};
constexpr std::array<TraceField, 3U> kSppRxDrop = {
    {{"h", TraceValue::kSession}, {"dropped", TraceValue::kDecA}, {"used", TraceValue::kDecB}}};
constexpr std::array<TraceField, 1U> kSppAuth = {{{"status", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kSppEvtDrop = {{{"type", TraceValue::kDecA}, {"total", TraceValue::kDecB}}};

// 每项 fields 用静态表中的 span，避免额外的可变数据。
constexpr std::array<TraceText, 38U> kTable = {{
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kOpen), "open", std::span<const TraceField>(kLinkOpen)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kClose), "close",
     std::span<const TraceField>(kLinkClose)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kKick), "kick", std::span<const TraceField>(kLinkKick)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kAlarmRxTimeout), "alarm-timeout",
     std::span<const TraceField>(kLinkTimeout)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kPendingTimeout), "pending-timeout",
     std::span<const TraceField>(kLinkTimeout)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kRoleChange), "role",
     std::span<const TraceField>(kLinkRoleChange)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kPairOpen), "pair-open",
     std::span<const TraceField>(kLinkPairOpen)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kPairClose), "pair-close",
     std::span<const TraceField>(kLinkPairClose)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kWhitelistAdd), "wl-add",
     std::span<const TraceField>(kLinkWhitelistAdd)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kWhitelistClear), "wl-clear",
     std::span<const TraceField>(kLinkWhitelistClear)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kWhitelistRemove), "wl-del",
     std::span<const TraceField>(kLinkWhitelistRemove)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kFrame), "frame",
     std::span<const TraceField>(kProtoFrame)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kReply), "reply",
     std::span<const TraceField>(kProtoReply)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kNoRoute), "no-route",
     std::span<const TraceField>(kProtoNoRoute)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kBadCrc), "bad-crc",
     std::span<const TraceField>(kProtoBadCrc)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kBadLength), "bad-length",
     std::span<const TraceField>(kProtoBadLength)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kReplyError), "reply-err",
     std::span<const TraceField>(kProtoReplyError)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kState), "state",
     std::span<const TraceField>(kAlarmState)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kOverride), "override",
     std::span<const TraceField>(kAlarmOverride)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kPush), "push",
     std::span<const TraceField>(kAlarmPush)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kPushFail), "push-fail",
     std::span<const TraceField>(kAlarmPushFail)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kLoad), "load", std::span<const TraceField>(kStoreResult)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kSave), "save", std::span<const TraceField>(kStoreResult)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kDebounce), "debounce",
     std::span<const TraceField>(kStoreDebounce)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kRetry), "retry", std::span<const TraceField>(kStoreRetry)},
    {fw::TraceCat::kExec, static_cast<uint8_t>(ExecTrace::kRoundOverrun), "overrun",
     std::span<const TraceField>(kExecOverrun)},
    {fw::TraceCat::kExec, static_cast<uint8_t>(ExecTrace::kCallDrop), "call-drop",
     std::span<const TraceField>(kExecCallDrop)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kFrame), "frame", std::span<const TraceField>(kRtcmFrame)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kReject), "reject", std::span<const TraceField>(kRtcmReject)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kResync), "resync", std::span<const TraceField>(kRtcmResync)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kUnknown), "unknown",
     std::span<const TraceField>(kRtcmUnknown)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_SRV_START), "start",
     std::span<const TraceField>(kSppStart)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_CONG), "cong", std::span<const TraceField>(kSppCong)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_WRITE_DONE), "write",
     std::span<const TraceField>(kSppWriteDone)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_WRITE_ERR), "write-err",
     std::span<const TraceField>(kSppWriteErr)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_RX_DROP), "rx-drop",
     std::span<const TraceField>(kSppRxDrop)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_AUTH), "auth", std::span<const TraceField>(kSppAuth)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_EVT_DROP), "evt-drop",
     std::span<const TraceField>(kSppEvtDrop)},
}};

constexpr std::array<const char *, 7U> kCatNames = {{"link", "spp", "rtcm", "proto", "alarm", "store", "exec"}};
constexpr std::array<const char *, 7U> kCatSummaries = {{
    "会话打开/关闭、角色判定、白名单、配对窗口",
    "经典蓝牙 SPP 事件、写入耗时、接收丢弃",
    "RTCM 帧计数、角色拒绝、重同步、未知字节",
    "产品协议帧、应答、无路由、CRC/长度错误",
    "报警状态、测试 override、状态推送",
    "NVS 读写结果、去抖触发、重试间隔",
    "执行器轮次超时、命令投递失败",
}};

// 追加一段文本；写满时停在末尾并保持 0 结尾。
void append(char *out, size_t cap, size_t &pos, const char *fmt, ...) noexcept
{
    if ((out == nullptr) || (cap == 0U) || (pos >= (cap - 1U))) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(&out[pos], cap - pos, fmt, args);
    va_end(args);
    if (written <= 0) {
        return;
    }
    const size_t len = static_cast<size_t>(written);
    // 返回值为“若空间足够会写多少”，因此越界时直接顶到末尾。
    pos = (len < (cap - pos)) ? (pos + len) : (cap - 1U);
}

const char *role_text(uint32_t value) noexcept
{
    switch (static_cast<core::Role>(value)) {
    case core::Role::kNone:
        return "NONE";
    case core::Role::kPhone:
        return "PHONE";
    case core::Role::kAlarm:
        return "ALARM";
    case core::Role::kPending:
        return "PENDING";
    default:
        return "?";
    }
}

const char *reason_text(uint32_t value) noexcept
{
    switch (static_cast<core::LinkReason>(value)) {
    case core::LinkReason::kWhitelisted:
        return "whitelisted";
    case core::LinkReason::kFirstPhone:
        return "first-phone";
    case core::LinkReason::kPendingHello:
        return "pending-hello";
    case core::LinkReason::kPairing:
        return "pairing";
    case core::LinkReason::kPolicyPreempt:
        return "preempt";
    case core::LinkReason::kNoSlot:
        return "no-slot";
    case core::LinkReason::kOther:
        return "other";
    default:
        return "?";
    }
}

const char *result_text(uint32_t value) noexcept
{
    // fw::Result 与 fw::Status 的数值定义一致（0=成功，1..8 为错误码）。
    switch (static_cast<fw::Result>(value)) {
    case fw::Result::kOk:
        return "OK";
    case fw::Result::kInvalidArg:
        return "EINVAL";
    case fw::Result::kNoSpace:
        return "ENOSPC";
    case fw::Result::kTimeout:
        return "ETIMEDOUT";
    case fw::Result::kNotFound:
        return "ENOENT";
    case fw::Result::kIoError:
        return "EIO";
    case fw::Result::kNotSupported:
        return "ENOTSUP";
    case fw::Result::kBusy:
        return "EBUSY";
    case fw::Result::kInvalidState:
        return "ESTATE";
    default:
        return "?";
    }
}

void append_value(char *out, size_t cap, size_t &pos, TraceValue value, const fw::TraceEvent &ev) noexcept
{
    switch (value) {
    case TraceValue::kDecA:
        append(out, cap, pos, "%lu", static_cast<unsigned long>(ev.a));
        break;
    case TraceValue::kHexA:
        append(out, cap, pos, "0x%lx", static_cast<unsigned long>(ev.a));
        break;
    case TraceValue::kDecB:
        append(out, cap, pos, "%lu", static_cast<unsigned long>(ev.b));
        break;
    case TraceValue::kHexB:
        append(out, cap, pos, "0x%lx", static_cast<unsigned long>(ev.b));
        break;
    case TraceValue::kRoleA:
        append(out, cap, pos, "%s", role_text(ev.a));
        break;
    case TraceValue::kRoleB:
        append(out, cap, pos, "%s", role_text(ev.b));
        break;
    case TraceValue::kReasonA:
        append(out, cap, pos, "%s", reason_text(ev.a));
        break;
    case TraceValue::kReasonB:
        append(out, cap, pos, "%s", reason_text(ev.b));
        break;
    case TraceValue::kResultA:
        append(out, cap, pos, "%s", result_text(ev.a));
        break;
    case TraceValue::kResultB:
        append(out, cap, pos, "%s", result_text(ev.b));
        break;
    case TraceValue::kSession:
        append(out, cap, pos, "%04lx", static_cast<unsigned long>(ev.session));
        break;
    case TraceValue::kNoValue:
        break;
    default:
        append(out, cap, pos, "%lu", static_cast<unsigned long>(ev.a));
        break;
    }
}

} // namespace

std::span<const TraceText> trace_table() noexcept
{
    return {kTable};
}

const char *trace_cat_name(fw::TraceCat cat) noexcept
{
    const uint32_t index = static_cast<uint32_t>(cat);
    if (index >= kCatNames.size()) {
        return "?";
    }
    return kCatNames[index];
}

const char *trace_cat_summary(fw::TraceCat cat) noexcept
{
    const uint32_t index = static_cast<uint32_t>(cat);
    if (index >= kCatSummaries.size()) {
        return "";
    }
    return kCatSummaries[index];
}

bool parse_cat(std::string_view name, fw::TraceCat &out) noexcept
{
    constexpr uint32_t kCount = static_cast<uint32_t>(fw::TraceCat::kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        if (name == kCatNames[i]) {
            out = static_cast<fw::TraceCat>(i);
            return true;
        }
    }
    return false;
}

void format_event(const fw::TraceEvent &ev, char *out, size_t cap) noexcept
{
    if ((out == nullptr) || (cap == 0U)) {
        return;
    }
    out[0] = '\0';
    size_t pos = 0U;
    const auto table = trace_table();
    for (const TraceText &entry : table) {
        if ((entry.cat != ev.cat) || (entry.code != ev.code)) {
            continue;
        }
        append(out, cap, pos, "%s  %s", trace_cat_name(ev.cat), entry.name);
        for (const TraceField &field : entry.fields) {
            append(out, cap, pos, " %s=", field.key);
            append_value(out, cap, pos, field.value, ev);
        }
        return;
    }
    // 未知码：保留原始数值，便于对照新加的调用点。
    append(out, cap, pos, "cat=%s code=%u a=%lu b=%lu", trace_cat_name(ev.cat), static_cast<unsigned>(ev.code),
           static_cast<unsigned long>(ev.a), static_cast<unsigned long>(ev.b));
}

} // namespace app::debug
