// GNSS 控制状态机：上电 → 探测（方言×波特率）→ 流动站配置脚本 → 运行 → 静默超时重新探测（ADR-0010 §2.6）。
// 纯逻辑、非阻塞、不做 IO：poll() 返回一个 Action，由 main 侧 gnss 模块执行（写 UART、改波特率、电源复位）；
// 分流器识别出的帧经 on_frame() 送入。时间全部由调用方传入 now_ms（uint32 回绕安全）。
// 线程安全：否（一个实例由一个任务使用）。无堆、无 OS 依赖。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoder.hpp"
#include "gnss/dialect.hpp"
#include "gnss/types.hpp"

namespace gnss {

/** 控制器状态。顺序固定（控制台/追踪按数值打印）。 */
enum class State : uint8_t {
    kOff = 0,     // 未启动或 stop() 后
    kPowerUp,     // 已请求上电，等待模块启动时间
    kProbing,     // 轮询方言×波特率发送版本查询
    kConfiguring, // 执行流动站配置脚本
    kRunning,     // 配置完成，正常接收 NMEA
    kFailed,      // 探测/配置用尽重试，等待 retry_backoff_ms 后从 kPowerUp 重来
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
};

/** 探测成功后的结论（供 main 写 NVS 与 DeviceInfo）。 */
struct Detected {
    const Dialect *dialect = nullptr;
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
     * 配置（不启动）。dialects 为候选方言（通常是 enabled_dialects()），bauds 为探测波特率候选；
     * 两个表与 cfg 必须在实例生命周期内有效。hint 有效时先探测提示的（方言,波特率）。
     * 返回 false：dialects 或 bauds 为空、含 nullptr。
     */
    [[nodiscard]] bool init(std::span<const Dialect *const> dialects, std::span<const uint32_t> bauds,
                            const RoverConfig &cfg, const Hint &hint, const Timing &timing) noexcept;

    /** 从 kOff 开始：先断电 power_off_ms 再上电（电源复位），然后探测。 */
    void start(uint32_t now_ms) noexcept;
    /** 断电并进入 kOff。 */
    void stop(uint32_t now_ms) noexcept;
    /** 立即重新探测（不复位电源），例如控制台 gnss probe。 */
    void request_reprobe(uint32_t now_ms) noexcept;
    /** 已探测到模块时重跑配置脚本，例如修改 RoverConfig 后；否则等同 request_reprobe。 */
    void request_reconfigure(uint32_t now_ms) noexcept;

    /** 推进状态机，返回下一个要执行的动作（kNone 表示本轮无事）。调用方应循环 poll 直到 kNone。 */
    [[nodiscard]] Action poll(uint32_t now_ms) noexcept;

    /** 分流器识别出的一帧（任意协议）。用于应答匹配与静默检测。 */
    void on_frame(const demux::Decoder &dec, std::span<const uint8_t> frame, uint32_t now_ms) noexcept;

    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }
    /** 探测成功后有效（dialect 非空）；否则 dialect 为 nullptr。 */
    [[nodiscard]] const Detected &detected() const noexcept
    {
        return detected_;
    }
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
    std::span<const Dialect *const> dialects_{};
    std::span<const uint32_t> bauds_{};
    const RoverConfig *cfg_ = nullptr;
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
    ConfigStep step_{};  // 当前配置步骤（bytes 来源）
    CmdBuf probe_cmd_{}; // 当前探测命令（bytes 来源）
    const Dialect *probe_dialect_ = nullptr;
    Detected detected_{};
    ControllerStats stats_{};

    friend struct ControllerImpl; // 实现细节在 src/controller.cpp
};

/** 状态名（控制台用）。 */
[[nodiscard]] const char *state_name(State state) noexcept;

} // namespace gnss
