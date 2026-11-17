// GNSS 控制状态机：上电 → 探测（方言×波特率）→ 流动站配置脚本 → 运行 → 静默超时重新探测（ADR-0010 §2.6）。
// 纯逻辑、非阻塞、不做 IO：poll() 返回一个 Action，由 main 侧 gnss 模块执行（写 UART、改波特率、电源复位）；
// 分流器识别出的帧经 on_frame() 送入。时间全部由调用方传入 now_ms（uint32 回绕安全）。
// 线程安全：否（一个实例由一个任务使用）。无堆、无 OS 依赖。
// 冻结接口（ADR-0010 §3，ADR-0012 §3 修订：方言表由驱动接口取代）。变更须先修订 ADR-0012。
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
    kFailed,      // 探测/配置用尽重试，等待 retry_backoff_ms 后从 kPowerUp 重来
    kOutput,      // 基础脚本完成后逐项下发/关闭报文输出（ADR-0012 §2.4）
    kReadback,    // 运行中读回模块配置（ADR-0012 §2.7），结束后回 kRunning
};

/** poll() 要求调用方执行的动作。 */
struct Action {
    enum class Kind : uint8_t {
        kNone = 0,
        kPower,   // 设置电源：on 为 true 上电、false 断电
        kSetBaud, // 主机 UART 改波特率为 baud，并清空 RX（丢弃旧波特率下的残留）
        kWrite,   // 把 bytes 写入 UART（bytes 在下一次 poll/on_frame 前有效）
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
    uint32_t power_off_ms = kTimingPowerOffMs;
    uint32_t boot_ms = kTimingBootMs;
    uint32_t probe_timeout_ms = kTimingProbeTimeoutMs;
    uint8_t probe_rounds = kTimingProbeRounds;
    uint8_t step_retries = kTimingStepRetries;
    uint32_t silence_ms = kTimingSilenceMs;
    uint32_t retry_backoff_ms = kTimingRetryBackoffMs;
};

/** 控制器统计（控制台 gnss status 打印）。 */
struct ControllerStats {
    uint32_t probes_sent = 0;
    uint32_t steps_sent = 0;
    uint32_t step_nacks = 0;
    uint32_t step_timeouts = 0;
    uint32_t reprobes = 0; // kRunning 静默或外部 request_reprobe 引起的重新探测
    uint32_t failures = 0; // 进入 kFailed 的次数
    // 最近一次输出阶段（ADR-0012 §2.4）的结果，位 = OutputMsg。
    uint32_t applied_mask = 0;     // 已确认生效的报文
    uint32_t unsupported_mask = 0; // 型号不支持、已跳过（不是错误）
    uint32_t failed_mask = 0;      // 参数非法或重试用尽
    uint32_t output_runs = 0;      // 输出阶段次数
    uint32_t output_seq = 0;       // 每次输出阶段结束时 +1；main 用它匹配请求与完成
    // 最近一次配置读回（ADR-0012 §2.7）。
    uint32_t readback_lines = 0; // 本轮 match_readback() 命中的行数
    uint32_t readback_seq = 0;   // 每次读回结束（静默超时/总超时/不支持）时 +1
};

/** 探测成功后的结论（供 main 写 NVS 与 DeviceInfo）。 */
struct Detected {
    const Receiver *receiver = nullptr;
    ProbeResult probe{};
    uint32_t baud = 0; // 当前主机 UART 波特率（配置可能已改为 RoverConfig::port_baud）
    uint32_t caps = 0;
};

class Controller {
public:
    constexpr Controller() noexcept = default;
    ~Controller() = default;
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    /**
     * 配置（不启动）。receivers 为候选驱动（通常是 enabled_receivers()），bauds 为探测波特率候选；
     * 两个表与 cfg 必须在实例生命周期内有效。hint 有效时先探测提示的（方言,波特率）。
     * plan 为初始输出计划（ADR-0012 §2.4），调用方可在运行期用 set_output_plan()/request_output() 更新。
     * 返回 false：receivers 或 bauds 为空、含 nullptr。
     */
    [[nodiscard]] bool init(std::span<const Receiver *const> receivers, std::span<const uint32_t> bauds,
                            const RoverConfig &cfg, const OutputPlan &plan, const Hint &hint,
                            const Timing &timing) noexcept;

