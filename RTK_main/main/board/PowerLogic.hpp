// P2 电源按键消抖、启动分支与关机序列纯状态机（B2，无 IDF/FreeRTOS 依赖）。
// 纯值类型与确定性状态机；无共享可变全局，状态由调用方独占，线程/ISR 安全。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <cstdint>
#include <optional>

#include "fw/Status.hpp"

namespace app::board {

/** 本次启动运行档：正常模式或插 USB 时的充电待机模式。 */
enum class BootMode : uint8_t {
    kNormal = 0,
    kChargeStandby,
};

/** 本次启动的开机原因，供 power status 与 HIL 诊断。 */
enum class PowerOnReason : uint8_t {
    kUnknown = 0,
    kKey,
    kCharge,
    kLatch,
};

/** P2 启动期读取 GPIO25 与 USB 后的电源保持分支结论。 */
enum class PowerOnAction : uint8_t {
    kHoldConfirm = 0, // GPIO25 已高：照 B1 流程确认保持，power_on=key
    kChargeStandby,   // GPIO25 低且 USB 在位：充电待机，不模拟按键，power_on=charge
    kHoldRecoverWarn, // GPIO25 低且无 USB：异常（按压过短），打 W 后照 B1 模拟按键
};

/** 按键状态机事件输出。 */
enum class KeyEvent : uint8_t {
    kNone = 0,
    kShort,
    kLongHold,
};

/** 关机请求来源（预留 kLowBattery 与 kPhone）。 */
enum class ShutdownReason : uint8_t {
    kNone = 0,
    kKey,
    kConsole,
    kLowBattery,
    kPhone,
};

/** 关机序列阶段（严格按 kAnnounce -> kSave -> kQuiesce -> kCutPower 推进）。 */
enum class ShutdownStage : uint8_t {
    kIdle = 0,
    kAnnounce,
    kSave,
    kQuiesce,
    kCutPower,
};

inline constexpr uint32_t kKeyPollPeriodMs = 10U;         // ms，按键轮询周期
inline constexpr uint32_t kKeyDebounceMs = 30U;           // ms，按键消抖时间
inline constexpr uint32_t kKeyShortMaxMs = 1000U;         // ms，短按上限（严格小于触发 kShort）
inline constexpr uint32_t kDefaultLongPressMs = 2500U;    // ms，默认长按关机/开机阈值
inline constexpr uint32_t kShutdownSaveTimeoutMs = 1000U; // ms，kSave 落盘等待上限
inline constexpr uint32_t kShutdownCutInitHighMs = 2000U; // ms，无 USB 时首段 GPIO4 高电平
inline constexpr uint32_t kShutdownCutToggleMs = 3100U;   // ms，无 USB 时高/低交替周期
inline constexpr uint32_t kShutdownCutTimeoutMs = 30000U; // ms，断电超时回退上限

// SGM41513 寄存器与位定义（BQ25601 兼容映射，待实板 `power charger` dump 核对）。
inline constexpr uint8_t kSgmReg00 = 0x00U;
inline constexpr uint8_t kSgmReg02 = 0x02U;  // [5:0] ICHG 充电电流（V2.7 置 51，保留 [7:6]）
inline constexpr uint8_t kSgmReg03 = 0x03U;  // [7:4] IPRECHG，[3:0] ITERM（V2.7 置 0xF8）
inline constexpr uint8_t kSgmReg08 = 0x08U;  // [7:5] VBUS_STAT，[4:3] CHRG_STAT，[2] PG_STAT
inline constexpr uint8_t kSgmReg0A = 0x0AU;  // [7] VBUS_GD
inline constexpr uint8_t kSgmRegCount = 12U; // REG00..REG0B 共 12 个寄存器

inline constexpr uint8_t kSgmReg02PreserveMask = 0xC0U;
inline constexpr uint8_t kSgmReg02IchgMask = 0x3FU;
inline constexpr uint8_t kSgmReg08VbusShift = 5U;
inline constexpr uint8_t kSgmReg08VbusMask = 0x07U;
inline constexpr uint8_t kSgmReg08ChrgShift = 3U;
inline constexpr uint8_t kSgmReg08ChrgMask = 0x03U;
inline constexpr uint8_t kSgmReg08PgBit = 1U << 2U;
inline constexpr uint8_t kSgmReg0AVbusGdBit = 1U << 7U;

/** USB 在位检测来源（i2c 优先，失败退化到 GPIO2 STAT，无硬件时为 none）。 */
enum class UsbSource : uint8_t {
    kNone = 0,
    kI2c,
    kStat,
};

/** SGM41513 REG08 解码后的输入电源与充电状态。 */
struct ChargerInputStatus {
    bool ok = false;
    bool powerGood = false;
    uint8_t vbusStat = 0U;
    uint8_t chrgStat = 0U;
    uint8_t rawReg08 = 0U;
};

/** 综合 I2C 与 STAT 退化后的 USB 在位判定结果。 */
struct UsbDetection {
    bool present = false;
    UsbSource source = UsbSource::kNone;
};

/**
 * @brief 取得 USB 检测来源的可读短名
 * @param source: 检测来源枚举
 * @retval 静态短名（i2c / stat / none）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *usbSourceName(UsbSource source) noexcept;

/**
 * @brief 解码 SGM41513 REG08（BQ25601 兼容位映射）
 * @param rawReg08: REG08 原始字节
 * @retval ok=true 且填好 powerGood / vbusStat / chrgStat / rawReg08 的结构
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] ChargerInputStatus decodeSgmReg08(uint8_t rawReg08) noexcept;

/**
 * @brief 更新 REG08 缓存，软件锁争用时沿用前次读数
 * @param[in,out] last: 前次样本，实际通信失败后置为无效
 * @param status: 本次事务状态
 * @param raw: 成功时的 REG08 字节
 * @note 由样本所有者调用，非阻塞。
 */
void updateChargerSample(ChargerInputStatus &last, fw::Status status, uint8_t raw) noexcept;

/**
 * @brief 按 V2.7 语义计算 SGM41513 REG02 目标值（保留高 2 位，写入低 6 位 ICHG 编码）
 * @param currentVal: 当前 REG02 读回值
 * @param ichgCode: 目标低 6 位充电电流编码（V2.7 为 51）
 * @retval 掩码合并后的 REG02 写入值
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t maskSgmReg02(uint8_t currentVal, uint8_t ichgCode) noexcept
{
    return static_cast<uint8_t>((currentVal & kSgmReg02PreserveMask) |
                                (ichgCode & kSgmReg02IchgMask));
}

/**
 * @brief 综合 SGM41513 I2C 状态与 STAT(GPIO2) 退化路径判定 USB 是否在位
 * @param i2cStatus: I2C 读取 REG08 的解码结果
 * @param statActive: STAT(GPIO2 PWR_CHG) 是否处于充电有效态（低电平）
 * @param hasStatPin: 板级是否配置了 STAT 检测脚
 * @retval 包含在位标志与来源（i2c / stat / none）的结果
 * @note 纯函数，线程/ISR 安全。已知限制：I2C 故障且电池充满（STAT 高）时退化路径无法检测到 USB。
 */
[[nodiscard]] UsbDetection decideUsbPresent(const ChargerInputStatus &i2cStatus, bool statActive,
                                            bool hasStatPin = true) noexcept;

/**
 * @brief 取得启动运行档的可读短名
 * @param mode: 启动运行档
 * @retval 静态短名（normal 或 charge_standby）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *bootModeName(BootMode mode) noexcept;

/**
 * @brief 取得开机原因的可读短名
 * @param reason: 开机原因
 * @retval 静态短名（key / charge / latch / unknown）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *powerOnReasonName(PowerOnReason reason) noexcept;

/**
 * @brief 取得关机请求原因的可读短名
 * @param reason: 关机原因
 * @retval 静态短名（none / key / console / low_battery / phone）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *shutdownReasonName(ShutdownReason reason) noexcept;

/**
 * @brief 取得关机阶段的可读短名
 * @param stage: 关机阶段
 * @retval 静态短名（idle / announce / save / quiesce / cut_power）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *shutdownStageName(ShutdownStage stage) noexcept;

/**
 * @brief 按 GPIO25（U3 OUT）与 USB 在位状态决定 P2 启动分支
 * @param senseHigh: 拉 GPIO4 前 GPIO25 是否已高于保持阈值
 * @param usbPresent: USB 输入电源是否在位（SGM41513 REG08 优先，失败退化到 STAT）
 * @retval kHoldConfirm GPIO25 已高，按键开机并确认保持
 * @retval kChargeStandby GPIO25 为低且 USB 在位，进入充电待机
 * @retval kHoldRecoverWarn GPIO25 为低且无 USB，异常恢复并补按保持
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] PowerOnAction decidePowerOn(bool senseHigh, bool usbPresent) noexcept;

// SGM41513 手册第 21 页要求输入持续 220 ms 才启动 REGN，余量留给后续输入检测。
inline constexpr uint32_t kBootUsbDetectionMs = 350U;
inline constexpr uint32_t kBootUsbPollMs = 20U;

/**
 * @brief 在初次启动判定异常时，有界重读 USB 状态
 * @param senseHigh: 初次 ADC 样本是否已达保持阈值
 * @param first: 初次 USB 检测结果
 * @param now: 返回单调毫秒的回调
 * @param wait: 等待指定毫秒的回调
 * @param readUsb: 重读 USB 状态的回调
 * @retval 第一次在位样本，或到期时最后一次结果
 * @note 仅启动任务调用，可阻塞，非 ISR。
 */
template <typename Clock, typename Wait, typename ReadUsb>
[[nodiscard]] UsbDetection waitForBootUsb(bool senseHigh, UsbDetection first, Clock now, Wait wait,
                                          ReadUsb readUsb) noexcept
{
    const uint32_t start = now();
    while (!senseHigh && !first.present) {
        const uint32_t elapsed = now() - start;
        if (elapsed >= kBootUsbDetectionMs) {
            break;
        }
        const uint32_t remaining = kBootUsbDetectionMs - elapsed;
        wait(remaining < kBootUsbPollMs ? remaining : kBootUsbPollMs);
        first = readUsb();
    }
    return first;
}

/**
 * 电源按键消抖与短按/长按判定状态机。
 * 启动若已处于按下态，须先观测到一次稳定松开后才开始计数；每次长按只触发一次 kLongHold。
 */
class PowerKeySm final {
public:
    /**
     * @brief 构造按键状态机
     * @param longPressMs: 长按触发阈值（ms），小于消抖时长时按消抖时长处理
     * @note constexpr 构造，不访问硬件，不分配堆。
     */
    constexpr explicit PowerKeySm(uint32_t longPressMs = kDefaultLongPressMs) noexcept
        : longPressMs_(longPressMs < kKeyDebounceMs ? kKeyDebounceMs : longPressMs)
    {}

