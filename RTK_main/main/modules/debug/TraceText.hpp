// 追踪句子模板与字段表：按类别、事件码解释数值，未知码使用兜底格式（ADR-0009 §2.11）。
// 仅 DebugModule 输出任务格式化，非 ISR；只读查询函数无状态。
// 不分配堆、不阻塞；模板用 {key} 引用字段，未匹配占位符原样保留。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fw/Trace.hpp"

namespace app::debug {

/** 字段取值的渲染方式；决定 a/b/session 如何变成文本。 */
enum class TraceValue : uint8_t {
    kDecA = 0,        // a 十进制
    kHexA,            // a 十六进制
    kDecB,            // b 十进制
    kHexB,            // b 十六进制
    kRoleA,           // a 作为 core::Role
    kRoleB,           // b 作为 core::Role
    kReasonA,         // a 作为 core::LinkReason
    kReasonB,         // b 作为 core::LinkReason
    kResultA,         // a 作为 fw::Result
    kResultB,         // b 作为 fw::Result
    kGnssStateA,      // a 作为 gnss::State
    kGnssStateB,      // b 作为 gnss::State
    kGnssFamilyA,     // a 作为 gnss::Family
    kGnssModelA,      // a 作为 gnss::Model
    kSppStageA,       // a 作为 spp_server_trace_stage_t
    kSppCtrlA,        // a 作为 spp_server_trace_ctrl_t
    kShutdownStageA,  // a 作为 board::ShutdownStage
    kShutdownReasonA, // a 作为 board::ShutdownReason
    kShutdownReasonB, // b 作为 board::ShutdownReason
    kSession,         // 事件会话号（4 位十六进制）
    kNoValue,         // 只打印键名
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

/**
 * @brief 全部已知事件码；未知码由 formatEvent 用兜底格式输出。
 * @retval 全部已知事件码的静态文本定义视图
 * @note 只读常量表；不阻塞，线程/ISR 安全。
 */
[[nodiscard]] std::span<const TraceText> traceTable() noexcept;

/**
 * @brief 类别的大写短名（LINK/SPP/RTCM/PROTO/ALARM/STORE/EXEC/GNSS）；越界返回 "?"。
 * @param cat: 追踪类别
 * @retval 类别对应的大写短名；越界返回 ?
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *traceCatName(fw::TraceCat cat) noexcept;

/**
 * @brief 类别名 → 类别，大小写不敏感；未知名称返回 false 且不改 out。
 * @param name: 大小写不敏感的类别名
 * @param[out] out: 类别输出，失败不变
 * @retval true 已更新 out
 * @retval false 名称未知，out 保持原值
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool parseCat(std::string_view name, fw::TraceCat &out) noexcept;

/**
 * @brief  把一个 trace 事件格式化为一行文本：`[LINK] session 0x0081 opened as ALARM (whitelisted)`
 * @param  ev: 事件
 * @param[out] out: 输出缓冲（不含时间前缀与换行）；未知码输出 `[LINK] code 77 a=3 b=4`
 * @param  cap: out 容量；不足时截断并保持 0 结尾
 */
void formatEvent(const fw::TraceEvent &ev, char *out, size_t cap) noexcept;

/**
 * @brief 一组的一句话说明（`debug list` 用），未知类别返回空串。
 * @param cat: 追踪类别
 * @retval 类别的一句话说明；越界返回空串
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *traceCatSummary(fw::TraceCat cat) noexcept;

} // namespace app::debug
