// OLED 显示模块跨模块共享类型与轻量 UI 事件总线（ADR-0020 / OL2）。
// 纯值类型与无锁原子事件通道，无 IDF/硬件依赖；notify* 可在任意任务调用，不阻塞、非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "fw/Atomic.hpp"
namespace app {

/** OLED 当前显示页面（OL2：自检、表盘、四子菜单、充电待机、关机进度）。 */
enum class OledPage : uint8_t {
    kSystemCheck = 0,
    kDialplate,
    kSubSat,
    kSubPos,
    kSubDevice,
    kSubPower,
    kChargeStandby,
    kShutdown,
};

/**
 * @brief 取得 OLED 页面枚举的简短诊断名
 * @param page: OLED 页面枚举值
 * @retval 静态短名（system_check/dialplate/sat/pos/device/power/charge_standby/shutdown）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr const char *oledPageName(OledPage page) noexcept
{
    switch (page) {
    case OledPage::kSystemCheck:
        return "system_check";
    case OledPage::kDialplate:
        return "dialplate";
    case OledPage::kSubSat:
        return "sat";
    case OledPage::kSubPos:
        return "pos";
    case OledPage::kSubDevice:
        return "device";
    case OledPage::kSubPower:
        return "power";
    case OledPage::kChargeStandby:
        return "charge_standby";
    case OledPage::kShutdown:
        return "shutdown";
    default:
        return "unknown";
    }
}

/** OLED 状态快照（oled 任务写，console `oled` 命令读）。 */
struct OledStatus {
    bool enabled = false;    // 板型具备 Feature::kOled
    bool present = false;    // SH1106 探测成功并完成初始化
    bool poweredOff = false; // 面板及 GPIO12 已断电
    OledPage page = OledPage::kSystemCheck;
    uint16_t fpsX10 = 0U;          // 最近 1 s 窗口实际刷屏帧率 × 10（静止为 0）
    uint8_t lastDirtyPages = 0U;   // 最近一次写出帧的脏页数（0..8）
    uint32_t framesFlushed = 0U;   // 实际写出到 I2C 的帧数
    uint32_t framesSkipped = 0U;   // 内容未变跳过的空闲帧数
    uint32_t totalDirtyPages = 0U; // 累计写出的脏页总数
    uint32_t lastI2cUs = 0U;       // 最近一次写出帧的 I2C 耗时（us）
    uint32_t maxI2cUs = 0U;        // 历史最大单帧 I2C 耗时（us）
    uint32_t lastFrameUs = 0U;     // 最近一次写出帧的渲染+I2C 总耗时（us）
    uint32_t maxFrameUs = 0U;      // 历史最大单帧总耗时（us）
    uint32_t i2cErrors = 0U;       // 累计 I2C 传输失败次数
};

/** UiEventBus 原子状态标志位（CODING-STYLE 规则 3）。 */
enum class UiBusFlag : uint8_t {
    kOledActive = 1U << 0U,           ///< OLED 刷新任务已启动在线
    kShutdownFinalFlushed = 1U << 1U, ///< USB 关机终帧（已关机/充电中）已刷出到 I2C
};

/**
 * @brief 取得 UiBusFlag 对应的 uint32_t 掩码位
 * @param flag: 标志位枚举
 * @retval 对应位掩码
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint32_t uiBusFlagBit(UiBusFlag flag) noexcept
{
    return static_cast<uint32_t>(flag);
}

/**
 * 解耦按键/关机事件与 OLED 渲染任务的轻量通知通道。
 * PowerModule 只写原子计数并调用可选唤醒钩子，不在 kLink 内直接画屏或访问 I2C。
 */
class UiEventBus final {
public:
    using WakeFn = void (*)(void *ctx) noexcept;
    static constexpr size_t kShortKeyQueueCap = 8U; // SPSC 短按时间戳队列容量（2 的幂）
    static constexpr size_t kShortKeyQueueMask = kShortKeyQueueCap - 1U;