    /**
     * @brief 重置内部状态并更新长按阈值
     * @param longPressMs: 长按触发阈值（ms），小于消抖时长时按消抖时长处理
     * @note 由持有者串行调用，不阻塞，非 ISR 限制无。
     */
    void reset(uint32_t longPressMs) noexcept;

    /**
     * @brief 以当前单调时间和原始按下状态推进按键状态机
     * @param nowMs: 单调时间（ms），支持 uint32_t 自然回绕
     * @param rawPressed: 本次采样的原始按键按下状态（true = 按下）
     * @retval KeyEvent::kNone 无新事件
     * @retval KeyEvent::kShort 稳定按下且小于 1000 ms 后稳定松开
     * @retval KeyEvent::kLongHold 持续按下达到 longPressMs（每次按下仅触发一次）
     * @note 由持有者串行调用，不阻塞、不分配堆。
     */
    [[nodiscard]] KeyEvent poll(uint32_t nowMs, bool rawPressed) noexcept;

    /**
     * @brief 查询最近一次消抖后的稳定按下状态
     * @retval true 当前处于稳定按下态
     * @retval false 当前处于稳定松开态或尚未采样
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] bool pressed() const noexcept
    {
        return stablePressed_;
    }

    /**
     * @brief 查询状态机是否已越过开机按住保护并进入就绪计数态
     * @retval true 已观测到稳定松开，后续按下正常计数
     * @retval false 尚未初始化或仍处于开机按住未松开状态
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] bool armed() const noexcept
    {
        return armed_;
    }

    /**
     * @brief 查询最近一次短按/长按的持续时间（ms）
     * @retval 持续时间（ms）
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] uint32_t lastHoldMs() const noexcept
    {
        return lastHoldMs_;
    }

private:
    uint32_t longPressMs_ = kDefaultLongPressMs;
    bool initialized_ = false;
    bool armed_ = false;
    bool stablePressed_ = false;
    bool candidatePressed_ = false;
    bool pressing_ = false;
    bool longFired_ = false;
    uint32_t candidateSinceMs_ = 0U;
    uint32_t pressStartMs_ = 0U;
    uint32_t lastHoldMs_ = 0U;
};

/** 充电待机保持确认输出，由 kLink 驱动按键和重启。 */
enum class StandbyWakeAction : uint8_t {
    kNone,
    kPress,
    kHeld,
    kTimeout,
};

/** 充电待机长按后逐轮确认数字保持电平。 */
class StandbyWakeSm final {
public:
    /**
     * @brief 推进待机保持确认
     * @param nowMs: 单调时钟（ms）
     * @param longPress: 本轮是否达到长按阈值
     * @param senseHigh: 当前 U3 OUT 数字电平
     * @param timeoutMs: 保持确认总时限（ms）
     * @retval 本轮是否开始模拟按压、已确认保持、超时或继续等待
     * @note 由同一任务每 10 ms 调用，非 ISR。
     */
    [[nodiscard]] StandbyWakeAction confirmStandbyHold(uint32_t nowMs, bool longPress,
                                                       bool senseHigh, uint32_t timeoutMs) noexcept;

private:
    uint32_t startedMs_ = 0U;
    unsigned streak_ = 0U;
    bool active_ = false;
};

/** 关机状态机单步输出动作，由硬件适配层执行 GPIO4/重启/日志。 */
struct ShutdownOutput {
    ShutdownStage stage = ShutdownStage::kIdle;
    bool stageEntered = false;
    bool saveTimedOut = false;
    bool driveKeyHigh = false;
    bool requestRestart = false;
    bool cutTimedOut = false;
};

/**
 * 电源关机序列状态机（announce -> save -> quiesce -> cut_power）。
 * 无阻塞，按单调毫秒时钟推进，支持无 USB 脉冲断电与有 USB 关 U3 后重启。
 */
class ShutdownSm final {
public:
    constexpr ShutdownSm() noexcept = default;

