// 追踪事件的句子式文本表（ADR-0009 §2.11）：把 (类别, 事件码, a, b) 解释成一行英文句子。
// 线程：只由 DebugModule 的输出任务调用（唯一格式化者），非 ISR；不分配内存、不阻塞。
//
// 句子模板用 {key} 占位，占位符按字段表渲染（数值、枚举名、会话号等）；模板与字段表分离，
// 事件码与字段含义保持不变，仅更换呈现方式（§2.11）。没有对应模板时才退回 `key=value` 清单。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fw/Trace.hpp"

namespace app::debug {

/** 字段取值的渲染方式；决定 a/b/session 如何变成文本。 */
enum class TraceValue : uint8_t {
    kDecA = 0,    // a 十进制
    kHexA,        // a 十六进制
    kDecB,        // b 十进制
    kHexB,        // b 十六进制
    kRoleA,       // a 作为 core::Role
    kRoleB,       // b 作为 core::Role
    kReasonA,     // a 作为 core::LinkReason
    kReasonB,     // b 作为 core::LinkReason
    kResultA,     // a 作为 fw::Result
    kResultB,     // b 作为 fw::Result
    kGnssStateA,  // a 作为 gnss::State
    kGnssStateB,  // b 作为 gnss::State
    kGnssFamilyA, // a 作为 gnss::Family
    kGnssModelA,  // a 作为 gnss::Model
    kSppStageA,   // a 作为 spp_server_trace_stage_t
    kSppCtrlA,    // a 作为 spp_server_trace_ctrl_t
    kSession,     // 事件会话号（4 位十六进制）
    kNoValue,     // 只打印键名
};

/** 句子模板中的一个占位符，与 TraceText::sentence 里的 `{key}` 对应。 */
struct TraceField {
    const char *key;
    TraceValue value;
};

/** 一个事件码的文本定义；code 与 cat 一起定位。 */
struct TraceText {
    fw::TraceCat cat;
    uint8_t code;
    const char *sentence; // 例如 "session {h} opened as {role} ({why})"
    std::span<const TraceField> fields;
};

/** 全部已知事件码；未知码由 formatEvent 用兜底格式输出。 */
[[nodiscard]] std::span<const TraceText> traceTable() noexcept;

/** 类别的大写短名（LINK/SPP/RTCM/PROTO/ALARM/STORE/EXEC/GNSS）；越界返回 "?"。 */
[[nodiscard]] const char *traceCatName(fw::TraceCat cat) noexcept;

/** 类别名 → 类别，大小写不敏感；未知名称返回 false 且不改 out。 */
[[nodiscard]] bool parseCat(std::string_view name, fw::TraceCat &out) noexcept;

/**
 * @brief  把一个 trace 事件格式化为一行文本：`[LINK] session 0x0081 opened as ALARM (whitelisted)`
 * @param  ev: 事件
 * @param  out: 输出缓冲（不含时间前缀与换行）；未知码输出 `[LINK] code 77 a=3 b=4`
 * @param  cap: out 容量；不足时截断并保持 0 结尾
 */
void formatEvent(const fw::TraceEvent &ev, char *out, size_t cap) noexcept;

/** 一组的一句话说明（`debug list` 用），未知类别返回空串。 */
[[nodiscard]] const char *traceCatSummary(fw::TraceCat cat) noexcept;

} // namespace app::debug
