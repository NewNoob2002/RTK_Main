// 追踪事件值格式化：把字段值按类型翻成文本。
// 线程：仅 DebugModule 输出任务调用（唯一格式化者），非 ISR；无堆、无阻塞、不调用日志。

#include "modules/debug/TraceText.hpp"

#include <cstddef>
#include <cstdint>

#include "AppConfig.hpp" // RTK_GNSS_ENABLED
#include "SppServer.h"
#include "core/TraceCodes.hpp"
#include "board/PowerLogic.hpp"
#include "TraceTextInternal.hpp"
#if RTK_GNSS_ENABLED
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "gnss/Types.hpp"
#endif

namespace app::debug {

namespace {

using detail::append;

/**
 * @brief 把角色枚举翻成文本
 * @param value: 值类型
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *roleText(uint32_t value) noexcept
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

/**
 * @brief 把 SPP 阶段码翻成文本
 * @param value: 值类型
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *sppStageText(uint32_t value) noexcept
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

/**
 * @brief 把 SPP 控制码翻成文本
 * @param value: 值类型
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *sppCtrlText(uint32_t value) noexcept
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

/**
 * @brief 把链路原因码翻成文本
 * @param value: 值类型
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *reasonText(uint32_t value) noexcept
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

/**
 * @brief 把结果码翻成文本
 * @param value: 值类型
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *resultText(uint32_t value) noexcept
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
/**
 * @brief 把 GNSS 星座族翻成文本
 * @param value: 值类型
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *gnssFamilyText(uint32_t value) noexcept
{
    // 家族名集中在 gnss 组件（ADR-0012 §2.2），越界保护也用它自己的取值范围。
    const auto family = ::gnss::familyFromU8(static_cast<uint8_t>(value));
    if (!family.has_value()) {
        return "?";
    }
    return ::gnss::familyName(*family);
}
#endif

/**
 * @brief 把关机阶段枚举翻成文本
 * @param value: 关机阶段数值
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *shutdownStageText(uint32_t value) noexcept
{
    // 追踪事件字段仅承载 board::ShutdownStage 枚举值，转枚举匹配。
    switch (static_cast<board::ShutdownStage>(value)) {
    case board::ShutdownStage::kIdle:
        return "idle";
    case board::ShutdownStage::kAnnounce:
        return "announce";
    case board::ShutdownStage::kSave:
        return "save";
    case board::ShutdownStage::kQuiesce:
        return "quiesce";
    case board::ShutdownStage::kCutPower:
        return "cut_power";
    default:
        return "?";
    }
}

/**
 * @brief 把关机原因枚举翻成文本（覆盖 kNone/kKey/kConsole/kLowBattery/kPhone/kWeb）
 * @param value: 关机原因数值
 * @retval 静态文本，不为空
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
const char *shutdownReasonText(uint32_t value) noexcept
{
    // 追踪事件字段仅承载 board::ShutdownReason 枚举值，转枚举匹配。
    switch (static_cast<board::ShutdownReason>(value)) {
    case board::ShutdownReason::kNone:
        return "none";
    case board::ShutdownReason::kKey:
        return "key";
    case board::ShutdownReason::kConsole:
        return "console";
    case board::ShutdownReason::kLowBattery:
        return "low_battery";
    case board::ShutdownReason::kPhone:
        return "phone";
    case board::ShutdownReason::kWeb:
        return "web";
    default:
        return "?";
    }
}

} // namespace

namespace detail {

using detail::append;

// 不拆（ADR-0013 §2.4）：平铺的 18 分支格式映射，拆开只是把 case 搬到别处。
// NOLINTBEGIN(readability-function-size): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
void appendValue(char *out, size_t cap, size_t &pos, TraceValue value,
                 const fw::TraceEvent &ev) noexcept
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
        append(out, cap, pos, "%s", roleText(ev.a));
        break;
    case TraceValue::kRoleB:
        append(out, cap, pos, "%s", roleText(ev.b));
        break;
    case TraceValue::kReasonA:
        append(out, cap, pos, "%s", reasonText(ev.a));
        break;
    case TraceValue::kReasonB:
        append(out, cap, pos, "%s", reasonText(ev.b));
        break;
    case TraceValue::kResultA:
        append(out, cap, pos, "%s", resultText(ev.a));
        break;
    case TraceValue::kResultB:
        append(out, cap, pos, "%s", resultText(ev.b));
        break;
#if RTK_GNSS_ENABLED
    case TraceValue::kGnssStateA:
        append(out, cap, pos, "%s", ::gnss::stateName(static_cast<::gnss::State>(ev.a)));
        break;
    case TraceValue::kGnssStateB:
        append(out, cap, pos, "%s", ::gnss::stateName(static_cast<::gnss::State>(ev.b)));
        break;
    case TraceValue::kGnssFamilyA:
        append(out, cap, pos, "%s", gnssFamilyText(ev.a));
        break;
    case TraceValue::kGnssModelA:
        append(out, cap, pos, "%s", ::gnss::modelName(static_cast<::gnss::Model>(ev.a)));
        break;
#endif
    case TraceValue::kSppStageA:
        append(out, cap, pos, "%s", sppStageText(ev.a));
        break;
    case TraceValue::kSppCtrlA:
        append(out, cap, pos, "%s", sppCtrlText(ev.a));
        break;
    case TraceValue::kShutdownStageA:
        append(out, cap, pos, "%s", shutdownStageText(ev.a));
        break;
    case TraceValue::kShutdownReasonA:
        append(out, cap, pos, "%s", shutdownReasonText(ev.a));
        break;
    case TraceValue::kShutdownReasonB:
        append(out, cap, pos, "%s", shutdownReasonText(ev.b));
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
// NOLINTEND(readability-function-size)

/** 渲染 {key} 占位符：在字段表里按键名查找并用对应方式输出；查不到则原样保留。 */

} // namespace detail

} // namespace app::debug
