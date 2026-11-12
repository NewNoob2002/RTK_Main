// RTCM 模块跨模块共享类型与命令动词定义（拆自原 app_data.hpp 与 app_ids.hpp）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "app_config.hpp"
#include "core/trace_codes.hpp"
#include "fw/trace.hpp"

namespace app {

/** RTCM 模块命令动词。 */
enum class RtcmVerb : uint8_t { kReset };

/** kRtcm 追踪事件码（ADR-0009）；控制台文本表 main/modules/debug/trace_text.cpp 按此顺序解释。 */
enum class RtcmTrace : uint8_t {
    kFrame = 0,   // session=会话，a=消息号（12 bit），b=线路帧长度
    kReject = 1,  // session=会话，a=拒绝原因（RtcmReject），b=线路帧长度
    kResync = 2,  // session=会话，a=新增重同步次数，b=0
    kUnknown = 3, // session=会话，a=未知字节数，b=0
};

/** RTCM 帧被丢弃的原因。 */
enum class RtcmReject : uint8_t {
    kRole = 0,     // 会话不是手机角色，不转发 RTCM
    kCapacity = 1, // 本轮批次容量越界（分流器交付队列被破坏）
};

// 事件码是 uint8_t 负载；会话号按 core::kTraceSessionMask 截到低 16 位。线程安全：只写 trace 环。
inline void trace_rtcm(RtcmTrace code, uint32_t session, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kRtcm, static_cast<uint8_t>(code), static_cast<uint16_t>(session & core::kTraceSessionMask),
              a, b);
}

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t verb(RtcmVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** 分流器统计快照，只保留要展示的三项计数。 */
struct DemuxStats {
    uint32_t unknown_bytes = 0;
    uint32_t product_rejects = 0;
    uint32_t rtcm_rejects = 0;
};

// 仅 kLink 内部传递完整 RTCM 批次。view 指向 bt_link 的静态存储，
// 在 rtcm.poll 消费后、下一轮 wait 前才复用；其他任务只读取 kRtcmStats。
// 每会话遗留数据最多 1029 B，新输入每轮最多 256 B，故 2×1029+256 B 足够。
inline constexpr size_t kRtcmBatchBytes = kMaxSessions * 1029U + 256U;
/** 一轮交付给 RTCM 模块的批次视图；wire 指向 bt_link 的静态存储，仅 kLink 内部使用。 */
struct RtcmBatch {
    std::span<const uint8_t> wire{};
    uint32_t rejected = 0;
};

} // namespace app
