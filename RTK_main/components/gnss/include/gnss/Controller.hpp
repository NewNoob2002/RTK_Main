// GNSS 控制状态机：上电 → 探测（方言×波特率）→ 流动站配置脚本 → 运行 → 静默超时重新探测（ADR-0010 §2.6）；纯逻辑不做 IO，poll() 只返回 Action 由 main 侧执行。
// 线程安全：否（一个实例由一个任务使用；帧经 onFrame() 送入）。ISR 安全：否。
// 堆与阻塞：不使用堆、不阻塞、无 OS 依赖；时间全部由调用方传入 nowMs（uint32 回绕安全）。冻结接口（ADR-0010 §3，ADR-0012 §3 修订：方言表由驱动接口取代）实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"
#include "gnss/Receiver.hpp"
#include "gnss/Types.hpp"

namespace gnss {

/** 控制器状态。顺序固定（控制台/追踪按数值打印）。 */
enum class State : uint8_t {
    kOff = 0,     // 未启动或 stop() 后
    kPowerUp,     // 已请求上电，等待模块启动时间
    kProbing,     // 轮询方言×波特率发送版本查询
    kConfiguring, // 执行流动站配置脚本
    kRunning,     // 配置完成，正常接收 NMEA
    kFailed,      // 探测/配置用尽重试，等待 retryBackoffMs 后从 kPowerUp 重来
    kOutput,      // 基础脚本完成后逐项下发/关闭报文输出（ADR-0012 §2.4）
    kReadback,    // 运行中读回模块配置（ADR-0012 §2.7），结束后回 kRunning
};

/** poll() 要求调用方执行的动作。 */
struct Action {
    enum class Kind : uint8_t {
        kNone = 0,
        kPower,   // 设置电源：on 为 true 上电、false 断电
        kSetBaud, // 主机 UART 改波特率为 baud，并清空 RX（丢弃旧波特率下的残留）
        kWrite,   // 把 bytes 写入 UART（bytes 在下一次 poll/onFrame 前有效）
    };
    Kind kind = Kind::kNone;
    bool on = false;
    uint32_t baud = 0;
    std::span<const uint8_t> bytes{};
};

/** 启动提示（来自 NVS 上次探测结果）；family=kUnknown 表示无提示。 */
struct Hint {
    Family family = Family::kUnknown;
    Model model = Model::kUnknown;
    uint32_t baud = 0;
};

/** Timing 默认值（ADR-0010 §2.6）；单独命名以便控制台/测试引用同一组数字。 */
inline constexpr uint32_t kTimingPowerOffMs = 200;
inline constexpr uint32_t kTimingBootMs = 1500;
inline constexpr uint32_t kTimingProbeTimeoutMs = 400;
inline constexpr uint8_t kTimingProbeRounds = 3;
inline constexpr uint8_t kTimingStepRetries = 2;
inline constexpr uint32_t kTimingSilenceMs = 5000;
inline constexpr uint32_t kTimingRetryBackoffMs = 10000;

/** 时序参数（默认值见 ADR-0010 §2.6；主机测试可缩短）。 */
struct Timing {
    uint32_t powerOffMs = kTimingPowerOffMs;
    uint32_t bootMs = kTimingBootMs;
    uint32_t probeTimeoutMs = kTimingProbeTimeoutMs;
    uint8_t probeRounds = kTimingProbeRounds;
    uint8_t stepRetries = kTimingStepRetries;
    uint32_t silenceMs = kTimingSilenceMs;
    uint32_t retryBackoffMs = kTimingRetryBackoffMs;
};

/** 控制器统计（控制台 gnss status 打印）。 */
struct ControllerStats {
    uint32_t probesSent = 0;
    uint32_t stepsSent = 0;
    uint32_t stepNacks = 0;
    uint32_t stepTimeouts = 0;
    uint32_t reprobes = 0; // kRunning 静默或外部 requestReprobe 引起的重新探测
    uint32_t failures = 0; // 进入 kFailed 的次数
    // 最近一次输出阶段（ADR-0012 §2.4）的结果，位 = OutputMsg。
    uint32_t appliedMask = 0;     // 已确认生效的报文
    uint32_t unsupportedMask = 0; // 型号不支持、已跳过（不是错误）
    uint32_t failedMask = 0;      // 参数非法或重试用尽
    uint32_t outputRuns = 0;      // 输出阶段次数
    uint32_t outputSeq = 0;       // 每次输出阶段结束时 +1；main 用它匹配请求与完成
    // 最近一次配置读回（ADR-0012 §2.7）。
    uint32_t readbackLines = 0; // 本轮 matchReadback() 命中的行数
    uint32_t readbackSeq = 0;   // 每次读回结束（静默超时/总超时/不支持）时 +1
};

/** 探测成功后的结论（供 main 写 NVS 与 DeviceInfo）。 */
struct Detected {
    const Receiver *receiver = nullptr;
    ProbeResult probe{};
    uint32_t baud = 0; // 当前主机 UART 波特率（配置可能已改为 RoverConfig::portBaud）
    uint32_t caps = 0;
};

class Controller {
public:
    constexpr Controller() noexcept = default;
    ~Controller() = default;
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    /**
     * @brief  配置控制器（不启动）
     * @param  receivers: 候选驱动表（通常是 enabledReceivers()），须在实例生命周期内有效
     * @param  bauds: 探测波特率候选表，须在实例生命周期内有效
     * @param  cfg: 流动站配置，须在实例生命周期内有效
     * @param  plan: 初始输出计划（ADR-0012 §2.4），运行期可用 setOutputPlan()/requestOutput() 更新
     * @param  hint: 上次探测结果提示；family=kUnknown 表示无提示，有效时先探测提示的（方言,波特率）
     * @param  timing: 时序参数（主机测试可缩短）
     * @retval true: 配置完成，状态为 kOff
     * @retval false: receivers/bauds 为空、含 nullptr，或 bauds 含 0；实例状态未改变
     * @note   非线程安全；只在单任务上下文调用，不阻塞
     */
    [[nodiscard]] bool init(std::span<const Receiver *const> receivers,
                            std::span<const uint32_t> bauds, const RoverConfig &cfg,
                            const OutputPlan &plan, const Hint &hint,
                            const Timing &timing) noexcept;

