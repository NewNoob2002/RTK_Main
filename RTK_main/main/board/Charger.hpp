// P2 SGM41513 I2C 充电芯片驱动（ESP-IDF v6.1 i2c_master，100 kHz，超时 <= 10 ms）。
// init 仅启动任务调用（幂等）；读写接口内部由静态互斥锁保护，可在启动任务、kLink 与 console 任务调用，非 ISR。
// 堆档位：禁止区（驱动本身不分配堆；IDF i2c_new_master_bus 的一次性内部分配仅在启动期 init 发生）。
#pragma once

#include <cstdint>
#include <span>

#include "board/BoardProfile.hpp"
#include "board/PowerLogic.hpp"
#include "fw/Status.hpp"

namespace app::board::charger {

/**
 * @brief 初始化 SGM41513 I2C 主机总线与设备句柄（幂等；i2cPort < 0 时返回 kNotFound）
 * @param pins: 板级充电芯片 I2C 与参数配置
 * @retval kOk 已初始化或重复调用
 * @retval kNotFound 本板未配置充电芯片
 * @retval kInvalidArg 引脚或地址配置非法
 * @retval 其他 IDF I2C 初始化失败映射码
 * @note 启动任务调用，非 ISR，可短暂阻塞。
 */
[[nodiscard]] fw::Status init(const ChargerPins &pins) noexcept;

/**
 * @brief 查询 SGM41513 I2C 设备是否已完成初始化
 * @retval true 已就绪
 * @retval false 未初始化或初始化失败
 * @note 任意任务可调用，不阻塞，非 ISR。
 */
[[nodiscard]] bool isReady() noexcept;

/**
 * @brief 读取 SGM41513 单个 8 位寄存器
 * @param reg: 寄存器地址（0x00..0x0B）
 * @param[out] value: 成功时写入读回字节，失败时保留原值
 * @retval kOk 读取成功
 * @retval kInvalidState 驱动未初始化
 * @retval kTimeout I2C 传输超时（10 ms）
 * @retval 其他 IDF I2C 错误映射码
 * @note 任意任务可调用，互斥最多等待 20 ms，另加单次传输最多 10 ms，非 ISR。
 */
[[nodiscard]] fw::Status readReg(uint8_t reg, uint8_t &value) noexcept;

/**
 * @brief 读-改-写 SGM41513 寄存器指定掩码位（new = (cur & ~mask) | (value & mask)）
 * @param reg: 寄存器地址
 * @param mask: 需要覆盖的位掩码
 * @param value: 写入位值
 * @retval kOk 写入成功
 * @retval kInvalidState 驱动未初始化
 * @retval kTimeout I2C 传输超时
 * @retval 其他 IDF I2C 错误映射码
 * @note 任意任务可调用，互斥最多等待 20 ms，另加两次传输各最多 10 ms，非 ISR。
 */
[[nodiscard]] fw::Status writeRegMasked(uint8_t reg, uint8_t mask, uint8_t value) noexcept;

/**
 * @brief 读取并解码 REG08，互斥被占用时保留前次样本
 * @param[in,out] last: REG08 缓存；通信错误或未初始化时置为无效
 * @retval kOk 获得新样本
 * @retval kBusy 锁争用，保留缓存
 * @retval 其他 通信失败或驱动未就绪
 * @note 任意任务可调用，锁不等待，实际传输最多 10 ms，非 ISR。
 */
[[nodiscard]] fw::Status refreshInputStatus(ChargerInputStatus &last) noexcept;

/**
 * @brief 移植 V2.7 充电参数：配置 REG02[5:0]=ichgCode（保留高 2 位）与 REG03=reg03
 * @param pins: 板级充电参数配置
 * @retval kOk 两个寄存器均写入成功
 * @retval 其他 读取或写入失败错误码（调用方仅记 W 日志，不阻断开机）
 * @note 启动早期（正常开机与充电待机两条路径）调用，非 ISR。
 */
[[nodiscard]] fw::Status applyChargeParams(const ChargerPins &pins) noexcept;

/**
 * @brief 连续只读转储 REG00..REG0B 共 12 个寄存器，供 `power charger` 核对位定义
 * @param[out] out: 12 字节输出缓冲；失败时仅保证前缀已读部分有效
 * @retval kOk 全部 12 个寄存器读取成功
 * @retval 其他 I2C 未就绪或读取错误
 * @note 仅控制台诊断路径调用，每次读取单独持锁，非 ISR。
 */
[[nodiscard]] fw::Status dumpRegs(std::span<uint8_t, kSgmRegCount> out) noexcept;

} // namespace app::board::charger
