// GNSS 模块跨模块共享类型：状态快照、命令动词、追踪事件码、RTCM 输入管道容量（ADR-0010 §2.7）。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "fw/trace.hpp"
#include "gnss/controller.hpp"
#include "gnss/nmea_parser.hpp"
#include "gnss/types.hpp"

namespace app {

/** RTCM 输入管道容量（rtcm 模块 kLink 写 → gnss 模块 kIo 读）；≥ 3 条最大 RTCM 帧。 */
inline constexpr size_t kRtcmPipeBytes = 4096;

/** GNSS 模块命令动词（CommandBus，控制台经 rtos::Executor::call 投递到 kIo）。 */
enum class GnssVerb : uint8_t {
    kProbe = 0,   // 重新探测（不复位电源）；arg 无
    kReset = 1,   // 电源复位后重新探测
    kSetRate = 2, // arg0 = 频率 Hz（1/2/5/10/20），写 NVS 并重配
    kSend = 3,    // 原样发送调试命令：args 为文本（不含 \r\n，≤ fw::kCommandArgBytes），仅 kRunning/kFailed 允许
};

/** kGnss 追踪事件码；控制台文本表 main/modules/debug/trace_text.cpp 按此解释。 */
enum class GnssTrace : uint8_t {
    kState = 0,       // a=新 gnss::State，b=旧 State
    kProbeSend = 1,   // a=gnss::Family，b=波特率
    kProbeHit = 2,    // a=gnss::Model，b=波特率
    kStepSend = 3,    // a=步骤序号，b=重试次数
    kStepAck = 4,     // a=步骤序号，b=0
    kStepNack = 5,    // a=步骤序号，b=0
    kStepTimeout = 6, // a=步骤序号，b=重试次数
    kSilence = 7,     // a=静默毫秒，b=0
    kPipeDrop = 8,    // a=被丢帧长度，b=管道剩余空间
    kBaud = 9,        // a=新波特率，b=旧波特率
};

/** 记录 kGnss 事件；session 固定 0。线程：任意任务，非 ISR。 */
inline void trace_gnss(GnssTrace code, uint32_t a, uint32_t b) noexcept
{
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(code), 0U, a, b);
}

/** RTCM 输入管道统计。 */
struct RtcmPipeStats {
    uint32_t pushed_frames = 0; // rtcm 模块成功写入的帧
    uint32_t pushed_bytes = 0;
    uint32_t dropped_frames = 0; // 空间不足整帧丢弃
    uint32_t dropped_bytes = 0;
    uint32_t written_bytes = 0; // gnss 模块已写入 UART 的字节
};

/** GNSS 状态快照（data::DataCenter::gnss_status）。 */
struct GnssStatus {
    bool enabled = false; // RTK_GNSS_ENABLE 且 UART 初始化成功
    gnss::State state = gnss::State::kOff;
    gnss::Family family = gnss::Family::kUnknown;
    gnss::Model model = gnss::Model::kUnknown;
    std::array<char, gnss::kVersionLen> fw_version{};
    uint32_t host_baud = 0;
    uint32_t caps = 0;
    gnss::ControllerStats ctrl{};
    gnss::NmeaStats nmea{};
    uint32_t uart_rx_bytes = 0;
    uint32_t uart_tx_bytes = 0;
    uint32_t demux_unknown = 0;
    uint32_t demux_resyncs = 0;
    RtcmPipeStats pipe{};
};

} // namespace app