    /**
     * @brief  从 kOff 开始：先断电 powerOffMs 再上电（电源复位），然后探测
     * @param  nowMs: 当前时间
     * @note   非线程安全；状态被无条件改写到 kPowerUp
     */
    void start(uint32_t nowMs) noexcept;
    /**
     * @brief  断电并进入 kOff
     * @param  nowMs: 当前时间
     * @note   下一轮 poll() 先返回一次断电动作
     */
    void stop(uint32_t nowMs) noexcept;
    /**
     * @brief  立即重新探测（不复位电源），例如控制台 gnss probe
     * @param  nowMs: 当前时间
     * @note   非 kOff 时 reprobes 计数加一
     */
    void requestReprobe(uint32_t nowMs) noexcept;
    /**
     * @brief  已探测到模块时重跑配置脚本，例如修改 RoverConfig 后；否则等同 requestReprobe()
     * @param  nowMs: 当前时间
     */
    void requestReconfigure(uint32_t nowMs) noexcept;
    /**
     * @brief  运行中修改报文输出（ADR-0012 §2.4）：只重跑输出阶段，不重新探测、不复位模块
     * @param  plan: 新输出计划，成功时立即生效
     * @param  nowMs: 当前时间（本实现未使用，保留给状态机计时扩展）
     * @retval true: 已进入 kOutput，本轮结果掩码被清零
     * @retval false: 非 kRunning 或尚未探测到模块；状态与计划均未改变
     * @note   非线程安全；调用方在 false 时应改用 requestReconfigure() 或稍后重试
     */
    [[nodiscard]] bool requestOutput(const OutputPlan &plan, uint32_t nowMs) noexcept;
    /**
     * @brief  读回模块当前配置（ADR-0012 §2.7）：发 receiver().buildReadback() 的命令，进入 kReadback
     * @param  nowMs: 当前时间，用于起算静默计时与总时长
     * @retval true: 已进入 kReadback，即 readbackLines 清零、总时长开始计时
     * @retval false: 非 kRunning、尚未探测到模块，或驱动不支持读回；stats 不变
     * @note   每命中一行 matchReadback() 重置 1 s 静默计时，静默或总时长到期后回 kRunning 并
     *         readbackSeq+1；行文本由调用方在 onFrame() 之外自行用 receiver().matchReadback() 取得，
     *         控制器只计数与计时。非线程安全，不阻塞。
     */
    [[nodiscard]] bool requestReadback(uint32_t nowMs) noexcept;
    /**
     * @brief  更新输出计划，不触发任何动作
     * @param  plan: 新计划（探测到型号后 caps 才确定，main 每轮按最新配置重算）
     */
    void setOutputPlan(const OutputPlan &plan) noexcept
    {
        plan_ = plan;
    }
    /**
     * @brief  当前输出计划（诊断/测试用）
     * @retval 控制器持有的计划引用，生命周期同本实例
     */
    [[nodiscard]] const OutputPlan &outputPlan() const noexcept
    {
        return plan_;
    }

