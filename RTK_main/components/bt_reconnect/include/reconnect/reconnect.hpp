// 通用 SPP 客户端重连策略状态机：纯 C++20，无 IDF/FreeRTOS/rtk_* 依赖（ExternalSpeak 报警器以后复用）。
// 只产生“动作”，由调用方执行（esp_spp_start_discovery / esp_spp_connect / 定时），因此可在 host 上全覆盖测试。
// 线程模型：单线程；所有方法只能由同一任务调用。时间单位 ms，u32 回绕由无符号差值处理。
// 冻结接口（ADR-0007 §5）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace reconnect {

inline constexpr size_t kAddrLen = 6;
/** 经典蓝牙对端地址（6 字节，大端线序，直接来自 rem_bda）。 */
using Addr = std::array<uint8_t, kAddrLen>;

inline constexpr uint32_t kDefFastWindowMs = 30000U;
inline constexpr uint32_t kDefFastRetryMs = 1000U;
inline constexpr uint32_t kDefBackoffMinMs = 1000U;
inline constexpr uint32_t kDefBackoffMaxMs = 30000U;
inline constexpr uint8_t kDefJitterPct = 20U;
inline constexpr uint8_t kMaxJitterPct = 50U;
inline constexpr uint32_t kDefStableOnlineMs = 10000U;
inline constexpr uint8_t kDefSdpAfterFailures = 2U;
inline constexpr uint32_t kDefAttemptTimeoutMs = 8000U;
inline constexpr uint32_t kDefaultSeed = 0x2545F491U;

/** 状态机的当前状态；转移规则见 Reconnector 类注释。 */
enum class State : uint8_t {
    kIdle = 0,    // 未设置目标或尚未 start
    kDiscovering, // 已发出 SDP，等待 on_sdp_result
    kConnecting,  // 已发出 connect，等待 on_open / on_connect_failed
    kConnected,   // 会话在线
    kBackoff,     // 等待下一次尝试
    kStopped,     // 调用方显式 stop，直到再次 start
};

/** 调用方需要执行的动作类型；状态机自身不调用蓝牙 API。 */
enum class ActionType : uint8_t {
    kNone = 0,
    kStartSdp, // 调用方发起 SDP 查询目标
    kConnect,  // 调用方以 scn 发起连接
};

/** 一次状态转移输出的动作；type 为 kNone 时 scn 无意义。 */
struct Action {
    ActionType type = ActionType::kNone;
    uint8_t scn = 0; // kConnect 时有效
};

/** 重连参数；configure() 会校验范围，非法值整组拒绝。 */
struct Config {
    uint32_t fast_window_ms = kDefFastWindowMs;         // 从 start 或掉线起算的快速重试窗口
    uint32_t fast_retry_ms = kDefFastRetryMs;           // 快速窗口内的固定间隔
    uint32_t backoff_min_ms = kDefBackoffMinMs;         // 窗口外退避起点
    uint32_t backoff_max_ms = kDefBackoffMaxMs;         // 退避上限（翻倍直到上限）
    uint8_t jitter_pct = kDefJitterPct;                 // 抖动 ±jitter_pct%，0 = 不抖动；> kMaxJitterPct 视为非法
    uint32_t stable_online_ms = kDefStableOnlineMs;     // 在线满该时长才复位退避
    uint8_t sdp_after_failures = kDefSdpAfterFailures;  // 有缓存 SCN 时，连续 connect 失败达到该次数才重新 SDP；0 非法
    uint32_t attempt_timeout_ms = kDefAttemptTimeoutMs; // SDP 或 connect 发出后无结果的超时，按失败处理
};

/** 累计统计；configure() 不清零，重新 start() 也不清零。 */
struct Stats {
    uint32_t attempts = 0;      // 发出的 connect 次数
    uint32_t sdp_queries = 0;   // 发出的 SDP 次数
    uint32_t failures = 0;      // SDP 失败 + connect 失败 + 超时
    uint32_t connects = 0;      // on_open 次数
    uint32_t disconnects = 0;   // 在线后 on_close 次数
    uint32_t last_delay_ms = 0; // 最近一次计划的等待时长（含抖动）
};

