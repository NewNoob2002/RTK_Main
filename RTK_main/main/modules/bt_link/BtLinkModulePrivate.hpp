// 链路模块内部契约：只在 modules/bt_link/*.cpp 之间共享的常量。
// 仅 kLink 任务或启动期使用；全部 inline constexpr 或纯函数，不分配堆、非 ISR。

#pragma once

#include <cstddef>
#include <cstdint>

#include "AppConfig.hpp"

namespace app::detail {

/** RX 调度每轮的时间预算（毫秒），保持 512 B 读缓冲下的既有节奏。 */
inline constexpr uint32_t kRoundBudgetMs = 20;
#if RTK_GNSS_ENABLED
/** GNSS 定位超过该时长未更新即视为陈旧（毫秒）。 */
inline constexpr uint32_t kGnssFixStaleMs = 3000U;
/** 允许的 GNSS 时钟抖动（毫秒）。 */
inline constexpr uint32_t kGnssClockJitterMs = 10U;
/** 单轮最多消费的 GNSS 流记录数。 */
inline constexpr size_t kMaxGnssStreamRecordsPerPoll = 16U;
/** 单轮最多冲刷的 GNSS 流记录数。 */
inline constexpr size_t kMaxGnssFlushRecords = 64U;
#endif
/** setAlarm 命令的参数字节数（level + override）。 */
inline constexpr size_t kAlarmArgBytes = 2;
/** RTCM3 线路帧最小字节数。
 * @note RTCM3 线路帧：1 B 前导 + 2 B 长度，载荷首 12 bit 为消息号，尾 3 B 为 CRC24Q。
 */
inline constexpr size_t kMinRtcmMessageBytes = 8;
/** RTCM3 载荷（消息号）在帧内的偏移。 */
inline constexpr size_t kRtcmPayloadOffset = 3;
/** 消息号在载荷前 2 字节中的移位位数（首 12 bit）。 */
inline constexpr uint32_t kRtcmMsgShiftBits = 4;
/** 会话句柄显示掩码。 */
inline constexpr uint32_t kHandleDisplayMask = 0xFFFFU;

} // namespace app::detail
