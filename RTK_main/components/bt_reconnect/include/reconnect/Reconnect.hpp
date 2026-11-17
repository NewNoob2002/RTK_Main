// 通用 SPP 客户端重连策略状态机：纯 C++20，无 IDF/FreeRTOS/自有组件依赖。
// 只产生动作（esp_spp_start_discovery / esp_spp_connect / 定时），由调用方执行；所有方法只能由同一任务调用，非 ISR 安全。
// 时间单位 ms，u32 回绕由无符号差值处理；不阻塞、不分配堆。冻结接口（ADR-0007 §5）。
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
    kDiscovering, // 已发出 SDP，等待 onSdpResult
    kConnecting,  // 已发出 connect，等待 onOpen / onConnectFailed
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
    uint32_t fastWindowMs = kDefFastWindowMs; // 从 start 或掉线起算的快速重试窗口
    uint32_t fastRetryMs = kDefFastRetryMs;   // 快速窗口内的固定间隔
    uint32_t backoffMinMs = kDefBackoffMinMs; // 窗口外退避起点
    uint32_t backoffMaxMs = kDefBackoffMaxMs; // 退避上限（翻倍直到上限）
    uint8_t jitterPct = kDefJitterPct; // 抖动 ±jitterPct%，0 = 不抖动；> kMaxJitterPct 视为非法
    uint32_t stableOnlineMs = kDefStableOnlineMs; // 在线满该时长才复位退避
    uint8_t sdpAfterFailures =
        kDefSdpAfterFailures; // 有缓存 SCN 时，连续 connect 失败达到该次数才重新 SDP；0 非法
    uint32_t attemptTimeoutMs =
        kDefAttemptTimeoutMs; // SDP 或 connect 发出后无结果的超时，按失败处理
};

/** 累计统计；configure() 不清零，重新 start() 也不清零。 */
struct Stats {
    uint32_t attempts = 0;    // 发出的 connect 次数
    uint32_t sdpQueries = 0;  // 发出的 SDP 次数
    uint32_t failures = 0;    // SDP 失败 + connect 失败 + 超时
    uint32_t connects = 0;    // onOpen 次数
    uint32_t disconnects = 0; // 在线后 onClose 次数
    uint32_t lastDelayMs = 0; // 最近一次计划的等待时长（含抖动）
};

/**
 * 语义（host 测试逐条覆盖）：
 * - start(now)：无目标 → 返回 false；否则进入尝试流程：有缓存 SCN 直接 kConnect，否则 kStartSdp；
 *   快速窗口从 now 起算。stop() → kStopped，之后所有事件只更新统计，不产生动作。
 * - 失败（SDP 失败、connect 失败、attempt 超时、在线时掉线）→ kBackoff，下一次尝试时刻 =
 *   now + delay。delay：处于快速窗口内 → fastRetryMs；否则按 backoffMinMs 起翻倍至 backoffMaxMs。
 *   抖动在 delay 上乘以 [100-j, 100+j]% 的均匀整数比例，随机数来自注入的 xorshift32 种子（固定种子可重复）。
 * - 掉线（onClose 且曾 kConnected）：若在线时长 ≥ stableOnlineMs，退避级数复位并重新开启快速窗口；
 *   否则保留当前级数（防止“连上即断”时快速重试风暴）。
 * - SCN 缓存：onSdpResult(ok, scn) 成功时缓存；连续 sdpAfterFailures 次 connect 失败后清除缓存，下次走 SDP。
 * - poll(now)：kBackoff 且到期 → 返回下一次动作；kDiscovering/kConnecting 且超过 attemptTimeoutMs → 按失败处理。
 *   其它情况返回 kNone。所有 on_* 也可能直接返回动作（例如 SDP 成功后立即 kConnect）。
 * - 非预期事件（状态不匹配）忽略，返回 kNone，不改变状态。
 */
class Reconnector final {
public:
    constexpr Reconnector() noexcept = default;

    /**
     * @brief  校验并保存重连参数与随机数种子
     * @param  cfg: 重连参数，时长单位均为 ms
     * @param  seed: xorshift32 种子；0 表示改用 kDefaultSeed
     * @retval true 参数合法，已保存
     * @retval false 任一时长为 0、jitterPct > kMaxJitterPct、sdpAfterFailures == 0 或
     *         backoffMinMs > backoffMaxMs；此时配置与状态不变
     * @note   不清零 Stats，不改变状态机状态；单线程调用，非 ISR
     */
    [[nodiscard]] bool configure(const Config &cfg, uint32_t seed) noexcept;

    /**
     * @brief  设置重连目标
     * @param  addr: 经典蓝牙对端地址，6 字节大端线序
     * @note   与当前地址相同时不做改动；否则清除 SCN 缓存、连续失败计数与退避级数并回到 kIdle。
     *         若已在线，调用方负责先断开；单线程调用，非 ISR
     */
    void setTarget(const Addr &addr) noexcept;

