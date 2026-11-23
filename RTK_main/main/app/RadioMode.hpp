// 启动射频模式、一次性请求和模块启用条件（ADR-0017）。
// 纯查询可在任意任务或 ISR 调用，RTC 请求接口的线程约束见各函数。
#pragma once

#include <cstdint>
#include <optional>

#include "board/PowerLogic.hpp"

namespace app {

/** kWifiConfig 中释放 BTDM 内存后，本次启动禁止调用任何 esp_bt_* 控制器或设备 API。 */
enum class RadioMode : uint8_t { kBluetooth, kWifiConfig };
enum class RadioReason : uint8_t { kConsole, kPhone, kWebExit, kIdle, kUi };
enum class ModuleEnable : uint8_t { kAlways, kNormalOnly, kBtOnly, kWifiOnly };

struct RadioSelection {
    RadioMode mode = RadioMode::kBluetooth;
    std::optional<RadioReason> reason{};
};

/**
 * @brief 判断启动运行档与射频模式是否启用该模块
 * @param bootMode: 本次启动运行档（正常或充电待机）
 * @param mode: 启动选定的射频模式
 * @param enable: 模块登记的启用条件
 * @retval true 模块应装配并运行
 * @retval false 跳过此模块
 * @note 纯查询，线程和 ISR 安全。
 */
[[nodiscard]] constexpr bool moduleEnabled(board::BootMode bootMode, RadioMode mode,
                                           ModuleEnable enable) noexcept
{
    if (enable == ModuleEnable::kAlways) {
        return true;
    }
    if (bootMode != board::BootMode::kNormal) {
        return false;
    }
    return enable == ModuleEnable::kNormalOnly ||
           (mode == RadioMode::kBluetooth && enable == ModuleEnable::kBtOnly) ||
           (mode == RadioMode::kWifiConfig && enable == ModuleEnable::kWifiOnly);
}

/**
 * @brief 判断正常启动下射频模式是否启用该模块
 * @param mode: 启动选定的射频模式
 * @param enable: 模块登记的启用条件
 * @retval true 模块应装配并运行
 * @retval false 跳过此模块
 * @note 纯查询，线程和 ISR 安全。
 */
[[nodiscard]] constexpr bool moduleEnabled(RadioMode mode, ModuleEnable enable) noexcept
{
    return moduleEnabled(board::BootMode::kNormal, mode, enable);
}

/**
 * @brief 取得用于启动和控制台输出的模式名
 * @param mode: 射频模式
 * @retval 静态短名 bt 或 wifi
 * @note 纯查询，线程和 ISR 安全。
 */
[[nodiscard]] constexpr const char *radioModeName(RadioMode mode) noexcept
{
    return mode == RadioMode::kWifiConfig ? "wifi" : "bt";
}

/**
 * @brief 取得用于启动和控制台输出的请求原因名
 * @param reason: 上一次被接受的请求原因
 * @retval 静态原因短名，无请求时返回 default
 * @note 纯查询，线程和 ISR 安全。
 */
[[nodiscard]] const char *radioReasonName(std::optional<RadioReason> reason) noexcept;

/**
 * @brief 编码为包含 magic、mode、reason 和 CRC-8 的单字 RTC 记录
 * @param mode: 待请求的射频模式
 * @param reason: 切换原因
 * @retval 编码后的记录
 * @note 纯计算，线程和 ISR 安全。
 */
[[nodiscard]] uint32_t encodeRadioRecord(RadioMode mode, RadioReason reason) noexcept;

/**
 * @brief 校验并解码 RTC 请求记录
 * @param record: 待读取的单字记录
 * @retval 有效请求或默认蓝牙选择
 * @note 纯计算，线程和 ISR 安全。
 */
[[nodiscard]] RadioSelection decodeRadioRecord(uint32_t record) noexcept;

/**
 * @brief 读取并立即清除请求记录，只接受软件重启传来的请求
 * @param record: 需清除的单字记录
 * @param softwareReset: 本次复位是否来自 esp_restart
 * @retval 有效请求或默认蓝牙选择
 * @note 启动期单线程，非 ISR，不阻塞。volatile 确保读写跨复位保留区。
 */
[[nodiscard]] RadioSelection consumeRadioRecord(volatile uint32_t &record,
                                                bool softwareReset) noexcept;

/**
 * @brief 保存一次性请求并通过 esp_restart 切换模式
 * @param mode: 下次启动的射频模式
 * @param reason: 切换原因
 * @note 仅任务调用，非 ISR，调用者须串行化重启请求。构建关闭本功能时重启采用蓝牙。
 */
[[noreturn]] void requestRadioMode(RadioMode mode, RadioReason reason) noexcept;

/**
 * @brief 消费 RTC 内存中的一次性请求。功能关闭时也清除记录，并返回默认蓝牙选择。
 * @retval 本次启动选择的模式与原因
 * @note 仅启动任务调用一次，非 ISR，不阻塞。
 */
[[nodiscard]] RadioSelection consumeRadioRequest() noexcept;

} // namespace app
