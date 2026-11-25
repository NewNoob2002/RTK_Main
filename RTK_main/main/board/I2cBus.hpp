// 板级 I2C0 共享总线单一所有者（ADR-0020 §2.2，ESP-IDF v6.1 driver/i2c_master.h）。
// ensureInit 仅启动期串行调用（幂等）；BusLock 静态互斥锁串行化 Charger、OLED 与诊断扫描访问，非 ISR。
// 堆档位：启动期一次性（本文件不分配堆；IDF i2c_new_master_bus/add_device 在启动期一次性内部分配）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fw/Status.hpp"

namespace app::board::i2c {

inline constexpr uint32_t kDefaultMutexWaitMs = 20U; // ms，跨任务互斥等待上限
inline constexpr int kDefaultScanTimeoutMs = 10;     // ms，扫描单地址探测超时上限
inline constexpr uint8_t kScanFirstAddress = 0x08U;
inline constexpr uint8_t kScanLastAddress = 0x77U;
inline constexpr size_t kScanAddressCount = kScanLastAddress - kScanFirstAddress + 1U;
inline constexpr unsigned kProbeRecoveryRetries = 2U; // 探测总线故障时复位并重试次数（总计 3 次）

/** 共享 I2C 总线的静态互斥锁 RAII 守卫，确保跨任务（kLink/oled/console）事务串行化。 */
class BusLock final {
public:
    /**
     * @brief 尝试在有界超时内获取共享 I2C 总线互斥锁
     * @param ticks: 最大等待 tick，0 为立即尝试（非阻塞）
     * @note 任意任务可调用，最多阻塞 ticks，非 ISR。
     */
    explicit BusLock(TickType_t ticks = pdMS_TO_TICKS(kDefaultMutexWaitMs)) noexcept;

    /** @brief 释放已持有的共享 I2C 总线互斥锁；非 ISR。 */
    ~BusLock() noexcept;

    BusLock(const BusLock &) = delete;
    BusLock &operator=(const BusLock &) = delete;
    BusLock(BusLock &&) = delete;
    BusLock &operator=(BusLock &&) = delete;