    /**
     * @brief  清除重连目标并回到 kIdle
     * @note   同时清除 SCN 缓存、连续失败计数、退避级数与计时起点；累计 Stats 保留。
     *         单线程调用，非 ISR
     */
    void clearTarget() noexcept;

    /**
     * @brief  查询是否已设置目标
     * @retval true 已 setTarget 且尚未 clearTarget
     * @retval false 无目标
     */
    [[nodiscard]] bool hasTarget() const noexcept
    {
        return has_target_;
    }

    /**
     * @brief  读取当前目标地址
     * @retval 目标地址；未设置目标时为全 0
     */
    [[nodiscard]] const Addr &target() const noexcept
    {
        return addr_;
    }

    /**
     * @brief  启动重连流程并输出第一个要执行的动作
     * @param  nowMs: 当前毫秒时刻，同时作为快速重试窗口起点
     * @param[out] out: 本次动作；失败时为 kNone
     * @retval true 已进入 kConnecting（有 SCN 缓存）或 kDiscovering，out 为对应动作
     * @retval false 无目标，或当前处于 kDiscovering/kConnecting/kConnected；out 为 kNone
     * @note   可从 kIdle/kBackoff/kStopped 重新开始；清空退避级数与连续失败计数；单线程调用，非 ISR
     */
    [[nodiscard]] bool start(uint32_t nowMs, Action &out) noexcept;

    /**
     * @brief  停止重连状态机，直到再次 start
     * @note   进入 kStopped；之后所有 on_* 事件只累计统计、不产生动作。累计 Stats 保留，非 ISR
     */
    void stop() noexcept;

    /**
     * @brief  推进超时与退避计时，返回到期应执行的动作
     * @param  nowMs: 当前毫秒时刻；与内部记录做无符号差值，允许 u32 回绕
     * @retval kStartSdp 退避到期且无 SCN 缓存，调用方应发起 SDP
     * @retval kConnect 退避到期且有 SCN 缓存，调用方应按 scn 发起连接
     * @retval kNone 无到期动作，或状态不属于 kBackoff/kDiscovering/kConnecting
     * @note   kDiscovering/kConnecting 超过 attemptTimeoutMs 时按失败转入 kBackoff 并返回 kNone，
     *         与 onConnectFailed 一样累计失败并按 sdpAfterFailures 清除 SCN 缓存。
     *         由持有对象的任务周期调用，不阻塞、非 ISR 安全
     */
    [[nodiscard]] Action poll(uint32_t nowMs) noexcept;

    /**
     * @brief  处理 SDP 查询结果
     * @param  ok: 查询是否成功
     * @param  scn: 成功时的服务信道号；失败时忽略
     * @param  nowMs: 当前毫秒时刻
     * @retval kConnect 成功：已缓存 scn，应立刻发起连接
     * @retval kNone 失败已转入 kBackoff，或状态不是 kDiscovering/kStopped（非预期事件被忽略）
     * @note   失败累加 failures 并排定下一次尝试；kStopped 下只累计统计，非 ISR
     */
    [[nodiscard]] Action onSdpResult(bool ok, uint8_t scn, uint32_t nowMs) noexcept;

    /**
     * @brief  处理连接建立
     * @param  nowMs: 当前毫秒时刻，作为在线计时起点
     * @retval kNone 恒为 kNone；kConnecting 时转入 kConnected 并清零连续失败计数
     * @note   非预期状态忽略；kStopped 下只累计 connects；非 ISR
     */
    [[nodiscard]] Action onOpen(uint32_t nowMs) noexcept;

    /**
     * @brief  处理连接失败
     * @param  nowMs: 当前毫秒时刻
     * @retval kNone 恒为 kNone；kConnecting 时转入 kBackoff 并排定下一次尝试
     * @note   连续失败达到 sdpAfterFailures 时清除 SCN 缓存，下次走 SDP；
     *         非预期状态忽略；kStopped 下只累计 failures；非 ISR
     */
    [[nodiscard]] Action onConnectFailed(uint32_t nowMs) noexcept;

    /**
     * @brief  处理在线会话断开
     * @param  nowMs: 当前毫秒时刻
     * @retval kNone 恒为 kNone；kConnected 时转入 kBackoff 并排定下一次尝试
     * @note   在线时长 ≥ stableOnlineMs 时复位退避级数并重开快速窗口，否则保留级数；
     *         非预期状态忽略；kStopped 下只累计 disconnects；非 ISR
     */
    [[nodiscard]] Action onClose(uint32_t nowMs) noexcept;

    /**
     * @brief  读取当前状态
     * @retval 状态机的当前 State
     */
    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }

    /**
     * @brief  查询距离下一次重连尝试的剩余时间
     * @param  nowMs: 当前毫秒时刻
     * @retval 0 不在 kBackoff，或已到尝试时刻
     * @retval 正数 kBackoff 中的剩余毫秒数（已含抖动）
     */
    [[nodiscard]] uint32_t nextAttemptIn(uint32_t nowMs) const noexcept;

    /**
     * @brief  读取累计统计
     * @retval 统计快照的常量引用；configure() 与 start() 都不清零
     */
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
