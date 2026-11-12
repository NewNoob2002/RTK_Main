// 追踪文本表：把 (类别, 事件码, a, b) 解释成一行人可读的事件描述（ADR-0009 §2.1）。
// 线程：只由 DebugModule 的输出任务调用（唯一格式化者），非 ISR；不分配内存、不阻塞。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fw/trace.hpp"

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
    kSession,     // 事件会话号（4 位十六进制）
    kNoValue,     // 只打印键名
};

/** 一个 `键=值` 字段。 */
struct TraceField {
    const char *key;
    TraceValue value;
};

/** 一个事件码的文本定义；code 与 cat 一起定位。 */
struct TraceText {
    fw::TraceCat cat;
    uint8_t code;
    const char *name;
    std::span<const TraceField> fields;
};

/** 全部已知事件码；未知码由 format_event 用兜底格式输出。 */
[[nodiscard]] std::span<const TraceText> trace_table() noexcept;

/** 类别名（link/spp/rtcm/proto/alarm/store/exec）；越界返回 "?"。 */
[[nodiscard]] const char *trace_cat_name(fw::TraceCat cat) noexcept;

/** 类别名 → 类别；未知名称返回 false 且不改 out。 */
[[nodiscard]] bool parse_cat(std::string_view name, fw::TraceCat &out) noexcept;

/**
 * 一行事件文本：`link  open  h=0081 role=ALARM why=whitelisted`（不含时间前缀与换行）。
 * 未知码输出 `cat=… code=… a=… b=…`。cap 不足时截断并保持 0 结尾。
 */
void format_event(const fw::TraceEvent &ev, char *out, size_t cap) noexcept;

/** 一组的单行说明（`debug list` 用），未知类别返回空串。 */
[[nodiscard]] const char *trace_cat_summary(fw::TraceCat cat) noexcept;

} // namespace app::debug
