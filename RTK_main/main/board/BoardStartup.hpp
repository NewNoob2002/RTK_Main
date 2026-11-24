// 板级启动期硬件操作（ADR-0015）。
// 由 app_main 启动任务顺序调用，非 ISR，可按 tick 阻塞。
#pragma once

#include "board/PowerLogic.hpp"
#include "fw/Status.hpp"

namespace app::board {

struct Profile;
struct PowerHold;
struct PowerKey;
struct ChargerPins;

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
 * @brief 将已识别板型的外设输出置为安全（关闭）电平（含 gpio_config，仅启动首次配置使用）
 * @param profile: 已识别板型配置
 * @retval kOk 全部安全输出设置完成
 * @retval kIoError 任一 GPIO 设置失败
 * @note 启动任务调用，非 ISR。
 */
[[nodiscard]] fw::Status applySafeOutputs(const Profile &profile) noexcept;

/**
 * @brief 仅更新已配置安全输出引脚的电平（不重复 gpio_config，避免关机静默阶段触发 gpio conflict 告警）
 * @param profile: 已识别板型配置
 * @retval kOk 全部安全输出电平设置完成
 * @retval kIoError 任一 GPIO 设电平失败
 * @note 仅在启动期已完成 applySafeOutputs 之后由 kLink 关机路径调用，非 ISR。
 */
[[nodiscard]] fw::Status setSafeOutputLevels(const Profile &profile) noexcept;

/**
 * @brief 执行已识别板型的电源保持
 * @param hold: 板级电源保持配置
 * @retval kOk 保持成功或板型无需保持
 * @retval kIoError GPIO 或 ADC 操作失败
 * @retval kTimeout 截止前未检测到保持
 * @retval kInvalidArg 配置方式无效
 * @note 仅启动任务或充电待机长按开机路径调用；P2 可阻塞至保持时限，非 ISR。
 */
[[nodiscard]] fw::Status enablePowerHold(const PowerHold &hold) noexcept;

/**
 * @brief 启动期电源分支与保持：P2 探测并配置充电芯片，用 GPIO25 和 USB 在位判定正常开机或充电待机
 * @param hold: 板级电源保持配置
 * @param key: 板级 USB 与 STAT 检测引脚配置
 * @param chargerPins: 板级 I2C 充电芯片配置（i2cPort < 0 时跳过）
 * @param[out] bootMode: 本次启动运行档（正常或充电待机）
 * @param[out] reason: 本次启动的开机原因（key / charge / latch）
 * @retval kOk 电源保持完成或进入充电待机
 * @retval kIoError GPIO 或 ADC 操作失败
 * @retval kTimeout 保持超时
 * @retval kInvalidArg 配置方式无效
 * @note 仅启动任务调用；P2 正常开机可阻塞至保持时限，非 ISR。
 */
[[nodiscard]] fw::Status startupPower(const PowerHold &hold, const PowerKey &key,
                                      const ChargerPins &chargerPins, BootMode &bootMode,
                                      PowerOnReason &reason) noexcept;

} // namespace app::board