    constexpr UiEventBus() noexcept = default;
    ~UiEventBus() = default;
    UiEventBus(const UiEventBus &) = delete;
    UiEventBus &operator=(const UiEventBus &) = delete;
    UiEventBus(UiEventBus &&) = delete;
    UiEventBus &operator=(UiEventBus &&) = delete;
    /**
     * @brief 绑定或解绑 OLED 任务唤醒钩子（启动时绑定，退出前清零）
     * @param fn: 唤醒回调函数，nullptr 表示解绑
     * @param ctx: 透传给 fn 的静态对象指针
     * @note 仅 OLED 模块启动/停机路径调用；不阻塞，非 ISR。
     */
    void setWakeHook(WakeFn fn, void *ctx) noexcept
    {
        if (fn == nullptr) {
            wakeFn_.store(nullptr, std::memory_order_release);
            return;
        }
        wakeCtx_ = ctx;
        wakeFn_.store(fn, std::memory_order_release);
    }

    /**
     * @brief 绑定或解绑 USB 关机终帧完成后的重启路径唤醒钩子（如唤醒 kLink 任务）
     * @param fn: 唤醒回调函数，nullptr 表示解绑
     * @param ctx: 透传给 fn 的静态对象指针
     * @note 仅 PowerModule 在 USB 终帧等待窗口内绑定/解绑；不阻塞，非 ISR。
     */
    void setRestartWakeHook(WakeFn fn, void *ctx) noexcept
    {
        if (fn == nullptr) {
            restartWakeFn_.store(nullptr, std::memory_order_release);
            return;
        }
        restartWakeCtx_ = ctx;
        restartWakeFn_.store(fn, std::memory_order_release);
    }

    /**
     * @brief 登记 OLED 刷新任务是否在线运行（供 PowerModule 判断 USB 关机是否需等终帧）
     * @param active: true 表示 OLED 任务已运行；false 表示离线或未启用
     * @note 仅 OLED 模块任务启停调用；无锁原子写，不阻塞，非 ISR。
     */
    void setOledActive(bool active) noexcept
    {
        const uint32_t bit = uiBusFlagBit(UiBusFlag::kOledActive);
        if (active) {
            static_cast<void>(flags_.fetch_or(bit, std::memory_order_release));
        } else {
            static_cast<void>(flags_.fetch_and(~bit, std::memory_order_release));
        }
    }

    /**
     * @brief 查询 OLED 刷新任务是否在线运行
     * @retval true OLED 任务在线
     * @retval false OLED 未启用或未运行
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] bool oledActive() const noexcept
    {
        return (flags_.load(std::memory_order_acquire) & uiBusFlagBit(UiBusFlag::kOledActive)) !=
               0U;
    }

    /**
     * @brief 清除 USB 关机终帧已刷出标志（发起新关机或 30 s 超时取消时调用）
     * @note 仅 PowerModule 调用；无锁原子写，不阻塞，非 ISR。
     */
    void resetShutdownFinalFrame() noexcept
    {
        static_cast<void>(flags_.fetch_and(~uiBusFlagBit(UiBusFlag::kShutdownFinalFlushed),
                                           std::memory_order_release));
    }

    /**
     * @brief 标记 USB 关机终帧（已关机/充电中）已刷出到 SH1106 并立即唤醒等待重启的任务
     * @note 仅 OLED 刷新任务在 I2C 刷出成功后调用；无锁原子写，不阻塞，非 ISR。
     */
    void markShutdownFinalFrameFlushed() noexcept
    {
        static_cast<void>(flags_.fetch_or(uiBusFlagBit(UiBusFlag::kShutdownFinalFlushed),
                                          std::memory_order_release));
        if (const WakeFn fn = restartWakeFn_.load(std::memory_order_acquire); fn != nullptr) {
            fn(restartWakeCtx_);
        }
    }

