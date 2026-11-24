// OLED 显示模块跨模块共享类型与轻量 UI 事件总线（ADR-0020）。
// 纯值类型与无锁原子事件通道，无 IDF/硬件依赖；notify* 可在任意任务调用，不阻塞、非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <atomic>
#include <cstdint>

#include "fw/Atomic.hpp"

namespace app {

/** OLED 当前显示页面（OL1 只读页面集合）。 */
enum class OledPage : uint8_t {
    kSplash = 0,
    kStatus,
    kDeviceInfo,
    kChargeStandby,
    kShutdown,
};

/**
 * @brief 取得 OLED 页面枚举的简短诊断名
 * @param page: OLED 页面枚举值
 * @retval 静态短名（splash/status/device/charge_standby/shutdown）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr const char *oledPageName(OledPage page) noexcept
{
    switch (page) {
    case OledPage::kSplash:
        return "splash";
    case OledPage::kStatus:
        return "status";
    case OledPage::kDeviceInfo:
        return "device";
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
    bool poweredOff = false; // 面板及 GPIO12 已断电（关机路径）
    OledPage page = OledPage::kSplash;
    uint32_t framesFlushed = 0U; // 实际写出到 I2C 的帧数
    uint32_t framesSkipped = 0U; // 内容未变跳过的空闲帧数
    uint32_t i2cErrors = 0U;     // 累计 I2C 传输失败次数
};

/**
 * 解耦按键/关机事件与 OLED 渲染任务的轻量通知通道。
 * PowerModule 只写原子计数并调用可选唤醒钩子，不在 kLink 内直接画屏或访问 I2C。
 */
class UiEventBus final {
public:
    using WakeFn = void (*)(void *ctx) noexcept;

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
     * @brief 投递一次短按事件并唤醒 UI 任务（如有绑定）
     * @note 由 kLink 的 PowerModule 调用；不阻塞、不分配堆，非 ISR。
     */
    void notifyShortKey() noexcept
    {
        shortKeyCount_.fetch_add(1U, std::memory_order_release);
        notifyWake();
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
    static_assert(fw::kAtomicUsable<uint32_t>, "UI 短按计数原子须无锁");
    static_assert(fw::kAtomicUsable<WakeFn>, "UI 唤醒钩子指针原子须无锁");

    std::atomic<uint32_t> shortKeyCount_{0U};
    std::atomic<WakeFn> wakeFn_{nullptr};
    void *wakeCtx_ = nullptr;
};

} // namespace app
