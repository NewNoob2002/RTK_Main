// 4G 电源门控策略（ADR-0021 §2.9 / N1 复审 #7）：纯逻辑，主机可测。
//  - 配网（kWifiConfig）与关机：整体断电（powerAllowed=false）。
//  - 电量 < kLowBatteryPct：只禁止“新上电”（startAllowed=false），已在线模块保持到 PW1 低电关机流程；
//    回升到 阈值 + kLowBatteryResumeMargin 才恢复新上电（迟滞）。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <cstdint>

#include "board/PowerLogic.hpp"

namespace app {

/** 低电量回升余量（%）：禁用后需回升到阈值 + 该余量才允许再次新上电。 */
inline constexpr uint8_t kNetLowBatteryResumeMargin = 5U;

/** 电源门控输入。 */
struct NetGateInput {
    bool wifiConfig = false;                        // 当前为 Wi-Fi 配网模式
    bool shutdown = false;                          // PW1 关机已开始
    uint8_t batteryPct = board::kBatteryPctUnknown; // 电量百分比，未知为 kBatteryPctUnknown
};

/** 电源门控决策。 */
struct NetGateDecision {
    bool powerAllowed = true; // false：必须（保持）断电
    bool startAllowed = true; // false：Off 阶段不允许新上电（已在线不受影响）
};

/** 低电量迟滞状态机（仅 kNet 任务使用）。 */
class NetPowerGate final {
public:
    /**
     * @brief 按输入更新迟滞并给出决策
     * @param in: 当前输入
     * @retval 门控决策
     * @note 纯逻辑，不阻塞，非 ISR。
     */
    [[nodiscard]] NetGateDecision update(const NetGateInput &in) noexcept
    {
        if (in.batteryPct != board::kBatteryPctUnknown) {
            if (in.batteryPct < board::kLowBatteryPct) {
                lowLatched_ = true;
            } else if (in.batteryPct >= board::kLowBatteryPct + kNetLowBatteryResumeMargin) {
                lowLatched_ = false;
            }
        }
        NetGateDecision out;
        out.powerAllowed = !in.wifiConfig && !in.shutdown;
        out.startAllowed = !lowLatched_;
        return out;
    }

    /**
     * @brief 低电量迟滞是否已锁定
     * @retval true 已锁定（禁止新上电）
     */
    [[nodiscard]] bool lowLatched() const noexcept
    {
        return lowLatched_;
    }

private:
    bool lowLatched_ = false;
};

} // namespace app