/**
 * 语义（实现须满足，J4 host 测试逐条覆盖）：
 * - start(now)：无目标 → 返回 false；否则进入尝试流程：有缓存 SCN 直接 kConnect，否则 kStartSdp；
 *   快速窗口从 now 起算。stop() → kStopped，之后所有事件只更新统计，不产生动作。
 * - 失败（SDP 失败、connect 失败、attempt 超时、在线时掉线）→ kBackoff，下一次尝试时刻 =
 *   now + delay。delay：处于快速窗口内 → fast_retry_ms；否则按 backoff_min_ms 起翻倍至 backoff_max_ms。
 *   抖动在 delay 上乘以 [100-j, 100+j]% 的均匀整数比例，随机数来自注入的 xorshift32 种子（固定种子可重复）。
 * - 掉线（on_close 且曾 kConnected）：若在线时长 ≥ stable_online_ms，退避级数复位并重新开启快速窗口；
 *   否则保留当前级数（防止“连上即断”时快速重试风暴）。
 * - SCN 缓存：on_sdp_result(ok, scn) 成功时缓存；连续 sdp_after_failures 次 connect 失败后清除缓存，下次走 SDP。
 * - poll(now)：kBackoff 且到期 → 返回下一次动作；kDiscovering/kConnecting 且超过 attempt_timeout_ms → 按失败处理。
 *   其它情况返回 kNone。所有 on_* 也可能直接返回动作（例如 SDP 成功后立即 kConnect）。
 * - 非预期事件（状态不匹配）忽略，返回 kNone，不改变状态。
 */
class Reconnector final {
public:
    constexpr Reconnector() noexcept = default;

    /** 配置非法（jitter_pct > kMaxJitterPct、sdp_after_failures == 0、backoff_min > backoff_max、任一时长为 0）→ false
     * 且不改状态。 */
    [[nodiscard]] bool configure(const Config &cfg, uint32_t seed) noexcept;
    /** 设置目标；改变目标会清除 SCN 缓存并回到 kIdle（若已在线，调用方负责断开）。 */
    void set_target(const Addr &addr) noexcept;
    void clear_target() noexcept;
    [[nodiscard]] bool has_target() const noexcept
    {
        return has_target_;
    }
    [[nodiscard]] const Addr &target() const noexcept
    {
        return addr_;
    }

    [[nodiscard]] bool start(uint32_t now_ms, Action &out) noexcept;
    void stop() noexcept;

    [[nodiscard]] Action poll(uint32_t now_ms) noexcept;
    [[nodiscard]] Action on_sdp_result(bool ok, uint8_t scn, uint32_t now_ms) noexcept;
    [[nodiscard]] Action on_open(uint32_t now_ms) noexcept;
    [[nodiscard]] Action on_connect_failed(uint32_t now_ms) noexcept;
    [[nodiscard]] Action on_close(uint32_t now_ms) noexcept;

    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }
    /** kBackoff 时距离下一次尝试的剩余 ms（已到期为 0）；其它状态返回 0。 */
    [[nodiscard]] uint32_t next_attempt_in(uint32_t now_ms) const noexcept;
    [[nodiscard]] const Stats &stats() const noexcept
    {
        return stats_;
    }

private:
    Config cfg_{};
    Stats stats_{};
    uint32_t rng_ = kDefaultSeed;
    uint32_t window_start_ms_ = 0;
    uint32_t state_since_ms_ = 0;
    uint32_t next_attempt_ms_ = 0;
    uint32_t backoff_ms_ = 0;
    Addr addr_{};
    uint8_t scn_ = 0;
    uint8_t connect_failures_ = 0;
    bool has_target_ = false;
    bool has_scn_ = false;
    State state_ = State::kIdle;
};

} // namespace reconnect