    /** 从 kOff 开始：先断电 power_off_ms 再上电（电源复位），然后探测。 */
    void start(uint32_t now_ms) noexcept;
    /** 断电并进入 kOff。 */
    void stop(uint32_t now_ms) noexcept;
    /** 立即重新探测（不复位电源），例如控制台 gnss probe。 */
    void request_reprobe(uint32_t now_ms) noexcept;
    /** 已探测到模块时重跑配置脚本，例如修改 RoverConfig 后；否则等同 request_reprobe。 */
    void request_reconfigure(uint32_t now_ms) noexcept;
    /**
     * 运行中修改报文输出（ADR-0012 §2.4）：只重跑输出阶段，不重新探测、不复位模块。
     * 仅 kRunning 接受，其它状态返回 false（调用方应改用 request_reconfigure 或稍后重试）。
     */
    [[nodiscard]] bool request_output(const OutputPlan &plan, uint32_t now_ms) noexcept;
    /**
     * 读回模块当前配置（ADR-0012 §2.7）：发 receiver().build_readback() 的命令，进入 kReadback；
     * 每命中一行 match_readback() 重置 1 s 静默计时，静默到期或总时长到期后回 kRunning 并 readback_seq+1。
     * 仅 kRunning 且驱动支持读回时返回 true；不支持或状态不对返回 false（不改 stats）。
     * 行文本由调用方在 on_frame() 之外自行用 receiver().match_readback() 取得，控制器只计数与计时。
     */
    [[nodiscard]] bool request_readback(uint32_t now_ms) noexcept;
    /** 更新输出计划（探测到型号后 caps 才确定，main 每轮按最新配置重算）；不触发任何动作。 */
    void set_output_plan(const OutputPlan &plan) noexcept
    {
        plan_ = plan;
    }
    /** 当前输出计划（诊断/测试用）。 */
    [[nodiscard]] const OutputPlan &output_plan() const noexcept
    {
        return plan_;
    }

    /** 推进状态机，返回下一个要执行的动作（kNone 表示本轮无事）。调用方应循环 poll 直到 kNone。 */
    [[nodiscard]] Action poll(uint32_t now_ms) noexcept;

    /** 分流器识别出的一帧（任意协议）。用于应答匹配与静默检测。 */
    void on_frame(const demux::Decoder &dec, std::span<const uint8_t> frame, uint32_t now_ms) noexcept;

    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }
    /** 探测成功后有效（receiver 非空）；否则 receiver 为 nullptr。 */
    [[nodiscard]] const Detected &detected() const noexcept
    {
        return detected_;
    }
    /**
     * 当前驱动（ADR-0012 §2.2）：探测成功后为对应驱动，未探测到（或探测中被清空）时为 null_receiver()。
     * 永不返回 nullptr，调用方无需判空。
     */
    [[nodiscard]] const Receiver &receiver() const noexcept;
    /**
     * 最近一次探测发送所用的驱动（kProbeSend 追踪日志用）；未在探测中时为 null_receiver()。
     * 与 detected() 不同：探测失败也会保留最后一个候选项，因此可用来区分“发的是哪家的查询”。
     */
    [[nodiscard]] const Receiver &probe_receiver() const noexcept;

    [[nodiscard]] const ControllerStats &stats() const noexcept
    {
        return stats_;
    }
    /** 当前主机 UART 应使用的波特率（每次 kSetBaud 后更新）。 */
    [[nodiscard]] uint32_t host_baud() const noexcept
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

/** 状态名（控制台用）。 */
[[nodiscard]] const char *state_name(State state) noexcept;

} // namespace gnss
