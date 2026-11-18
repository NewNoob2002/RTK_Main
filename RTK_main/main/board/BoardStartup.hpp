// 板级启动期硬件操作（ADR-0015）。
// 由 app_main 启动任务顺序调用，非 ISR，可按 tick 阻塞。
#pragma once

#include "fw/Status.hpp"

namespace app::board {

struct Profile;
struct PowerHold;

/**
 * @brief 读取板级 ID、验证板配置并设置安全输出
 * @param[out] selected: 成功时为表项，失败时为 nullptr
 * @retval kOk 识别和安全输出设置完成
 * @retval kIoError ADC 或 GPIO 操作失败
 * @retval kNotFound 未命中板型
 * @retval kInvalidArg 板级 UART 与构建配置冲突
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] fw::Status identify(const Profile *&selected) noexcept;

/**
 * @brief 执行已识别板型的电源保持
 * @param hold: 板级电源保持配置
 * @retval kOk 保持成功或板型无需保持
 * @retval kIoError GPIO 或 ADC 操作失败
 * @retval kTimeout 截止前未检测到保持
 * @retval kInvalidArg 配置方式无效
 * @note 仅启动任务调用；P2 可阻塞至保持时限，非 ISR。
 */
[[nodiscard]] fw::Status enablePowerHold(const PowerHold &hold) noexcept;

} // namespace app::board