    /**
     * @brief 查询是否成功取得总线互斥锁
     * @retval true 已持锁
     * @retval false 总线未初始化或获取超时
     * @note 不阻塞，非 ISR。
     */
    [[nodiscard]] bool locked() const noexcept
    {
        return locked_;
    }

private:
    SemaphoreHandle_t handle_ = nullptr;
    bool locked_ = false;
};

/**
 * @brief 总线建链前检查 SDA 电平；若被从机拉低则以开漏 GPIO 发出最多 9 个 SCL 脉冲与 STOP 释放总线
 * @param sdaGpio: SDA 引脚编号
 * @param sclGpio: SCL 引脚编号
 * @retval true SDA 处于高电平（原本空闲或恢复成功）
 * @retval false 恢复脉冲后 SDA 仍为低电平
 * @note 仅启动期 i2c_new_master_bus 之前调用，不分配堆，非 ISR。
 */
[[nodiscard]] bool recoverBusPins(int sdaGpio, int sclGpio) noexcept;

/**
 * @brief 幂等创建板级共享 I2C 主总线与静态互斥锁（全生命周期仅创建一次）
 * @param port: I2C 端口号（如 0）
 * @param sdaGpio: SDA 引脚编号
 * @param sclGpio: SCL 引脚编号
 * @retval kOk 总线已就绪（首次创建或相同参数重复调用）
 * @retval kInvalidArg 引脚/端口非法，或与已创建总线的引脚不一致
 * @retval kNoSpace 静态互斥锁创建失败
 * @retval 其他 IDF i2c_new_master_bus 错误映射码
 * @note 仅启动任务调用，可短暂阻塞，非 ISR。
 */
[[nodiscard]] fw::Status ensureInit(int port, int sdaGpio, int sclGpio) noexcept;

/**
 * @brief 查询板级共享 I2C 总线是否已就绪
 * @retval true 总线句柄与互斥锁均有效
 * @retval false 尚未初始化或初始化失败
 * @note 任意任务可调用，不阻塞，非 ISR。
 */
[[nodiscard]] bool isReady() noexcept;

/**
 * @brief 探测指定 7-bit I2C 地址并带回原始 IDF 错误码（内部自动加锁）
 * @param addr: 7-bit I2C 地址
 * @param timeoutMs: 单次探测超时上限（ms）
 * @param[out] rawErr: 成功时写 ESP_OK，失败时写原始 esp_err_t
 * @param lockWaitMs: 总线互斥锁等待上限（ms）
 * @retval kOk 从设备应答
 * @retval kInvalidState 总线未初始化
 * @retval kBusy 互斥锁等待超时
 * @retval kNotFound 从设备无应答（ESP_ERR_NOT_FOUND）
 * @retval 其他 IDF 错误映射码
 * @note 任意任务可调用，可阻塞，非 ISR。
 */
[[nodiscard]] fw::Status probe(uint8_t addr, int timeoutMs, esp_err_t &rawErr,
                               uint32_t lockWaitMs = kDefaultMutexWaitMs) noexcept;

/**
 * @brief 在共享总线上挂载独立从设备句柄（各设备可使用不同时钟频率）
 * @param addr: 7-bit I2C 地址
 * @param sclSpeedHz: 该设备的 SCL 时钟频率（Hz，如 100000 或 400000）
 * @param[out] out: 成功时写入设备句柄，失败时保持原值
 * @retval kOk 挂载成功
 * @retval kInvalidState 总线未初始化
 * @retval kInvalidArg 地址或速率为 0
 * @retval 其他 IDF 错误映射码
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] fw::Status addDevice(uint8_t addr, uint32_t sclSpeedHz,
                                   i2c_master_dev_handle_t &out) noexcept;

/**
 * @brief 从共享总线卸载从设备句柄并置空（用于初始化失败回收，避免句柄泄漏）
 * @param[in,out] dev: 待卸载设备句柄；空句柄直接返回 kOk，卸载后置 nullptr
 * @retval kOk 卸载成功或句柄本就为空
 * @retval kInvalidState 总线未初始化（句柄已置空）
 * @retval kBusy 获取总线锁超时
 * @retval 其他 IDF i2c_master_bus_rm_device 错误映射码
 * @note 仅启动失败回滚路径调用，非 ISR。
 */
[[nodiscard]] fw::Status removeDevice(i2c_master_dev_handle_t &dev) noexcept;

/**
 * @brief 持锁复位共享 I2C 主总线控制器以从 SDA/SCL 卡死超时恢复
 * @param lockWaitMs: 总线互斥锁等待上限（ms）
 * @retval kOk 总线复位完成
 * @retval kInvalidState 总线未初始化
 * @retval kBusy 互斥锁等待超时
 * @retval 其他 IDF i2c_master_bus_reset 错误映射码
 * @note 任意任务在传输超时释放旧锁后调用，可短暂阻塞，非 ISR。
 */
[[nodiscard]] fw::Status resetBus(uint32_t lockWaitMs = kDefaultMutexWaitMs) noexcept;

/**
 * @brief 扫描共享总线上应答的 7-bit 地址（0x08..0x77），每地址单独持锁
 * @param[out] addresses: 应答地址按升序写入前缀，出错时保留已扫前缀
 * @param[out] count: 已写入的地址数量
 * @param timeoutMs: 单地址探测超时上限（ms）
 * @retval kOk 扫描完成
 * @retval kInvalidState 总线未初始化
 * @retval kBusy 未取得总线锁
 * @retval 其他 探测超时或总线故障映射码
 * @note 仅 DEV 控制台诊断路径调用，每地址最多阻塞 lockWait + timeoutMs，非 ISR。
 */
[[nodiscard]] fw::Status scanBus(std::span<uint8_t, kScanAddressCount> addresses, size_t &count,
                                 int timeoutMs = kDefaultScanTimeoutMs) noexcept;

} // namespace app::board::i2c
