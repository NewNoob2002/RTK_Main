// 电源按键与关机管理模块（kLink 执行器，B2）：按键消抖、关机请求与分阶段关机、充电待机开机。
// init 仅启动任务调用；poll 仅 kLink 任务调用（I2C 单次传输最多等待 10 ms，不分配堆，非 ISR）；requestShutdown 任意任务安全。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "board/PowerLogic.hpp"
#include "cli/Console.hpp"
#include "def/SystemDef.hpp"
#include "fw/Result.hpp"
#include "rtos/Executor.hpp"

namespace app {
struct AppContext;

namespace power {

/**
 * @brief 线程安全发起关机请求（由 kLink 的 PowerModule 分阶段执行，不在调用方上下文断电）
 * @param reason: 关机原因（kKey / kConsole / kLowBattery / kPhone）
 * @retval true 请求已受理
 * @retval false 充电待机、模块未启用、已有动作或原因为 kNone
 * @note 任意任务可调用，不阻塞、不分配堆，非 ISR。
 */
[[nodiscard]] bool requestShutdown(board::ShutdownReason reason) noexcept;

} // namespace power

/** 电源模块（kLink 执行器）：P2 按键轮询、关机序列状态机与充电待机长按开机。 */
class PowerModule final {
public:
    constexpr PowerModule() noexcept = default;
    ~PowerModule() = default;
    PowerModule(const PowerModule &) = delete;
    PowerModule &operator=(const PowerModule &) = delete;
    PowerModule(PowerModule &&) = delete;
    PowerModule &operator=(PowerModule &&) = delete;

    /**
     * @brief 绑定所属应用上下文
     * @param context: 固件唯一应用上下文，生命周期覆盖模块
     * @note 仅启动前单线程调用，不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }

    /**
     * @brief 初始化电源按键与检测引脚（仅 P2 kKeyEmulate 板型生效，其他板直接返回 kOk 不启用）
     * @retval fw::Result::kOk 初始化完成或本板型不启用
     * @retval fw::Result::kInvalidState 上下文未绑定
     * @retval fw::Result::kIoError GPIO 配置失败
     * @note 仅启动前单线程调用；首次电池采样可等待，非 ISR。
     */
    [[nodiscard]] fw::Result init() noexcept;

    /**
     * @brief 10 ms 周期轮询按键、推进关机序列并发布电源快照
     * @param nowMs: 当前单调时间（ms）
     * @retval false 总是（按执行器周期轮询）
     * @note 仅 kLink 任务调用，I2C 单次传输最多等待 10 ms，非 ISR。
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;

private:
    struct PinLevels {
        bool keyPressed = false;
        bool u3On = false;
        bool usbPresent = false;
        bool usbKnown = false;
        bool charging = false;
        board::UsbSource usbSource = board::UsbSource::kNone;
        bool chargerOk = false;
        uint8_t chargerReg08 = 0U;
    };

    /**
     * @brief 取得已绑定的应用上下文引用
     * @retval 应用上下文引用
     * @note 仅在已绑定后调用，非 ISR。
     */
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    /**
     * @brief 配置按键与 U3 检测输入，并独立记录 GPIO27/STAT 是否可用
     * @retval true 两个必需输入配置成功
     * @retval false 必需输入配置失败（已记 E 级日志）
     * @note 仅 init 调用，非 ISR。
     */
    [[nodiscard]] bool configureInputPins() noexcept;

    /**
     * @brief 读取按键、U3、STAT，并在 GPIO27 不可用时按 200 ms 节流刷新 REG08
     * @param nowMs: 当前单调时间（ms）
     * @param forceChargerRead: true 时需要 I2C 回退则忽略 200 ms 节流
     * @retval 本轮采样结果
     * @note 仅启动任务或 kLink 调用，非 ISR。
     */
    [[nodiscard]] PinLevels samplePins(uint32_t nowMs, bool forceChargerRead) noexcept;

    /**
     * @brief 启动电池采样并读取第一组滤波结果，失败时允许电源模块继续工作
     * @param gpio: 电池分压输入，未配置时为 -1
     * @note 仅启动任务；组内采样间隔 5 ms，非 ISR。
     */
    void initBattery(int gpio) noexcept;

    /**
     * @brief 收集一个电池样本，成组时更新中值与百分比
     * @param nowMs: 本次采样的单调时间（ms）
     * @retval kOk 收到样本并更新时间；若本组收齐则更新电压和电量
     * @retval 其他 ADC 错误，清除本组采样与发布值
     * @note 启动任务或 kLink 串行调用，非 ISR。
     */
    [[nodiscard]] fw::Status takeBatterySample(uint32_t nowMs) noexcept;

    /**
     * @brief 用每轮至多一个样本更新电量，并经 B2 仲裁提交持续低电关机
     * @param nowMs: 当前单调时间（ms）
     * @param pins: 本轮 USB 在位和可用性
     * @note 仅 kLink 调用，非 ISR。
     */
    void pollBattery(uint32_t nowMs, const PinLevels &pins) noexcept;

