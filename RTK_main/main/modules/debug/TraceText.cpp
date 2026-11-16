// 追踪事件文本表与句子渲染（ADR-0009 §2.1/§2.11）：把二进制事件翻成一行人可读文本。
// 线程：仅 DebugModule 输出任务调用（唯一格式化者），非 ISR；无堆、无阻塞、不调用日志。

#include "modules/debug/TraceText.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string_view>

#include "AppConfig.hpp" // RTK_GNSS_ENABLED
#include "core/TraceCodes.hpp"
#if RTK_GNSS_ENABLED
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"
#endif
#include "def/RtcmDef.hpp"
#include "def/StorageDef.hpp"
#include "def/SystemDef.hpp"
#include "SppServer.h"

namespace app::debug {

namespace {

// 字段表：每个事件码一项，键名与句子模板里的 {占位符} 一一对应。
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
constexpr std::array<TraceField, 3U> kLinkCloseCancel = {
    {{"h", TraceValue::kSession}, {"rx", TraceValue::kDecA}, {"paused", TraceValue::kDecB}}};

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
constexpr std::array<TraceField, 2U> kAlarmPushFail = {{{"h", TraceValue::kSession}, {"err", TraceValue::kResultA}}};

constexpr std::array<TraceField, 2U> kStoreResult = {{{"result", TraceValue::kResultA}, {"ok", TraceValue::kDecB}}};
constexpr std::array<TraceField, 1U> kStoreDebounce = {{{"ms", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kStoreRetry = {{{"retry_ms", TraceValue::kDecA}, {"fails", TraceValue::kDecB}}};

constexpr std::array<TraceField, 2U> kExecOverrun = {{{"ms", TraceValue::kDecA}, {"budget", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kExecCallDrop = {
    {{"target", TraceValue::kDecA}, {"timeout_ms", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kExecResultDrop = {{{"seq", TraceValue::kDecA}, {"old", TraceValue::kDecB}}};
constexpr std::array<TraceField, 1U> kExecWdtFail = {{{"err", TraceValue::kHexA}}};

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
    {{"h", TraceValue::kSession}, {"err", TraceValue::kResultA}, {"bytes", TraceValue::kDecB}}};
constexpr std::array<TraceField, 3U> kSppRxDrop = {
    {{"h", TraceValue::kSession}, {"dropped", TraceValue::kDecA}, {"used", TraceValue::kDecB}}};
constexpr std::array<TraceField, 1U> kSppAuth = {{{"status", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kSppEvtDrop = {{{"type", TraceValue::kDecA}, {"total", TraceValue::kDecB}}};
constexpr std::array<TraceField, 3U> kSppStage = {
    {{"h", TraceValue::kSession}, {"stage", TraceValue::kSppStageA}, {"status", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kSppCtrlFull = {{{"h", TraceValue::kSession}, {"ctrl", TraceValue::kSppCtrlA}}};
constexpr std::array<TraceField, 3U> kSppCtrlErr = {
    {{"h", TraceValue::kSession}, {"ctrl", TraceValue::kSppCtrlA}, {"err", TraceValue::kHexB}}};
constexpr std::array<TraceField, 2U> kSppReject = {{{"h", TraceValue::kSession}, {"sent", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kSppWriteAnon = {
    {{"status", TraceValue::kDecA}, {"candidates", TraceValue::kHexB}}};
constexpr std::array<TraceField, 3U> kSppWriteStray = {
    {{"h", TraceValue::kSession}, {"bytes", TraceValue::kDecA}, {"status", TraceValue::kDecB}}};

#if RTK_GNSS_ENABLED
constexpr std::array<TraceField, 2U> kGnssState = {
    {{"state", TraceValue::kGnssStateA}, {"from", TraceValue::kGnssStateB}}};
constexpr std::array<TraceField, 2U> kGnssProbeSend = {
    {{"family", TraceValue::kGnssFamilyA}, {"baud", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kGnssProbeHit = {
    {{"model", TraceValue::kGnssModelA}, {"baud", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kGnssStepRetry = {{{"step", TraceValue::kDecA}, {"retry", TraceValue::kDecB}}};
constexpr std::array<TraceField, 1U> kGnssStep = {{{"step", TraceValue::kDecA}}};
constexpr std::array<TraceField, 1U> kGnssSilence = {{{"ms", TraceValue::kDecA}}};
constexpr std::array<TraceField, 2U> kGnssPipeDrop = {{{"len", TraceValue::kDecA}, {"space", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kGnssBaud = {{{"baud", TraceValue::kDecA}, {"from", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kGnssAppMask = {{{"mask", TraceValue::kHexA}, {"disc", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kGnssAppDrop = {{{"kind", TraceValue::kDecA}, {"type", TraceValue::kDecB}}};
constexpr std::array<TraceField, 2U> kGnssOutCfg = {{{"mask", TraceValue::kHexA}, {"saved", TraceValue::kDecB}}};
#endif

// 每项 fields 用静态表中的 span，避免额外的可变数据。
#if RTK_GNSS_ENABLED
constexpr std::array<TraceText, 60U> kTable = {{
#else
constexpr std::array<TraceText, 47U> kTable = {{
#endif
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kOpen), "session {h} opened as {role} ({why})",
     std::span<const TraceField>(kLinkOpen)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kClose), "session {h} closed ({role})",
     std::span<const TraceField>(kLinkClose)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kKick), "session {h} kicked ({role}, {why})",
     std::span<const TraceField>(kLinkKick)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kAlarmRxTimeout),
     "session {h} alarm reply timeout after {ms} ms", std::span<const TraceField>(kLinkTimeout)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kPendingTimeout),
     "session {h} pending hello timeout after {ms} ms", std::span<const TraceField>(kLinkTimeout)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kRoleChange), "session {h} role {from} -> {to}",
     std::span<const TraceField>(kLinkRoleChange)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kPairOpen), "pairing window opened for {window_ms} ms",
     std::span<const TraceField>(kLinkPairOpen)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kPairClose), "pairing window closed after {timeout} ms",
     std::span<const TraceField>(kLinkPairClose)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kWhitelistAdd), "whitelist add, {count} entries",
     std::span<const TraceField>(kLinkWhitelistAdd)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kWhitelistClear), "whitelist cleared",
     std::span<const TraceField>(kLinkWhitelistClear)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kWhitelistRemove),
     "whitelist remove, {count} entries left", std::span<const TraceField>(kLinkWhitelistRemove)},
    {fw::TraceCat::kLink, static_cast<uint8_t>(core::LinkTrace::kCloseCancel),
     "session {h} close cancelled {rx} rx bytes (paused={paused})", std::span<const TraceField>(kLinkCloseCancel)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kFrame), "session {h} frame {id}, {len} bytes",
     std::span<const TraceField>(kProtoFrame)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kReply), "session {h} reply to {msg}: {result}",
     std::span<const TraceField>(kProtoReply)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kNoRoute), "message {msg} has no route for {role}",
     std::span<const TraceField>(kProtoNoRoute)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kBadCrc), "dropped {dropped} frames: bad CRC",
     std::span<const TraceField>(kProtoBadCrc)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kBadLength),
     "dropped frame, declared {declared} bytes", std::span<const TraceField>(kProtoBadLength)},
    {fw::TraceCat::kProto, static_cast<uint8_t>(core::ProtoTrace::kReplyError), "session {h} reply failed: {err}",
     std::span<const TraceField>(kProtoReplyError)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kState), "state {state}, level {level}",
     std::span<const TraceField>(kAlarmState)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kOverride),
     "override {set}, state/level {state_level}", std::span<const TraceField>(kAlarmOverride)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kPush),
     "session {h} push state {state}, level {level}", std::span<const TraceField>(kAlarmPush)},
    {fw::TraceCat::kAlarm, static_cast<uint8_t>(core::AlarmTrace::kPushFail), "session {h} push failed: {err}",
     std::span<const TraceField>(kAlarmPushFail)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kLoad), "{result}, {ok} keys",
     std::span<const TraceField>(kStoreResult)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kSave), "{result}, {ok} keys",
     std::span<const TraceField>(kStoreResult)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kDebounce), "save deferred by {ms} ms",
     std::span<const TraceField>(kStoreDebounce)},
    {fw::TraceCat::kStore, static_cast<uint8_t>(StoreTrace::kRetry), "retry in {retry_ms} ms after {fails} failures",
     std::span<const TraceField>(kStoreRetry)},
    {fw::TraceCat::kExec, static_cast<uint8_t>(ExecTrace::kRoundOverrun), "round took {ms} ms, budget {budget} ms",
     std::span<const TraceField>(kExecOverrun)},
    {fw::TraceCat::kExec, static_cast<uint8_t>(ExecTrace::kCallDrop), "call to {target} dropped after {timeout_ms} ms",
     std::span<const TraceField>(kExecCallDrop)},
    {fw::TraceCat::kExec, static_cast<uint8_t>(ExecTrace::kResultDrop),
     "result of command {seq} lost, result queue still full after dropping {old} old results",
     std::span<const TraceField>(kExecResultDrop)},
    {fw::TraceCat::kExec, static_cast<uint8_t>(ExecTrace::kWdtResetFail),
     "task watchdog reset failed with error {err}, watchdog feeding stopped",
     std::span<const TraceField>(kExecWdtFail)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kFrame), "session {h} frame {msg}, {len} bytes",
     std::span<const TraceField>(kRtcmFrame)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kReject), "frame rejected, reason {why}, {len} bytes",
     std::span<const TraceField>(kRtcmReject)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kResync), "session {h} resync {count}",
     std::span<const TraceField>(kRtcmResync)},
    {fw::TraceCat::kRtcm, static_cast<uint8_t>(RtcmTrace::kUnknown), "session {h} dropped {bytes} unknown bytes",
     std::span<const TraceField>(kRtcmUnknown)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_SRV_START), "server started, scn {scn}",
     std::span<const TraceField>(kSppStart)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_CONG), "congestion {cong}",
     std::span<const TraceField>(kSppCong)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_WRITE_DONE),
     "session {h} wrote {bytes} bytes in {ms} ms", std::span<const TraceField>(kSppWriteDone)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_WRITE_ERR),
     "session {h} write failed {err} after {bytes} bytes", std::span<const TraceField>(kSppWriteErr)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_RX_DROP),
     "session {h} dropped {dropped} rx bytes (used {used})", std::span<const TraceField>(kSppRxDrop)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_AUTH), "auth status {status}",
     std::span<const TraceField>(kSppAuth)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_EVT_DROP), "event {type} dropped, total {total}",
     std::span<const TraceField>(kSppEvtDrop)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_STAGE),
     "{stage} finished with status {status} (session {h})", std::span<const TraceField>(kSppStage)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_CTRL_FULL),
     "control queue full, {ctrl} reply lost (session {h})", std::span<const TraceField>(kSppCtrlFull)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_CTRL_ERR),
     "{ctrl} request failed with error {err} (session {h})", std::span<const TraceField>(kSppCtrlErr)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_REJECT),
     "session {h} rejected, no free secure slot (disconnect sent {sent})", std::span<const TraceField>(kSppReject)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_WRITE_STRAY),
     "stray write completion ignored, {bytes} bytes, status {status} (session {h})",
     std::span<const TraceField>(kSppWriteStray)},
    {fw::TraceCat::kSpp, static_cast<uint8_t>(SPP_SERVER_TRACE_WRITE_ANON),
     "anonymous write failure {status}, disconnect candidates {candidates}",
     std::span<const TraceField>(kSppWriteAnon)},
