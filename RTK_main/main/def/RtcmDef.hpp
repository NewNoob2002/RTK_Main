// RTCM 模块跨模块共享类型与命令动词定义（拆自原 AppData.hpp 与 app_ids.hpp）。
// 只含类型与枚举定义，无运行期状态；由 rtcm 模块与对应命令共用，线程/ISR 安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "AppConfig.hpp"
#include "core/TraceCodes.hpp"
#include "fw/Trace.hpp"

namespace app {

/** RTCM 模块命令动词。 */
enum class RtcmVerb : uint8_t { kReset };

/** kRtcm 追踪事件码（ADR-0009）；控制台文本表 main/modules/debug/TraceText.cpp 按此顺序解释。 */
enum class RtcmTrace : uint8_t {
    kFrame = 0,   // session=会话，a=消息号（12 bit），b=线路帧长度
    kReject = 1,  // session=会话，a=拒绝原因（RtcmReject），b=线路帧长度
    kResync = 2,  // session=会话，a=新增重同步次数，b=0
    kUnknown = 3, // session=会话，a=未知字节数，b=0
};

/** RTCM 帧被丢弃的原因。 */
enum class RtcmReject : uint8_t {
    kRole = 0,     // 会话不是手机角色，不转发 RTCM
    kCapacity = 1, // 预留兼容值（改为直推管道后不再使用批次缓存）
};

/**
 * @brief 记录一条 RTCM 子事件
 * @param code: 子事件码
 * @param session: 会话号，按追踪字段宽度截取
 * @param a: 事件第一个数值字段
 * @param b: 事件第二个数值字段
 * @note 仅 kLink 任务调用，不分配、不格式化；非 ISR。
 * @note 事件码是 uint8_t 负载；会话号按 core::kTraceSessionMask 截到低 16 位。线程安全：只写 trace 环。
 */
inline void traceRtcm(RtcmTrace code, uint32_t session, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kRtcm, static_cast<uint8_t>(code),
              static_cast<uint16_t>(session & core::kTraceSessionMask), a, b);
}

/**
 * @brief 读取命令动词的数值编号
 * @param value: 待转换或写入的值
 * @retval 与枚举底层值相同的 uint8_t 动词
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t verb(RtcmVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/**
 * 分流器统计快照，只保留要展示的三项计数。
 * *Rejects 是该解码器“放弃候选”的累计次数：判帧拒绝、超长、契约违规、abort() 丢半帧都计入
 * （ADR-0004 §2），即“以该解码器起头但最终没成帧”的次数，不只是校验失败；具体原因看 TRACE 的 reject 行。
 */
struct DemuxStats {
    uint32_t unknownBytes = 0;
    uint32_t productRejects = 0;
    uint32_t rtcmRejects = 0;
};

/** bt_link 直推路径维护的 RTCM 累计快照（纯数据；RtcmModule 仅读取并管理 resetEpoch）。 */
struct RtcmLinkSnapshot {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint32_t crc32 = 0;
    uint16_t lastMsg = 0;
    uint32_t rejectedRole = 0;
    uint32_t pipeAcceptedFrames = 0;
    uint32_t pipeAcceptedBytes = 0;
    uint32_t pipeDroppedFrames = 0;
    uint32_t pipeDroppedBytes = 0;
    uint32_t resetEpoch = 0;
};

/** kIo 侧 RTCM 管道消费与 UART 写回状态快照（spinlock 保护跨任务读）。 */
struct RtcmPipeIoStats {
    uint32_t pendingBytes = 0;
    uint32_t uartWriteErrors = 0;
    uint32_t discardedBytes = 0;
    bool paused = false;
};

} // namespace app