    /**
     * @brief 分发按键短/长按事件并处理 8 s 强制关机兜底
     * @param nowMs: 当前单调时间（ms）
     * @param event: 本轮按键状态机事件
     * @param keyPressed: 当前物理按键是否处于按下状态
     * @note 仅 kLink 在 poll 中调用，非阻塞，非 ISR。
     */
    void handleKeyPoll(uint32_t nowMs, board::KeyEvent event, bool keyPressed) noexcept;

    /**
     * @brief 在充电待机中逐轮确认保持并安排重启
     * @param nowMs: 当前单调时间（ms）
     * @param longPress: 本轮是否达到长按阈值
     * @param senseHigh: U3 OUT 数字电平
     * @note 仅 kLink，非阻塞，非 ISR。
     */
    void handleChargeStandbyPowerOn(uint32_t nowMs, bool longPress, bool senseHigh) noexcept;

    /**
     * @brief 判断异步日志与 UART 是否已排空
     * @param elapsed: 自开始等待已过去的时间（ms）
     * @retval true 已完成
     * @retval false 继续等待
     * @note 仅 kLink，非阻塞，非 ISR。
     */
    [[nodiscard]] bool isLogDrained(uint32_t elapsed) const noexcept;

    /** @brief 投递并轮询关门后的执行器队尾命令。 @note 仅 kLink，非阻塞。 */
    void pollSaveFences() noexcept;

    /**
     * @brief 推进关机状态机并执行当前阶段动作
     * @param nowMs: 当前单调时间（ms）
     * @param[in,out] pins: 本轮采样的引脚电平，进入断电阶段前更新
     * @note 仅 kLink 调用，I2C 单次传输最多等待 10 ms，非 ISR。
     */
    void stepShutdown(uint32_t nowMs, PinLevels &pins) noexcept;

    /**
     * @brief 判断 kSave 阶段全部待落盘数据与在途设置是否已清空
     * @retval true 可进入 kQuiesce
     * @retval false 仍有延迟写或在途写入
     * @note 仅 kLink 调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool isSaveComplete() const noexcept;

    /**
     * @brief 记录 kSave 超时时仍未完成的保存项告警日志
     * @note 仅 kLink 在 kSave 超时边沿调用一次，非 ISR。
     */
    void logSaveTimeoutItems() const noexcept;

    /**
     * @brief 执行关机阶段刚进入时的一次性动作（日志、拦截写入、冲刷、静默或拉高 GPIO4）
     * @param nowMs: 当前单调时间（ms）
     * @param pins: 本轮采样的引脚电平
     * @param out: 本轮状态机输出
     * @note 仅 kLink 调用，不阻塞，非 ISR。
     */
    void handleShutdownStageEntry(uint32_t nowMs, const PinLevels &pins,
                                  const board::ShutdownOutput &out) noexcept;

    /**
     * @brief 处理 kCutPower 阶段的 GPIO4 驱动、有 USB 重启与 30 s 超时回退
     * @param nowMs: 当前单调时间（ms）
     * @param out: 本轮状态机输出
     * @note 仅 kLink 调用，不阻塞，非 ISR。
     */
    void handleShutdownCutPower(uint32_t nowMs, const board::ShutdownOutput &out) noexcept;

    /**
     * @brief 执行 kQuiesce 进入动作：断蓝牙/停 Wi-Fi、外设回安全电平并打印关机日志
     * @param nowMs: 当前单调时间（ms）
     * @note 仅 kLink 调用，非 ISR。
     */
    void enterQuiesce(uint32_t nowMs) noexcept;

    /** @brief 在静默期限到达时记录仍未停止的无线业务。 @note 仅 kLink。 */
    void logQuiesceTimeout() noexcept;

    /**
     * @brief 判断 kQuiesce 日志冲刷等待是否完成
     * @param nowMs: 当前单调时间（ms）
     * @retval true 日志已排空或到达有界等待上限
     * @retval false 继续等待输出任务排空日志
     * @note 仅 kLink 调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool isQuiesceDone(uint32_t nowMs) const noexcept;

    /**
     * @brief 驱动 GPIO4（TELL_MCU_KEY）输出目标电平
     * @param high: true 拉高模拟按下，false 拉低释放
     * @note 仅 kLink 调用，非 ISR。
     */
    void setKeyDrive(bool high) noexcept;

    /**
     * @brief 计算当前关机阶段的已完成里程碑位掩码（供 OLED 关机进度页只读消费）
     * @param pins: 当前供电检测结果
     * @retval ShutdownDoneBit 组合掩码，空闲态为 0
     * @note 仅 init 或 kLink 调用，不阻塞，非 ISR。
     */
    [[nodiscard]] uint8_t computeShutdownDoneMask(const PinLevels &pins) const noexcept;

    /**
     * @brief 计算当前关机阶段的降级结果位掩码（超时/外设安全电平失败，与完成位独立）
     * @retval ShutdownDegradedBit 组合掩码，无降级或空闲态为 0
     * @note 仅 init 或 kLink 调用，不阻塞，非 ISR。
     */
    [[nodiscard]] uint8_t computeShutdownDegradedMask() const noexcept;