    /**
     * @brief 查询 USB 关机终帧是否已刷出到面板
     * @retval true 已刷出，PowerModule 可立即继续重启
     * @retval false 尚未刷出
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] bool shutdownFinalFrameFlushed() const noexcept
    {
        return (flags_.load(std::memory_order_acquire) &
                uiBusFlagBit(UiBusFlag::kShutdownFinalFlushed)) != 0U;
    }

    /**
     * @brief 投递一次消抖短按事件（附带生产时刻单调时间戳）并唤醒 UI 任务（如有绑定）
     * @param eventMs: 消抖短按事件产生时刻（ms）
     * @note 仅 kLink 的 PowerModule 单生产者调用；无锁 SPSC 队列，不阻塞、不分配堆，非 ISR。
     */
    void notifyShortKey(uint32_t eventMs = 0U) noexcept
    {
        const uint32_t writeIdx = shortKeyWrite_.load(std::memory_order_relaxed);
        const uint32_t readIdx = shortKeyRead_.load(std::memory_order_acquire);
        if ((writeIdx - readIdx) < kShortKeyQueueCap) {
            shortKeyTimes_[writeIdx & kShortKeyQueueMask] = eventMs;
            shortKeyWrite_.store(writeIdx + 1U, std::memory_order_release);
        }
        shortKeyCount_.fetch_add(1U, std::memory_order_release);
        notifyWake();
    }

    /**
     * @brief 按生产顺序弹出一条未消费的消抖短按时间戳
     * @param[out] eventMsOut: 弹出的事件产生时刻（ms）
     * @retval true 成功弹出一条事件
     * @retval false 队列为空
     * @note 仅 OLED 任务单消费者调用；无锁 SPSC 读，不阻塞，非 ISR。
     */
    [[nodiscard]] bool popShortKey(uint32_t &eventMsOut) noexcept
    {
        const uint32_t readIdx = shortKeyRead_.load(std::memory_order_relaxed);
        const uint32_t writeIdx = shortKeyWrite_.load(std::memory_order_acquire);
        if (readIdx == writeIdx) {
            return false;
        }
        eventMsOut = shortKeyTimes_[readIdx & kShortKeyQueueMask];
        shortKeyRead_.store(readIdx + 1U, std::memory_order_release);
        return true;
    }

    /**
     * @brief 清空当前队列中全部未消费的短按时间戳（用于 OLED 初始化丢弃启动期残余事件）
     * @note 仅 OLED 单消费者调用，不阻塞，非 ISR。
     */
    void drainShortKeys() noexcept
    {
        const uint32_t writeIdx = shortKeyWrite_.load(std::memory_order_acquire);
        shortKeyRead_.store(writeIdx, std::memory_order_release);
    }

    /**
     * @brief 唤醒 UI 任务以尽快响应关机等状态变化
     * @note 任意任务可调用；不阻塞、不分配堆，非 ISR。
     */
    void notifyWake() noexcept
    {
        if (const WakeFn fn = wakeFn_.load(std::memory_order_acquire); fn != nullptr) {
            fn(wakeCtx_);
        }
    }

    /**
     * @brief 读取累计短按事件序号
     * @retval 自启动以来的短按累计次数
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] uint32_t shortKeyCount() const noexcept
    {
        return shortKeyCount_.load(std::memory_order_acquire);
    }

private:
    static_assert((kShortKeyQueueCap & kShortKeyQueueMask) == 0U, "短按队列容量须为 2 的幂");
    static_assert(fw::kAtomicUsable<uint32_t>, "UI 事件原子须无锁");
    static_assert(fw::kAtomicUsable<WakeFn>, "UI 唤醒钩子指针原子须无锁");

    std::array<uint32_t, kShortKeyQueueCap> shortKeyTimes_{};
    std::atomic<uint32_t> shortKeyWrite_{0U};
    std::atomic<uint32_t> shortKeyRead_{0U};
    std::atomic<uint32_t> shortKeyCount_{0U};
    std::atomic<uint32_t> flags_{0U};
    std::atomic<WakeFn> wakeFn_{nullptr};
    void *wakeCtx_ = nullptr;
    std::atomic<WakeFn> restartWakeFn_{nullptr};
    void *restartWakeCtx_ = nullptr;
};

} // namespace app
