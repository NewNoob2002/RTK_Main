// link_core 发出的追踪事件码（ADR-0009 §2.2）：link / proto / alarm 三个类别。
// 参数含义写在每个码后面；main 的 DebugModule 按此表格式化。新增码只能追加在各枚举末尾。
// 冻结接口（ADR-0009 §3）。
#pragma once

#include <cstdint>

#include "fw/Trace.hpp"

namespace core {

inline constexpr uint32_t kTraceSessionMask = 0xFFFFU; // TraceEvent::session 只存句柄低 16 位

/** TraceCat::kLink；session = 句柄低 16 位。 */
enum class LinkTrace : uint8_t {
    kOpen = 0,       // a = 判定角色 core::Role，b = 判定依据 LinkReason
    kClose,          // a = 关闭前角色
    kKick,           // 策略要求断开：a = 被断开会话的角色，b = LinkReason
    kAlarmRxTimeout, // 报警器超过 kAlarmRxTimeoutMs 无数据：a = 静默 ms
    kPendingTimeout, // 待定会话超过 kPendingTimeoutMs 未发 HELLO：a = 等待 ms
    kRoleChange,     // on_frame 改变角色（HELLO 等）：a = 旧角色，b = 新角色
    kPairOpen,       // 配对窗口打开：a = 时长 ms
    kPairClose,      // 配对窗口关闭：a = 1 超时 / 0 主动
    kWhitelistAdd,   // a = 白名单当前数量
    kWhitelistClear,
    kWhitelistRemove, // a = 白名单当前数量
    kCloseCancel,     // CLOSE 清理在途挂起帧与 RX 尾部：a = 取消挂起帧数(0/1)，b = 丢弃 RX 尾部字节
};

/** kOpen/kKick 的判定依据。 */
enum class LinkReason : uint8_t {
    kWhitelisted = 0, // 白名单内 → 报警器
    kFirstPhone,      // 非白名单且无手机 → 手机
    kPendingHello,    // 已有手机，等待 HELLO
    kPairing,         // 配对窗口内接入
    kPolicyPreempt,   // PHONE_PREEMPT 策略踢出/拒绝
    kNoSlot,          // 会话表满
    kOther,
};

/** TraceCat::kProto；session = 句柄低 16 位。 */
enum class ProtoTrace : uint8_t {
    kFrame = 0,  // 收到完整产品帧：a = (msg_id << 8) | type，b = 负载长度
    kReply,      // 应答已排队：a = msg_id，b = 结果（fw::Result 数值）
    kNoRoute,    // 无对应处理函数或角色不允许：a = msg_id，b = 发送方角色
    kBadCrc,     // a = 已丢弃字节数
    kBadLength,  // a = 声明长度
    kReplyError, // 应答写失败：a = 错误码
};

/** TraceCat::kAlarm。 */
enum class AlarmTrace : uint8_t {
    kState = 0, // 状态/级别变化：a = state，b = level
    kOverride,  // 控制台 override：a = 1 设置 / 0 清除，b = (state << 8) | level
    kPush,      // 推送给报警器：session = 目标，a = state，b = level
    kPushFail,  // a = 错误码
};

/** 带类型的便捷包装；参数求值昂贵时，调用方先用 fw::trace_on() 判断。 */
inline void traceLink(LinkTrace code, uint32_t session, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kLink, static_cast<uint8_t>(code),
              static_cast<uint16_t>(session & kTraceSessionMask), a, b);
}

inline void traceProto(ProtoTrace code, uint32_t session, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kProto, static_cast<uint8_t>(code),
              static_cast<uint16_t>(session & kTraceSessionMask), a, b);
}

inline void traceAlarm(AlarmTrace code, uint32_t session, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kAlarm, static_cast<uint8_t>(code),
              static_cast<uint16_t>(session & kTraceSessionMask), a, b);
}

} // namespace core
