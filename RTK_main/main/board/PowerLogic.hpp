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

/** 关机请求来源（含低电关机，kPhone 预留）。 */
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

/** 关机序列已完成里程碑位掩码（CODING-STYLE 规则 3，供 OLED 关机进度页只读消费）。 */
enum class ShutdownDoneBit : uint8_t {
    kAnnounced = 1U << 0U,       ///< 已进入关机流程（kAnnounce）
    kSaveFences = 1U << 1U,      ///< kLink / kIo 队尾保存栅栏已到达
    kSaveFlushed = 1U << 2U,     ///< NVS 延迟写与在途设置已落盘（进入 kQuiesce）
    kRadioQuiesced = 1U << 3U,   ///< SPP 已断开且扫描关闭请求已提交，或 SoftAP 已停用
    kPeripheralsSafe = 1U << 4U, ///< 外设输出已置安全电平
    kCutPower = 1U << 5U,        ///< 已进入断电动作阶段（kCutPower）
    kUsbOffReady = 1U << 6U,     ///< USB 路径 U3 已关断，等待最后一帧刷出后重启
};

/**
 * @brief 取得关机完成里程碑位对应的 uint8_t 掩码
 * @param bit: 关机里程碑位枚举
 * @retval 对应位掩码
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t shutdownDoneBit(ShutdownDoneBit bit) noexcept
{
    return static_cast<uint8_t>(bit);
}

/** 关机序列降级结果位掩码（超时/失败后仍按基线策略强制下电或 USB 重启兜底，供 OLED 独立展示降级文案）。 */
enum class ShutdownDegradedBit : uint8_t {
    kSaveTimeout = 1U << 0U,       ///< kSave 超时（1 s）仍有未落盘项或未到达栅栏
    kPeripheralsFailed = 1U << 1U, ///< 外设安全电平配置返回错误
    kRadioTimeout = 1U << 2U,      ///< 蓝牙或 Wi-Fi SoftAP 静默断开超时
};

/**
 * @brief 取得关机降级结果位对应的 uint8_t 掩码
 * @param bit: 关机降级结果位枚举
 * @retval 对应位掩码
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t shutdownDegradedBit(ShutdownDegradedBit bit) noexcept
{
    return static_cast<uint8_t>(bit);
}

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
inline constexpr uint8_t kSgmReg02 = 0x02U;  // [5:0] ICHG 充电电流
inline constexpr uint8_t kSgmReg03 = 0x03U;  // [7:4] IPRECHG，[3:0] ITERM
inline constexpr uint8_t kSgmReg08 = 0x08U;  // [7:5] VBUS_STAT，[4:3] CHRG_STAT，[2] PG_STAT
inline constexpr uint8_t kSgmReg0A = 0x0AU;  // [7] VBUS_GD
inline constexpr uint8_t kSgmRegCount = 12U; // REG00..REG0B 共 12 个寄存器

inline constexpr uint8_t kSgmReg08VbusShift = 5U;
inline constexpr uint8_t kSgmReg08VbusMask = 0x07U;
inline constexpr uint8_t kSgmReg08ChrgShift = 3U;
inline constexpr uint8_t kSgmReg08ChrgMask = 0x03U;
inline constexpr uint8_t kSgmReg08PgBit = 1U << 2U;
inline constexpr uint8_t kSgmReg0AVbusGdBit = 1U << 7U;

/** USB 在位检测来源（GPIO27 → I2C REG08 → GPIO2 STAT）。 */
enum class UsbSource : uint8_t {
    kNone = 0,
    kI2c,
    kStat,
    kGpio,
};

/** SGM41513 REG08 解码后的输入电源与充电状态。 */
struct ChargerInputStatus {
    bool ok = false;
    bool powerGood = false;
    uint8_t vbusStat = 0U;
    uint8_t chrgStat = 0U;
    uint8_t rawReg08 = 0U;
};

/** USB 在位判定结果。 */
struct UsbDetection {
    bool present = false;
    UsbSource source = UsbSource::kNone;
};

/**
 * @brief 取得 USB 检测来源的可读短名
 * @param source: 检测来源枚举
 * @retval 静态短名（gpio / i2c / stat / none）
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
 * @brief 依 GPIO27、SGM41513 REG08、GPIO2 STAT 的顺序判定 USB 在位
 * @param gpioPresent: GPIO27 的在位读数；未配置或配置失败时为 nullopt
 * @param i2cStatus: 在线充电芯片的 REG08 样本
 * @param statActive: STAT 低有效；未配置或配置失败时为 nullopt
 * @retval 在位标志及其来源
 * @note 纯函数，线程/ISR 安全。STAT 高无法区分无 USB 与充满。
 */
[[nodiscard]] UsbDetection decideUsbPresent(std::optional<bool> gpioPresent,
                                            const ChargerInputStatus &i2cStatus,
                                            std::optional<bool> statActive) noexcept;

inline constexpr uint8_t kBatteryPctUnknown = 0xFFU;

/**
 * @brief 按旧 P2 电芯电压表作分段线性插值
 * @param mv: 校准及还原分压后的电池电压（mV）
 * @retval 0..100 的整数百分比，向下取整
 * @note 纯函数，可跨任务或从 ISR 调用。
 */
[[nodiscard]] uint8_t batteryPercent(uint32_t mv) noexcept;

/** 由 kLink 持有的低电持续计时器。 */
class LowBatteryTimer final {
public:
    /**
     * @brief 判断电池供电且低于 10% 是否已持续至少 30 s
     * @param nowMs: 当前单调时间（ms）
     * @param onBattery: 已可靠确认 USB 不在位
     * @param pct: 电量百分比，未知时用 kBatteryPctUnknown
     * @retval true 低电持续时间已达阈值
     * @retval false 条件未满足；供电变化或电量恢复/未知会清除计时
     * @note 由调用方串行调用，不阻塞，支持 uint32_t 时钟回绕。
     */
    [[nodiscard]] bool isDue(uint32_t nowMs, bool onBattery, uint8_t pct) noexcept;

private:
    std::optional<uint32_t> sinceMs_{};
};

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
 * @param usbPresent: USB 输入电源是否在位（依次采用 GPIO27、在线 REG08、STAT）
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