    /**
     * @brief  推进状态机，返回下一个要执行的动作
     * @param  nowMs: 当前时间（uint32 回绕安全）
     * @retval 下一个动作；kind=kNone 表示本轮无事，调用方应循环 poll 直到 kNone
     * @note   单次调用内最多推进有限次状态迁移（内部有轮次上限），防止死循环
     */
    [[nodiscard]] Action poll(uint32_t nowMs) noexcept;

    /**
     * @brief  分流器识别出的一帧（任意协议），交给当前阶段处理
     * @param  dec: 该帧的解码器（协议名）
     * @param  frame: 整帧字节（含帧头，不含分帧器剥离的同步字）
     * @param  nowMs: 当前时间，只用于判“模块还在说话”
     * @note   探测阶段试匹配型号，等待应答时判 ACK/NACK；其余帧只刷新活动时间戳
     *         （kReadback 窗口例外：只有命中 matchReadback() 的行才刷新）
     */
    void onFrame(const demux::Decoder &dec, std::span<const uint8_t> frame,
                 uint32_t nowMs) noexcept;

    /**
     * @brief  当前状态机状态
     * @retval 状态值；kOff 表示未启动或已 stop()
     */
    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }
    /**
     * @brief  最近一次探测结论
     * @retval 探测结果引用；receiver 为 nullptr 表示尚未探测成功
     */
    [[nodiscard]] const Detected &detected() const noexcept
    {
        return detected_;
    }
    /**
     * @brief  当前驱动（ADR-0012 §2.2）
     * @retval 探测成功后为对应驱动；未探测到（或探测中被清空）时为 nullReceiver()，永不返回 nullptr
     */
    [[nodiscard]] const Receiver &receiver() const noexcept;
    /**
     * @brief  最近一次探测发送所用的驱动（追踪日志用）
     * @retval 探测中为当前候选项；未在探测中时为 nullReceiver()
     * @note   与 detected() 不同：探测失败也会保留最后一个候选项，因此可用来区分“发的是哪家的查询”
     */
    [[nodiscard]] const Receiver &probeReceiver() const noexcept;

    /**
     * @brief  控制器统计值（控制台 gnss status 打印）
     * @retval 统计引用，随控制器更新
     */
    [[nodiscard]] const ControllerStats &stats() const noexcept
    {
        return stats_;
    }
    /**
     * @brief  当前主机 UART 应使用的波特率
     * @retval 波特率值（每次 kSetBaud 动作后更新）
     */
    [[nodiscard]] uint32_t hostBaud() const noexcept
    {
        return host_baud_;
    }

private:
    std::span<const Receiver *const> receivers_{};
    std::span<const uint32_t> bauds_{};
    const RoverConfig *cfg_ = nullptr;
    OutputPlan plan_{};
    Hint hint_{};
    Timing timing_{};

    State state_ = State::kOff;
    uint8_t phase_ = 0;          // 状态内子阶段（实现定义）
    uint32_t deadline_ms_ = 0;   // 当前等待的截止时间
    uint32_t last_frame_ms_ = 0; // 最近一帧到达时间
    uint32_t host_baud_ = 0;
    size_t probe_index_ = 0; // 探测组合游标（含 hint 优先项）
    uint8_t probe_round_ = 0;
    size_t step_index_ = 0; // 配置脚本游标
    uint8_t step_tries_ = 0;
    bool waiting_reply_ = false;
    bool reply_ok_ = false;
    bool reply_nack_ = false;
    bool probe_hit_ = false;
    size_t output_index_ = 0; // 输出阶段游标（OutputMsg 数值）
    // 只有运行时改配置才需要给"关闭"的语句下发 UNLOG；开机首轮跳过它们，与基线脚本逐字节一致。
    bool output_rerun_ = false;
    ConfigStep step_{};   // 当前配置步骤（bytes 来源）
    CmdBuf probe_cmd_{};  // 当前探测命令（bytes 来源）
    CmdBuf output_cmd_{}; // 当前输出命令（bytes 来源）
    const Receiver *probe_receiver_ = nullptr;
    Detected detected_{};
    ControllerStats stats_{};

    friend struct ControllerImpl; // 实现细节在 src/Controller.cpp
};

/**
 * @brief  状态名（控制台/日志用）
 * @param  state: 控制器状态
 * @retval 静态字符串（"off"/"power-up"/"probing"/"configuring"/"running"/"failed"/"output"/"readback"）；
 *         未知值返回 "unknown"
 */
[[nodiscard]] const char *stateName(State state) noexcept;

} // namespace gnss