#if RTK_GNSS_ENABLED
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kState), "state {state} (from {from})",
     std::span<const TraceField>(kGnssState)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kProbeSend), "probe sent on {family} at {baud} baud",
     std::span<const TraceField>(kGnssProbeSend)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kProbeHit), "{model} detected at {baud} baud",
     std::span<const TraceField>(kGnssProbeHit)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kStepSend), "config step {step} sent, retry {retry}",
     std::span<const TraceField>(kGnssStepRetry)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kStepAck), "config step {step} acknowledged",
     std::span<const TraceField>(kGnssStep)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kStepNack), "config step {step} rejected by receiver",
     std::span<const TraceField>(kGnssStep)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kStepTimeout), "config step {step} timed out, retry {retry}",
     std::span<const TraceField>(kGnssStepRetry)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kSilence), "no data for {ms} ms, reprobing",
     std::span<const TraceField>(kGnssSilence)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kPipeDrop),
     "RTCM pipe dropped {len} bytes, {space} bytes free", std::span<const TraceField>(kGnssPipeDrop)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssTrace::kBaud), "baud {baud} (was {from})",
     std::span<const TraceField>(kGnssBaud)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kAppMask), "app sub mask {mask} (disc={disc})",
     std::span<const TraceField>(kGnssAppMask)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kAppDrop), "app drop kind={kind} type={type}",
     std::span<const TraceField>(kGnssAppDrop)},
    {fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kOutCfg), "gnss out mask={mask} saved={saved}",
     std::span<const TraceField>(kGnssOutCfg)},