    /**
     * @brief 发起一次关机请求；关机进行中或原因为 kNone 时忽略
     * @param reason: 关机请求来源
     * @retval true 已受理并进入 kAnnounce
     * @retval false 正在关机中或参数为 kNone
     * @note 由持有者串行调用，不阻塞。
     */
    [[nodiscard]] bool request(ShutdownReason reason) noexcept;

    /**
     * @brief 推进关机状态机一步
     * @param nowMs: 单调时间（ms），支持 uint32_t 自然回绕
     * @param saveDone: kSave 阶段全部待落盘数据是否已写完
     * @param quiesceDone: kQuiesce 阶段外设静默与日志冲刷是否已完成
     * @param senseHigh: GPIO25（U3 OUT）当前数字电平是否为高
     * @param usbPresent: USB 输入电源当前是否在位，缺少有效样本时为 nullopt
     * @retval 本轮阶段、边沿标志及 GPIO4/重启控制输出
     * @note 由持有者串行调用，不阻塞、不分配堆。
     */
    [[nodiscard]] ShutdownOutput step(uint32_t nowMs, bool saveDone, bool quiesceDone,
                                      bool senseHigh, std::optional<bool> usbPresent) noexcept;

    /**
     * @brief 查询是否正在执行关机序列
     * @retval true 处于 kAnnounce..kCutPower 任一阶段
     * @retval false 空闲态（kIdle）
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] bool active() const noexcept
    {
        return stage_ != ShutdownStage::kIdle;
    }

    /**
     * @brief 查询当前所处关机阶段
     * @retval 当前阶段枚举
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] ShutdownStage stage() const noexcept
    {
        return stage_;
    }

    /**
     * @brief 查询当前关机原因（空闲态为 kNone）
     * @retval 当前原因枚举
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] ShutdownReason reason() const noexcept
    {
        return reason_;
    }

    /**
     * @brief 查询最近一次被受理的关机原因（超时回退后仍保留，供 power status 显示）
     * @retval 最近原因枚举
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] ShutdownReason lastReason() const noexcept
    {
        return lastReason_;
    }

private:
    /**
     * @brief 推进 kCutPower 阶段（区分有 USB 与无 USB 两种断电协议及 30 s 超时）
     * @param nowMs: 当前单调时间（ms）
     * @param senseHigh: GPIO25 当前是否为高
     * @param usbPresent: USB 当前是否在位，缺少有效样本时为 nullopt
     * @retval 本轮输出动作
     * @note 仅 step() 在 kCutPower 阶段调用。
     */
    [[nodiscard]] ShutdownOutput stepCutPower(uint32_t nowMs, bool senseHigh,
                                              std::optional<bool> usbPresent) noexcept;

    ShutdownStage stage_ = ShutdownStage::kIdle;
    ShutdownReason reason_ = ShutdownReason::kNone;
    ShutdownReason lastReason_ = ShutdownReason::kNone;
    bool stageEntered_ = false;
    bool cutFirstHigh_ = false;
    bool cutHigh_ = false;
    bool cutUsbPresent_ = false;
    uint32_t stageStartMs_ = 0U;
    uint32_t cutToggleStartMs_ = 0U;
};

} // namespace app::board
