// GNSS 模块跨模块共享类型：状态快照、命令动词、追踪事件码、RTCM 输入管道容量（ADR-0010 §2.7）。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/services/GnssService.hpp" // kGnssMsgCount：App 语句类数（避免此头依赖 main/data）
#include "fw/Trace.hpp"
#include "gnss/Controller.hpp"
#include "gnss/NmeaParser.hpp"
#include "gnss/Types.hpp"

namespace app {

/** RTCM 输入管道容量（rtcm 模块 kLink 写 → gnss 模块 kIo 读）；≥ 3 条最大 RTCM 帧。 */
inline constexpr size_t kRtcmPipeBytes = 4096;

/** GNSS 模块命令动词（CommandBus，控制台经 rtos::Executor::call 投递到 kIo）。 */
enum class GnssVerb : uint8_t {
    kProbe = 0,   // 重新探测（不复位电源）；arg 无
    kReset = 1,   // 电源复位后重新探测
    kSetRate = 2, // arg0 = 频率 Hz（1/2/5/10/20），写 NVS 并重配
    kSend =
        3, // 原样发送调试命令：args 为文本（不含 \r\n，≤ fw::kCommandArgBytes），仅 kRunning/kFailed 允许
};

/** kGnss 追踪事件码；控制台文本表 main/modules/debug/TraceText.cpp 按此解释。 */
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
inline void traceGnss(GnssTrace code, uint32_t a, uint32_t b) noexcept
{
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(code), 0U, a, b);
}

/** RTCM 输入管道统计。 */
struct RtcmPipeStats {
    uint32_t pushedFrames = 0; // rtcm 模块成功写入的帧
    uint32_t pushedBytes = 0;
    uint32_t droppedFrames = 0; // 空间不足整帧丢弃
    uint32_t droppedBytes = 0;
    uint32_t writtenBytes = 0; // gnss 模块已写入 UART 的字节
};

/** 最近一次输出/频率变更的落地结果（ADR-0012 §2.4；控制台据此决定 OK/ERR 与 NVS 回滚）。 */
enum class GnssApply : uint8_t {
    kNone = 0,     // 没有进行中的变更
    kOk = 1,       // 全部支持的语句已 ACK，NVS 已写入
    kFailed = 2,   // 有语句失败，NVS 保持旧值
    kRestored = 3, // 失败后已回滚到旧配置
};

/** 读回文本缓冲容量：UM980 `CONFIG` 约 20–30 行 × ≤ 60 B，超出部分截断并置 truncated。 */
inline constexpr size_t kGnssReadbackBytes = 1536U;

/**
 * 最近一次配置读回的结论（ADR-0012 §2.7，data::DataCenter::gnssReadback）。
 * 文本本体超出 Slot 上限，放在 DataCenter::gnssReadbackText（交接缓冲，见该成员注释）；本结构只带长度与计数。
 */
struct GnssReadback {
    uint32_t seq = 0;       // 每次读回结束 +1（与 ControllerStats::readbackSeq 同步）
    uint16_t lines = 0;     // 命中行数（含因容量不足被丢弃的行）
    uint16_t len = 0;       // gnssReadbackText 有效字节；文本为若干 "...\n" 行，不以 '\0' 结尾
    bool supported = false; // 当前驱动支持读回；false 时其余字段无意义
    bool truncated = false; // 有行因容量不足被丢弃
    bool busy = false;      // 请求被拒：控制器不在 kRunning（未读回，lines/len 为 0）
};

/** GNSS 状态快照（data::DataCenter::gnssStatus）。 */
struct GnssStatus {
    bool enabled = false; // RTK_GNSS_ENABLE 且 UART 初始化成功
    gnss::State state = gnss::State::kOff;
    gnss::Family family = gnss::Family::kUnknown;
    gnss::Model model = gnss::Model::kUnknown;
    std::array<char, gnss::kVersionLen> fwVersion{};
    uint32_t hostBaud = 0;
    uint32_t caps = 0;
    gnss::ControllerStats ctrl{};
    gnss::NmeaStats nmea{};
    uint32_t uartRxBytes = 0;
    uint32_t uartTxBytes = 0;
    uint32_t demuxUnknown = 0;
    uint32_t demuxResyncs = 0;
    RtcmPipeStats pipe{};
    // 输出计划状态（ADR-0012 §2.4/§2.5）：每次输出阶段/频率重配递增 outputSeq，供控制台等待落地。
    uint32_t outputSeq = 0;
    uint32_t appliedMask = 0;     // 本轮所有输出项 ACK 的位
    uint32_t unsupportedMask = 0; // 接收机不支持（驱动返回 kNotSupported）的位
    uint32_t failedMask = 0;      // 下发失败/ACK 超时的位
    uint32_t supportedMask = 0;   // 接收机当前支持的 App 语句位（App 索引，供 GnssService 拒订阅）
    GnssApply lastApply = GnssApply::kNone;
    // 每次 gnss out/rate 变更有了结论（写入 NVS / 回滚 / 写入失败）时 +1；控制台等它变化后再读 lastApply，
    // 避免 outputSeq 已前进但两阶段提交尚未判定时读到上一次的 lastApply。
    uint32_t applySeq = 0;
    // 当前输出计划里每类 App 语句下发给接收机的周期（ms，0 = 不下发）；与 gnss/out 的 App 推送间隔不同，
    // 默认 GGA/RMC/GSA/GST 跟频率、SATSINFOA 5 s。长度随 kGnssMsgCount 变化，不写死。
    std::array<uint32_t, core::services::kGnssMsgCount> rxPeriodMs{};
};

} // namespace app