#endif
}};

// 类别短名必须大写（ADR-0009 §2.11）；顺序与 fw::TraceCat 枚举一致。
constexpr size_t kCatCount = static_cast<size_t>(fw::TraceCat::kCount);
// 名称与 fw::log_cat_name 一致（日志行与 trace 行同一套类别名）；数组长度跟随 kCount，漏加类别会编译失败。
constexpr std::array<const char *, kCatCount> kCatNames = {
    {"LINK", "SPP", "RTCM", "PROTO", "ALARM", "STORE", "EXEC", "GNSS", "SYS"}};
constexpr std::array<const char *, kCatCount> kCatSummaries = {{
    "session open/close, role decisions, whitelist, pairing window",
    "classic SPP events, write timing, rx drops",
    "RTCM frame counts, role rejects, resync, unknown bytes",
    "product frames, replies, no route, CRC/length errors",
    "alarm state, test override, state push",
    "NVS load/save results, debounce, retry delay",
    "executor round overrun, command delivery failure",
    "GNSS state machine, probe/config commands, RTCM pipe drops",
    "boot, console and system service logs",
}};

// 追加一段文本；写满时停在末尾并保持 0 结尾。
void append(char *out, size_t cap, size_t &pos, const char *fmt, ...) noexcept
{
    if ((out == nullptr) || (cap == 0U) || (pos >= (cap - 1U))) {
        return;
    }
    // 理由（下面三处 NOLINT）：C 可变参数只能走裸指针——va_list 在 x86_64 上就是数组类型、
    // vsnprintf 不接受 span；越界已由上面的 pos 检查排除，写入长度由返回值夹紧。
    va_list args;
    va_start(args, fmt); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay): va_list 即数组
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const int written = vsnprintf(&out[pos], cap - pos, fmt, args);
    va_end(args); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay): va_list 即数组
    if (written <= 0) {
        return;
    }
    const size_t len = static_cast<size_t>(written);
    // 返回值为“若空间足够会写多少”，因此越界时直接顶到末尾。
    pos = (len < (cap - pos)) ? (pos + len) : (cap - 1U);
}

