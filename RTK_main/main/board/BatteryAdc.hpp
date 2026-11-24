// P2 电池分压 ADC 采样接口。
// 启动任务初始化，随后仅 kLink 读取；可在启动阶段采样，非 ISR。
#pragma once

#include <cstdint>

#include "fw/Status.hpp"

namespace app::board {

/**
 * @brief 在板识别释放 ADC1 后初始化电池通道及 line fitting 校准
 * @param gpio: 板级电池分压输入；未配置时传 -1
 * @retval kOk 已就绪
 * @retval kNotFound 本板无电池 ADC
 * @retval kInvalidArg 引脚不属于 ADC1
 * @retval 其他 IDF 初始化错误映射码
 * @note 仅启动任务调用；SDK 启动期为 ADC 单元与校准各分配一次，非 ISR。
 */
[[nodiscard]] fw::Status initBatteryAdc(int gpio) noexcept;

/**
 * @brief 读取电池电压，使用校准毫伏及板级 ×2 分压系数
 * @param[out] mv: 成功时写入电池端 mV；失败时保留原值
 * @retval kOk 样本有效
 * @retval kInvalidState 电池 ADC 未就绪
 * @retval 其他 IDF 采样或校准错误映射码
 * @note 启动任务与 kLink 串行调用，不分配堆，非 ISR。
 */
[[nodiscard]] fw::Status readBatteryAdcMv(uint32_t &mv) noexcept;

} // namespace app::board
