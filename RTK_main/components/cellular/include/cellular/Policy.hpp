// 4G 链路的退避与 AT 超时常量表（ADR-0021 §2.4/§2.5）：所有时间常量集中于此，主机测试逐级覆盖。
// 纯 C++20 常量与 constexpr 函数，无状态，线程/ISR 安全。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cellular {

/** 指数退避参数：第 n 次（从 0 起）等待 initialMs << n，封顶 capMs。 */
struct BackoffPolicy final {
    uint32_t initialMs = 0U;
    uint32_t capMs = 0U;
};

/**
 * @brief  计算第 attempt 次失败后的退避毫秒
 * @param  policy:  退避参数
 * @param  attempt: 连续失败次数（0 表示第一次失败）
 * @retval 退避毫秒，倍增后不超过 policy.capMs
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint32_t backoffMs(const BackoffPolicy &policy, uint8_t attempt) noexcept
{
    uint32_t value = policy.initialMs;
    for (uint8_t i = 0U; i < attempt; ++i) {
        if (value >= policy.capMs) {
            break;
        }
        value *= 2U;
    }
    return (value > policy.capMs) ? policy.capMs : value;
}

/** 链路恢复与巡检策略总表（单一 constexpr 表，ADR-0021 §2.5）。 */
struct Policy final {
    BackoffPolicy l1{2000U, 60000U};   // 连接级：2 s→4 s→…→60 s
    BackoffPolicy l2{10000U, 120000U}; // 承载级：10 s→…→120 s
    std::array<uint32_t, 4> l3StepsMs{30000U, 60000U, 120000U,
                                      300000U}; // 模块级：30→60→120→300 s 封顶
    uint32_t powerOffMinMs = 2000U;             // L3 断电保持下限
    uint32_t syncWindowMs = 10000U;             // 上电后 AT 同步窗口
    uint32_t syncRetryMs = 500U;                // AT 同步重试间隔
    uint32_t syncSwapBaudAttempts = 3U;         // 连续失败多少次后换另一个波特率探测
    uint32_t simPollMs = 2000U;                 // SIM 检查轮询间隔
    uint32_t registerPollMs = 2000U;            // 注册状态轮询间隔
    uint32_t registerTimeoutMs = 180000U;       // 注册超时 → L3
    uint32_t healthPeriodMs = 30000U;           // Ready 巡检周期（CSQ + CEREG? + CPIN?）
    uint8_t l1FailToL2 = 5U;                    // 单连接连续失败升级 L2
    uint8_t qiactFailToL3 = 3U;                 // QIACT 连续失败升级 L3
    uint8_t deadTimeouts = 3U;                  // AT 连续无响应判通道失效
    uint32_t bootBaud = 115200U;                // EC600U 出厂波特率
    uint32_t fastBaud = 460800U;                // AT+IPR 目标波特率
    uint32_t maxPayloadWireMs = 15U; // 单次数据写（QISEND 载荷）线上时间上限；512 B@460800≈11 ms
};

/** 全局唯一策略实例。 */
inline constexpr Policy kPolicy{};

/**
 * @brief  计算第 attempt 次连续 L3 恢复后的退避毫秒
 * @param  attempt: 连续 L3 次数（0 表示第一次）
 * @retval 查表值（ADR-0021 §2.5 逐字：30/60/120/300 s，之后保持 300 s）
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint32_t l3BackoffMs(uint8_t attempt) noexcept
{
    const size_t last = kPolicy.l3StepsMs.size() - 1U;
    const size_t idx = (static_cast<size_t>(attempt) > last) ? last : static_cast<size_t>(attempt);
    return kPolicy.l3StepsMs[idx];
}

/** 各 AT 命令超时（毫秒，ADR-0021 §2.4：普通 300 ms–1 s；QIACT/QIOPEN 150 s）。 */
namespace at_timeout {
inline constexpr uint32_t kSync = 300U;  // AT / ATE0 / AT+CMEE
inline constexpr uint32_t kShort = 300U; // AT+CSQ / AT+GSN
inline constexpr uint32_t kMedium =
    1000U; // AT+IPR / AT+QCCID / AT+CPIN? / AT+CEREG? / AT+QICSGP / AT+QIRD
inline constexpr uint32_t kQiact = 150000U;
inline constexpr uint32_t kQideact = 40000U;
inline constexpr uint32_t kQiopenResult = 150000U; // QIOPEN 结果 URC 等待上限
inline constexpr uint32_t kQiclose = 10000U;
inline constexpr uint32_t kQisend = 5000U;
inline constexpr uint32_t kQpowd = 1000U;
inline constexpr uint32_t kPoweredDownWait = 1500U; // AT+QPOWD=1 后等 POWERED DOWN
inline constexpr uint32_t kDebugDefault = 5000U;    // 控制台 net at 默认超时
} // namespace at_timeout

/** 优雅关机总预算（PW1 kQuiesce 与 kNet 共用同一绝对截止）：QICLOSE → QPOWD → 等 POWERED DOWN。 */
inline constexpr uint32_t kShutdownTotalMs = 2000U;

/** 单次读取块与发送块上限（ADR-0021 §2.4）。 */
inline constexpr uint16_t kReadChunkBytes = 1024U;
inline constexpr uint16_t kSendChunkBytes = 512U;

} // namespace cellular
