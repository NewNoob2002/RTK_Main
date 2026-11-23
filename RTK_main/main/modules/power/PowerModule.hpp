// 电源按键与关机管理模块（kLink 执行器，B2）：按键消抖、关机请求与分阶段关机、充电待机开机。
// init 仅启动任务调用；poll 仅 kLink 任务调用（非阻塞、不分配堆，非 ISR）；requestShutdown 任意任务安全。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <cstdint>
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
     * @note 仅启动前单线程调用，非 ISR，不分配堆。
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
     * @brief 配置 P2 电源相关数字输入脚（GPIO18/25/2，均关内部上下拉）
     * @retval true 全部引脚配置成功
     * @retval false 任一引脚配置失败（已记 E 级日志）
     * @note 仅 init 调用，非 ISR。
     */
    [[nodiscard]] bool configureInputPins() noexcept;

    /**
     * @brief 读取按键、U3 OUT、STAT 引脚并按 200 ms 节流刷新 SGM41513 I2C USB 在位状态
     * @param nowMs: 当前单调时间（ms）
     * @param forceChargerRead: true 时忽略 200 ms 节流立即读一次 I2C
     * @retval 本轮采样结果
     * @note 仅 kLink 在 active_ 为真时调用，非 ISR。
     */
    [[nodiscard]] PinLevels samplePins(uint32_t nowMs, bool forceChargerRead) noexcept;

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
     * @brief 在状态变化时发布电源快照到 DataCenter
     * @param pins: 本轮采样电平
     * @param force: true 强制写入（init 时使用）
     * @note 仅 init 或 kLink 调用，不阻塞，非 ISR。
     */
    void publishSnapshot(const PinLevels &pins, bool force) noexcept;

    AppContext *context_ = nullptr;
    bool active_ = false;
    bool keyDriveHigh_ = false;
    board::PowerKeySm keySm_{};
    board::ShutdownSm shutdownSm_{};
    board::StandbyWakeSm standbyWake_{};
    rtos::Ticket linkFence_{};
    rtos::Ticket ioFence_{};
    bool linkFenceReached_ = false;
    bool ioFenceReached_ = false;
    bool restartPending_ = false;
    uint32_t restartStartMs_ = 0U;
    uint32_t shortPresses_ = 0U;
    uint32_t longPresses_ = 0U;
    uint32_t quiesceStartMs_ = 0U;
    uint32_t cutStartMs_ = 0U;
    uint32_t lastChargerPollMs_ = 0U;
    bool chargerSampled_ = false;
    bool chargerRetry_ = false;
    board::ChargerInputStatus lastChargerStatus_{};
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