    /**
     * @brief 判断应用层无线业务是否已完成关机收尾
     * @retval true SPP 已断开且扫描关闭请求已提交，或 SoftAP 已停用，或无需收尾
     * @retval false 本模式的应用层收尾仍在进行
     * @note 仅 kLink 调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool isRadioQuiesced() const noexcept;

    /**
     * @brief 复位单次关机周期的真实完成位与降级位记录
     * @note 仅 init 或 kLink 调用，不阻塞，非 ISR。
     */
    void resetShutdownProgress() noexcept;

    /**
     * @brief 在真实 FreeRTOS 任务上下文中等待 OLED USB 终帧完成通知或到达绝对 30 ms 截止
     * @param self: 当前任务句柄（非空）
     * @note 仅 USB 关机重启前最后一步调用，每次相对等待最多一 tick，非 ISR。
     */
    void waitOledFinalFrame(TaskHandle_t self) noexcept;

    /**
     * @brief 确认当前阶段已显示足够久，屏幕故障时按有界等待降级
     * @param nowMs: 当前单调时间
     * @param stage: 正在呈现的阶段
     * @retval 是否可继续推进
     */
    [[nodiscard]] bool shutdownStageVisible(uint32_t nowMs, board::ShutdownStage stage) noexcept;

    /**
     * @brief 确认终帧已停留足够久或刷出截止已到
     * @param nowMs: 当前单调时间
     * @param flushDeadlineMs: 最晚等到终帧写出的绝对时间
     * @param usb: true 检查 USB 重启终帧，false 检查电池断电终帧
     * @retval 是否放行断电或重启
     */
    [[nodiscard]] bool finalFrameReady(uint32_t nowMs, uint32_t flushDeadlineMs, bool usb) noexcept;

    /**
     * @brief 推进日志排空及 USB 终帧停留，条件满足后重启
     * @param nowMs: 当前单调时间
     * @retval 执行器无需加速轮询
     */
    [[nodiscard]] bool pollRestart(uint32_t nowMs) noexcept;

    /**
     * @brief 供 UiEventBus 终帧刷出后调用的非阻塞重启唤醒钩子
     * @param ctx: 未使用
     * @note 由 OLED 任务调用，不阻塞，非 ISR。
     */
    static void wakeRestartWait(void *ctx) noexcept;

    /**
     * @brief 在状态变化时发布电源快照到 DataCenter
     * @param pins: 本轮采样电平
     * @param force: true 强制写入（init 时使用）
     * @note 仅 init 或 kLink 调用，不阻塞，非 ISR。
     */
    void publishSnapshot(const PinLevels &pins, bool force) noexcept;

    AppContext *context_ = nullptr;
    bool active_ = false;
    bool keyDriveHigh_ = false;
    bool forcePowerOffFired_ = false;
    board::PowerKeySm keySm_{};
    board::ShutdownSm shutdownSm_{};
    board::StandbyWakeSm standbyWake_{};
    rtos::Ticket linkFence_{};
    rtos::Ticket ioFence_{};
    bool linkFenceReached_ = false;
    bool ioFenceReached_ = false;
    bool restartPending_ = false;
    uint32_t restartStartMs_ = 0U;
    bool restartLogReady_ = false;
    uint32_t oledWaitDeadlineMs_ = 0U;
    uint32_t stageEnteredMs_ = 0U;
    std::optional<uint32_t> stageOnScreenMs_{};
    std::optional<uint32_t> finalHoldDeadlineMs_{};
    bool finalFrameReady_ = false;
    bool saveFencesDone_ = false;
    bool saveFlushedDone_ = false;
    bool saveTimedOut_ = false;
    bool peripheralsSafeDone_ = false;
    bool peripheralsSafeFailed_ = false;
    bool radioQuiescedDone_ = false;
    bool radioTimedOut_ = false;
    uint32_t shortPresses_ = 0U;
    uint32_t longPresses_ = 0U;
    uint32_t quiesceStartMs_ = 0U;
    uint32_t cutStartMs_ = 0U;
    uint32_t lastChargerPollMs_ = 0U;
    bool chargerSampled_ = false;
    bool chargerRetry_ = false;
    board::ChargerInputStatus lastChargerStatus_{};
    bool usbGpioAvailable_ = false;
    bool statGpioAvailable_ = false;
    bool batteryAdcReady_ = false;
    static constexpr size_t kBatterySamples = 20U;
    std::array<uint32_t, kBatterySamples> batterySamples_{};
    size_t batterySampleCount_ = 0U;
    uint32_t lastBatteryBatchMs_ = 0U;
    uint32_t lastBatterySampleMs_ = 0U;
    uint32_t batteryMv_ = 0U;
    uint8_t batteryPct_ = board::kBatteryPctUnknown;
    board::LowBatteryTimer lowBattery_{};
    PowerSnapshot lastSnapshot_{};
};

/**
 * @brief 取得电源控制台命令表（DEV 档注册）
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取，不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> powerCmds() noexcept;

/**
 * @brief 取得电源子命令动词表（DEV 档注册）
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取，不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> powerVerbs() noexcept;

} // namespace app
