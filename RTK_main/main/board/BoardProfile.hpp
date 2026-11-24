// 板级配置值类型、ID 窗口与电源保持判定（ADR-0015）。
// 配置表和判定函数无平台依赖，线程/ISR 安全；保持计数归调用方独占。
#pragma once

#include <cstdint>
#include <span>

namespace app::board {

enum class BoardId : uint8_t { kUnknown, kP2V141, kE1DevKit };
enum class HoldKind : uint8_t { kNone, kLatchGpio, kKeyEmulate };
enum class Feature : uint8_t {
    kCellular = 1U << 0U,
    kOled = 1U << 1U,
    kImu = 1U << 2U,
    kCharger = 1U << 3U,
    kBeeper = 1U << 4U,
    kMotor = 1U << 5U,
    kLeds = 1U << 6U,
};

struct GnssPins {
    int uart = -1;
    int rxGpio = -1;
    int txGpio = -1;
    int powerGpio = -1;
    bool powerActiveHigh = false;
    uint8_t receiverPort = 0U; // 0 沿用台架固件的 Kconfig 模块侧端口。
};

struct SafeOutput {
    int gpio = -1;
    bool high = false;
};

struct PowerHold {
    HoldKind kind = HoldKind::kNone;
    SafeOutput latch{};
    int keyGpio = -1;
    int senseGpio = -1;
    uint32_t senseOnMv = 0U;
    uint32_t holdTimeoutMs = 0U;
};

struct PowerKey {
    int keyDetectGpio = -1;
    int usbDetectGpio = -1;
    int chgStatGpio = -1;
    bool keyActiveLow = true;
    bool usbActiveLow = true;
    bool chgActiveLow = true;
};

struct ChargerPins {
    int i2cPort = -1; // -1 表示本板无 I2C 充电芯片，不初始化 I2C
    int sdaGpio = -1;
    int sclGpio = -1;
    uint8_t addr = 0U;  // 7-bit I2C 地址（P2: SGM41513 = 0x1A）
    uint8_t reg02 = 0U; // REG02[5:0] 充电电流目标值，[7:6] 保留芯片原值
    uint8_t reg03 = 0U; // REG03 预充/终止电流目标值
};

struct OledPins {
    int i2cPort = -1; // -1 表示本板无 OLED，不初始化 I2C/GPIO
    int sdaGpio = -1;
    int sclGpio = -1;
    int powerGpio = -1;      // 电源使能脚（P2: GPIO12 高有效，启动绑定脚仅运行期驱动）
    uint8_t addr = 0U;       // 7-bit I2C 地址（P2: SH1106 = 0x3C）
    bool rotated180 = false; // true 表示面板倒装 180°（SH1106: 0xA0 + 0xC0）
};

struct Profile {
    BoardId id = BoardId::kUnknown;
    const char *name = "unknown";
    const char *product = "RTK";
    const char *hardwareVersion = "UNKNOWN";
    int batteryAdcGpio = -1;
    uint32_t idMinMv = 0U;
    uint32_t idMaxMv = 0U;
    GnssPins gnss{};
    PowerHold powerHold{};
    PowerKey powerKey{};
    ChargerPins charger{};
    OledPins oled{};
    std::span<const SafeOutput> safeOutputs{};
    uint8_t features = 0U;
};

/**
 * @brief 判断板级配置是否包含指定外设特性位
 * @param profile: 已识别的板级配置
 * @param feature: 外设特性位枚举
 * @retval true 该板型具备该特性
 * @retval false 该板型不具备该特性
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr bool hasFeature(const Profile &profile, Feature feature) noexcept
{
    // Feature 底层类型为 uint8_t 位掩码，转换为 uint8_t 按位与无损。
    return (profile.features & static_cast<uint8_t>(feature)) != 0U;
}

/**
 * @brief 取得板级配置对应的产品短名（未识别时回退为 `"RTK"`）
 * @param profile: 板级配置指针，可为 nullptr
 * @retval 非空产品短名（如 `"P2"`、`"E1"`、`"RTK"`）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr const char *productName(const Profile *profile) noexcept
{
    if (profile != nullptr && profile->product != nullptr) {
        return profile->product;
    }
    return Profile{}.product;
}

/**
 * @brief 取得所有已登记板型
 * @retval 全生命周期有效的只读表视图
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] std::span<const Profile> profiles() noexcept;

/**
 * @brief 用 ID 电压匹配参与自动识别的板型
 * @param mv: ID 电压（mV）
 * @retval 匹配的静态表项，未知时 nullptr
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const Profile *matchById(uint32_t mv) noexcept;

/**
 * @brief 按枚举查找已登记板型
 * @param id: 板型枚举值
 * @retval 匹配的静态表项，未登记时 nullptr
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const Profile *byId(BoardId id) noexcept;

/**
 * @brief 选择 kLink 的空闲等待上限
 * @param hold: 当前板型的电源保持配置
 * @retval 空闲等待毫秒数，P2 需在 10 ms 内采样按键
 * @note 纯函数，可跨任务调用。
 */
[[nodiscard]] constexpr uint32_t linkIdleWaitMs(const PowerHold &hold) noexcept
{
    constexpr uint32_t kPowerKeyPollMs = 10U;
    constexpr uint32_t kDefaultLinkWaitMs = 20U;
    return hold.kind == HoldKind::kKeyEmulate ? kPowerKeyPollMs : kDefaultLinkWaitMs;
}

enum class HoldDecision : uint8_t { kContinue, kHeld, kTimeout };

/**
 * @brief 用单次电压及连续达标数判定 P2 电源保持进度
 * @param hold: 配置表的门限和时限
 * @param mv: 本次读数（mV）
 * @param elapsedMs: 模拟按下后的单调耗时（ms）
 * @param streak: 当前样本之前的连续达标数，由调用方保存；超时时保持原值
 * @retval kContinue 继续采样
 * @retval kHeld 截止前连续三次达标
 * @retval kTimeout 到达截止时间（边界时超时优先）
 * @note 线程/ISR 安全；计数由调用方独占。
 */
[[nodiscard]] HoldDecision checkHold(const PowerHold &hold, uint32_t mv, uint32_t elapsedMs,
                                     uint8_t &streak) noexcept;

/**
 * @brief 取得成功识别后发布的板级配置
 * @retval 识别结果；识别失败（console-only）时返回无有效引脚的未知板型
 * @note 跨任务只读，非 ISR。
 */
[[nodiscard]] const Profile &current() noexcept;

/**
 * @brief 取得连接 ESP32 的接收机端口号
 * @retval 当前板型的模块侧端口号，台架档沿用 Kconfig
 * @note 仅启用 GNSS 且识别成功后调用，非 ISR。
 */
[[nodiscard]] uint8_t gnssReceiverPort() noexcept;

} // namespace app::board