bool key_equals(std::string_view a, const char *b) noexcept
{
    return a == std::string_view(b);
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

const char *spp_stage_text(uint32_t value) noexcept
{
    switch (value) {
    case SPP_SERVER_TRACE_STAGE_INIT:
        return "SPP init";
    case SPP_SERVER_TRACE_STAGE_START:
        return "SPP server start";
    case SPP_SERVER_TRACE_STAGE_UNINIT:
        return "SPP uninit";
    case SPP_SERVER_TRACE_STAGE_SRV_OPEN:
        return "incoming connection";
    default:
        return "unknown SPP stage";
    }
}

const char *spp_ctrl_text(uint32_t value) noexcept
{
    switch (value) {
    case SPP_SERVER_TRACE_CTRL_PIN:
        return "PIN reply";
    case SPP_SERVER_TRACE_CTRL_CONFIRM:
        return "SSP confirm";
    case SPP_SERVER_TRACE_CTRL_REJECT:
        return "reject disconnect";
    case SPP_SERVER_TRACE_CTRL_DISCONNECT:
        return "disconnect";
    default:
        return "unknown control";
    }
}

const char *reason_text(uint32_t value) noexcept
{
    switch (static_cast<core::LinkReason>(value)) {
    case core::LinkReason::kWhitelisted:
        return "whitelisted";
    case core::LinkReason::kFirstPhone:
        return "first phone";
    case core::LinkReason::kPendingHello:
        return "pending hello";
    case core::LinkReason::kPairing:
        return "pairing";
    case core::LinkReason::kPolicyPreempt:
        return "preempted";
    case core::LinkReason::kNoSlot:
        return "no free slot";
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

#if RTK_GNSS_ENABLED
const char *gnss_family_text(uint32_t value) noexcept
{
    // 家族名集中在 gnss 组件（ADR-0012 §2.2），越界保护也用它自己的取值范围。
    const auto family = ::gnss::family_from_u8(static_cast<uint8_t>(value));
    if (!family.has_value()) {
        return "?";
    }
    return ::gnss::family_name(*family);
}
#endif

void append_value(char *out, size_t cap, size_t &pos, TraceValue value, const fw::TraceEvent &ev) noexcept
{
    switch (value) {
    case TraceValue::kDecA:
        append(out, cap, pos, "%lu", static_cast<unsigned long>(ev.a));
        break;
    case TraceValue::kHexA:
        append(out, cap, pos, "0x%02lx", static_cast<unsigned long>(ev.a));
        break;
    case TraceValue::kDecB:
        append(out, cap, pos, "%lu", static_cast<unsigned long>(ev.b));
        break;
    case TraceValue::kHexB:
        append(out, cap, pos, "0x%02lx", static_cast<unsigned long>(ev.b));
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
#if RTK_GNSS_ENABLED
    case TraceValue::kGnssStateA:
        append(out, cap, pos, "%s", ::gnss::state_name(static_cast<::gnss::State>(ev.a)));
        break;
    case TraceValue::kGnssStateB:
        append(out, cap, pos, "%s", ::gnss::state_name(static_cast<::gnss::State>(ev.b)));
        break;
    case TraceValue::kGnssFamilyA:
        append(out, cap, pos, "%s", gnss_family_text(ev.a));
        break;
    case TraceValue::kGnssModelA:
        append(out, cap, pos, "%s", ::gnss::model_name(static_cast<::gnss::Model>(ev.a)));
        break;
#endif
    case TraceValue::kSppStageA:
        append(out, cap, pos, "%s", spp_stage_text(ev.a));
        break;
    case TraceValue::kSppCtrlA:
        append(out, cap, pos, "%s", spp_ctrl_text(ev.a));
        break;
    case TraceValue::kSession:
        append(out, cap, pos, "0x%04lx", static_cast<unsigned long>(ev.session));
        break;
    case TraceValue::kNoValue:
        break;
    default:
        append(out, cap, pos, "%lu", static_cast<unsigned long>(ev.a));
        break;
    }
}

/** 渲染 {key} 占位符：在字段表里按键名查找并用对应方式输出；查不到则原样保留。 */
void render_field(char *out, size_t cap, size_t &pos, std::string_view key, std::span<const TraceField> fields,
                  const fw::TraceEvent &ev) noexcept
{
    for (const TraceField &field : fields) {
        if (key_equals(key, field.key)) {
            append_value(out, cap, pos, field.value, ev);
            return;
        }
    }
    append(out, cap, pos, "{%.*s}", static_cast<int>(key.size()), key.data());
}

/** 按句子模板渲染一行；未闭合或空的 {} 原样输出，便于发现模板写错。 */
void render_sentence(std::string_view sentence, std::span<const TraceField> fields, const fw::TraceEvent &ev, char *out,
                     size_t cap, size_t &pos) noexcept
{
    // 用下标遍历模板，避免指针算术（项目规则禁止）。
    size_t i = 0U;
    while (i < sentence.size()) {
        if (sentence[i] != '{') {
            append(out, cap, pos, "%c", sentence[i]);
            ++i;
            continue;
        }
        size_t close = i + 1U;
        while ((close < sentence.size()) && (sentence[close] != '}')) {
            ++close;
        }
        if (close >= sentence.size()) { // 未闭合：原样输出剩余部分（逐字符，避免对非 0 结尾的视图取 data()）
            for (size_t k = i; k < sentence.size(); ++k) {
                append(out, cap, pos, "%c", sentence[k]);
            }
            return;
        }
        render_field(out, cap, pos, sentence.substr(i + 1U, close - i - 1U), fields, ev);
        i = close + 1U;
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
        const std::string_view candidate{kCatNames[i]};
        if (name.size() != candidate.size()) {
            continue;
        }
        bool same = true;
        for (size_t k = 0; k < name.size(); ++k) {
            // 只比较 ASCII、不区分大小写：控制台习惯输小写，表里是大写短名。
            const char upper = (name[k] >= 'a' && name[k] <= 'z') ? static_cast<char>(name[k] - 'a' + 'A') : name[k];
            if (upper != candidate[k]) {
                same = false;
                break;
            }
        }
        if (same) {
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
    const std::span<char> text{out, cap};
    text[0] = '\0';
    size_t pos = 0U;
    append(out, cap, pos, "[%s] ", trace_cat_name(ev.cat));
    for (const TraceText &entry : trace_table()) {
        if ((entry.cat != ev.cat) || (entry.code != ev.code)) {
            continue;
        }
        render_sentence(entry.sentence, entry.fields, ev, out, cap, pos);
        return;
    }
    // 未知码：保留原始数值，便于对照新加的调用点。
    append(out, cap, pos, "code %u a=%lu b=%lu", static_cast<unsigned>(ev.code), static_cast<unsigned long>(ev.a),
           static_cast<unsigned long>(ev.b));
}

} // namespace app::debug
