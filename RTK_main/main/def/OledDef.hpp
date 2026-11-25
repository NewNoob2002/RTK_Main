// OLED 显示模块跨模块共享类型与轻量 UI 事件总线（ADR-0020 / OL2）。
// 纯值类型与无锁原子事件通道，无 IDF/硬件依赖；notify* 可在任意任务调用，不阻塞、非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "board/PowerLogic.hpp"
#include "fw/Atomic.hpp"
#include "fw/Status.hpp"
namespace app {

/** OLED 当前显示页面。 */
enum class OledPage : uint8_t {
    kSystemCheck = 0,
    kDialplate,
    kSubSat,
    kSubPos,
    kSubComm,
    kSubDevice,
    kSubPower,
    kChargeStandby,
    kShutdown,
};

/**
 * @brief 取得 OLED 页面枚举的简短诊断名
 * @param page: OLED 页面枚举值
 * @retval 供诊断和主机仿真使用的静态短名
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
    case OledPage::kSubComm:
        return "comm";
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

/** OLED 初始化阶段（供失败诊断与 `oled status` 输出）。 */
enum class OledInitStage : uint8_t {
    kNone = 0,
    kDisabled,
    kStorage,
    kPowerPin,
    kI2cBus,
    kProbe,
    kAddDevice,
    kConfigure,
    kOk,
};

/**
 * @brief 取得 OLED 初始化阶段的可读短名
 * @param stage: 初始化阶段枚举
 * @retval 静态短名
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr const char *oledInitStageName(OledInitStage stage) noexcept
{
    switch (stage) {
    case OledInitStage::kNone:
        return "none";
    case OledInitStage::kDisabled:
        return "disabled";
    case OledInitStage::kStorage:
        return "storage";
    case OledInitStage::kPowerPin:
        return "power_pin";
    case OledInitStage::kI2cBus:
        return "i2c_bus";
    case OledInitStage::kProbe:
        return "probe";
    case OledInitStage::kAddDevice:
        return "add_device";
    case OledInitStage::kConfigure:
        return "configure";
    case OledInitStage::kOk:
        return "ok";
    default:
        return "unknown";
    }
}

/** 软件重启前 OLED 过渡提示类型（由重启方发起，OLED 刷出后静默 I2C 并回执）。 */
enum class RebootBanner : uint8_t {
    kNone = 0,
    kKeepFrame,     ///< 保持当前终帧画面，仅静默后续 I2C 传输
    kRebooting,     ///< 通用软件重启（REBOOTING...）
    kSwitchingWifi, ///< 切换至 Wi-Fi 配置模式（SWITCHING WI-FI）
    kSwitchingBt,   ///< 切换至蓝牙模式（SWITCHING BT）
    kStarting,      ///< 充电待机开机重启（STARTING）
};

/** OLED 状态快照（oled 任务写，console `oled` 命令读）。 */
struct OledStatus {
    bool enabled = false;    // 板型具备 Feature::kOled
    bool present = false;    // SH1106 探测成功并完成初始化
    bool poweredOff = false; // 面板及 GPIO12 已断电
    OledPage page = OledPage::kSystemCheck;
    uint16_t lastTxBytes = 0U;     // 最近一轮成功传输的 I2C 字节数
    uint8_t lastDirtyPages = 0U;   // 最近一次写出帧的脏页数（0..8）
    uint32_t framesFlushed = 0U;   // 实际写出到 I2C 的帧数
    uint32_t framesSkipped = 0U;   // 内容未变跳过的空闲帧数
    uint32_t totalDirtyPages = 0U; // 累计写出的脏页总数
    uint32_t lastI2cUs = 0U;       // 最近一次写出帧的 I2C 耗时（us）
    uint32_t maxI2cUs = 0U;        // 历史最大单帧 I2C 耗时（us）
    uint32_t lastFrameUs = 0U;     // 最近一次写出帧的渲染+I2C 总耗时（us）
    uint32_t maxFrameUs = 0U;      // 历史最大单帧总耗时（us）
    uint32_t lastRenderUs = 0U;    // 最近一次渲染墙钟时间（µs）
    uint32_t maxRenderUs = 0U;
    uint32_t lastIntervalMs = 0U; // 同一活动段内成功写屏的起点间隔
    uint32_t maxIntervalMs = 0U;
    uint32_t i2cErrors = 0U;                        // 累计 I2C 传输失败次数
    OledInitStage initStage = OledInitStage::kNone; ///< 最近一次初始化所处阶段
    fw::Status initStatus = fw::Status::kOk;        ///< 最近一次初始化返回状态码
    uint32_t initAttempts = 0U;                     ///< 累计初始化尝试次数（含后台重试）

    /** @brief 比较两份 OLED 状态快照。 */
    [[nodiscard]] bool operator==(const OledStatus &) const noexcept = default;
};

/** UiEventBus 原子状态标志位（CODING-STYLE 规则 3）。 */
enum class UiBusFlag : uint8_t {
    kOledActive = 1U << 0U,           ///< OLED 刷新任务已启动在线
    kShutdownFinalFlushed = 1U << 1U, ///< USB 关机终帧（已关机/充电中）已刷出到 I2C
    kBatteryFinalFlushed = 1U << 2U,  ///< 电池关机终帧已刷出
};

/** kLink 发布的蓝牙确认请求和窗口截止。id 为零时没有确认请求。 */
struct OledPairingStatus {
    static constexpr size_t kNameBytes = 17U;
    static constexpr size_t kSuffixBytes = 3U;

    uint32_t windowDeadlineMs = 0U;
    uint32_t requestDeadlineMs = 0U;
    uint32_t id = 0U;
    uint32_t number = 0U;
    std::array<char, kNameBytes> name{};
    std::array<uint8_t, kSuffixBytes> suffix{};
    bool windowOpen = false;
    bool hasNumber = false;

    /** @brief 判断新快照是否需要刷新。 */
    [[nodiscard]] bool operator==(const OledPairingStatus &) const noexcept = default;
};

/** 由 OLED 提交给 kLink 的动作。 */
enum class OledAction : uint8_t {
    kNone,
    kWifi,
    kBluetooth,
    kTogglePair,
    kPairAccept,
    kPairReject,
    kShutdownKey,
};

struct OledActionRequest {
    OledAction action = OledAction::kNone;
    uint32_t pairId = 0U;
};

/** PowerModule 暴露给 OLED 的按键长按与待机唤醒进度只读快照（无锁原子更新）。 */
struct KeyHoldSnapshot {
    bool active = false;                               ///< 消抖后按键正处于有效按住计数态
    bool standbyConfirm = false;                       ///< 充电待机长按已到达，正在确认 U3 保持
    uint32_t startMs = 0U;                             ///< 物理按下沿单调时刻（ms）
    uint32_t thresholdMs = board::kDefaultLongPressMs; ///< 长按触发阈值（ms）

    /** @brief 比较两份按键保持快照是否一致。 */
    [[nodiscard]] bool operator==(const KeyHoldSnapshot &) const noexcept = default;
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
    static constexpr size_t kLongKeyQueueCap = 4U; // SPSC 长按到达时间戳队列容量（2 的幂）
    static constexpr size_t kLongKeyQueueMask = kLongKeyQueueCap - 1U;

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
     * @brief 清除关机阶段和终帧的刷出凭据
     * @note 仅 PowerModule 调用；无锁原子写，不阻塞，非 ISR。
     */
    void resetShutdownFinalFrame() noexcept
    {
        static_cast<void>(
            flags_.fetch_and(uiBusFlagBit(UiBusFlag::kOledActive), std::memory_order_release));
    }

    /**
     * @brief 标记一次完整的中间阶段帧已刷出
     * @param stage: 已发送的快照阶段
     * @note OLED 任务调用，不阻塞。
     */
    void markShutdownStageFlushed(board::ShutdownStage stage) noexcept
    {
        static_cast<void>(flags_.fetch_or(shutdownStageFlag(stage), std::memory_order_release));
    }

    /**
     * @brief 查询该中间阶段是否已显示过完整帧
     * @param stage: 待检查的关机阶段
     * @retval 是否已刷出
     * @note PowerModule 调用，不阻塞。
     */
    [[nodiscard]] bool shutdownStageFlushed(board::ShutdownStage stage) const noexcept
    {
        return (flags_.load(std::memory_order_acquire) & shutdownStageFlag(stage)) != 0U;
    }

    /**
     * @brief 标记关机终帧已刷出到 SH1106，USB 路径唤醒等待重启的任务
     * @param usb: true 为 USB 重启终帧，false 为电池断电终帧
     * @note 仅 OLED 刷新任务在 I2C 刷出成功后调用；无锁原子写，不阻塞，非 ISR。
     */
    void markShutdownFinalFrameFlushed(bool usb = true) noexcept
    {
        const auto flag = usb ? UiBusFlag::kShutdownFinalFlushed : UiBusFlag::kBatteryFinalFlushed;
        static_cast<void>(flags_.fetch_or(uiBusFlagBit(flag), std::memory_order_release));
        if (usb) {
            if (const WakeFn fn = restartWakeFn_.load(std::memory_order_acquire); fn != nullptr) {
                fn(restartWakeCtx_);
            }
        }
    }

    /**
     * @brief 查询对应的关机终帧是否已刷出到面板
     * @param usb: true 为 USB 重启终帧，false 为电池断电终帧
     * @retval true 已刷出
     * @retval false 尚未刷出
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] bool shutdownFinalFrameFlushed(bool usb = true) const noexcept
    {
        const auto flag = usb ? UiBusFlag::kShutdownFinalFlushed : UiBusFlag::kBatteryFinalFlushed;
        return (flags_.load(std::memory_order_acquire) & uiBusFlagBit(flag)) != 0U;
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
     * @brief 投递一次长按阈值到达事件（附带产生时刻单调时间戳）并唤醒 UI 任务
     * @param eventMs: 长按到达时刻（ms）
     * @note 仅 kLink 的 PowerModule 单生产者调用；无锁 SPSC 队列，不阻塞、不分配堆，非 ISR。
     */
    void notifyLongKey(uint32_t eventMs = 0U) noexcept
    {
        const uint32_t writeIdx = longKeyWrite_.load(std::memory_order_relaxed);
        const uint32_t readIdx = longKeyRead_.load(std::memory_order_acquire);
        if ((writeIdx - readIdx) < kLongKeyQueueCap) {
            longKeyTimes_[writeIdx & kLongKeyQueueMask] = eventMs;
            longKeyWrite_.store(writeIdx + 1U, std::memory_order_release);
        }
        longKeyCount_.fetch_add(1U, std::memory_order_release);
        notifyWake();
    }

    /**
     * @brief 按生产顺序弹出一条未消费的长按到达时间戳
     * @param[out] eventMsOut: 弹出的事件产生时刻（ms）
     * @retval true 成功弹出一条事件
     * @retval false 队列为空
     * @note 仅 OLED 任务单消费者调用；无锁 SPSC 读，不阻塞，非 ISR。
     */
    [[nodiscard]] bool popLongKey(uint32_t &eventMsOut) noexcept
    {
        const uint32_t readIdx = longKeyRead_.load(std::memory_order_relaxed);
        const uint32_t writeIdx = longKeyWrite_.load(std::memory_order_acquire);
        if (readIdx == writeIdx) {
            return false;
        }
        eventMsOut = longKeyTimes_[readIdx & kLongKeyQueueMask];
        longKeyRead_.store(readIdx + 1U, std::memory_order_release);
        return true;
    }

    /**
     * @brief 清空当前队列中全部未消费的长按到达时间戳（用于 OLED 初始化丢弃启动期残余事件）
     * @note 仅 OLED 单消费者调用，不阻塞，非 ISR。
     */
    void drainLongKeys() noexcept
    {
        const uint32_t writeIdx = longKeyWrite_.load(std::memory_order_acquire);
        longKeyRead_.store(writeIdx, std::memory_order_release);
    }

    /**
     * @brief 读取累计长按到达事件序号
     * @retval 自启动以来的长按到达累计次数
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] uint32_t longKeyCount() const noexcept
    {
        return longKeyCount_.load(std::memory_order_acquire);
    }

    /**
     * @brief 更新按键保持中、按下起点与长按阈值快照，状态边沿变化时唤醒 OLED 任务
     * @param active: 消抖后按键是否正处于按住计数态
     * @param startMs: 物理按下沿单调起点（ms）
     * @param thresholdMs: 长按触发阈值（ms）
     * @param standbyConfirm: 充电待机长按已到达、正在确认 U3 保持
     * @note 仅 kLink 的 PowerModule 单生产者调用；无锁原子写，不阻塞、不分配堆，非 ISR。
     */
    void updateKeyHold(bool active, uint32_t startMs, uint32_t thresholdMs,
                       bool standbyConfirm) noexcept
    {
        const uint32_t nextFlags =
            (active ? kKeyHoldActiveBit : 0U) | (standbyConfirm ? kKeyHoldStandbyBit : 0U);
        const uint32_t prevFlags = keyHoldFlags_.load(std::memory_order_relaxed);
        const uint32_t prevStart = keyHoldStartMs_.load(std::memory_order_relaxed);
        const uint32_t prevThreshold = keyHoldThresholdMs_.load(std::memory_order_relaxed);
        if ((nextFlags == prevFlags) && (startMs == prevStart) && (thresholdMs == prevThreshold)) {
            return;
        }
        const uint32_t seq = keyHoldSeq_.load(std::memory_order_relaxed);
        keyHoldSeq_.store(seq + 1U, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        keyHoldStartMs_.store(startMs, std::memory_order_relaxed);
        keyHoldThresholdMs_.store(thresholdMs, std::memory_order_relaxed);
        keyHoldFlags_.store(nextFlags, std::memory_order_relaxed);
        keyHoldSeq_.store(seq + 2U, std::memory_order_release);
        if (nextFlags != prevFlags) {
            notifyWake();
        }
    }

    /**
     * @brief 无锁读取按键按住进度快照（一致拷贝，不阻塞）
     * @retval 按键按住与待机保持快照
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] KeyHoldSnapshot readKeyHold() const noexcept
    {
        constexpr unsigned kMaxRetries = 4U;
        KeyHoldSnapshot out{};
        for (unsigned attempt = 0U; attempt < kMaxRetries; ++attempt) {
            const uint32_t seq1 = keyHoldSeq_.load(std::memory_order_acquire);
            if ((seq1 & 1U) != 0U) {
                continue;
            }
            const uint32_t flags = keyHoldFlags_.load(std::memory_order_relaxed);
            const uint32_t startMs = keyHoldStartMs_.load(std::memory_order_relaxed);
            const uint32_t thresholdMs = keyHoldThresholdMs_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t seq2 = keyHoldSeq_.load(std::memory_order_relaxed);
            if (seq1 == seq2) {
                out.active = (flags & kKeyHoldActiveBit) != 0U;
                out.standbyConfirm = (flags & kKeyHoldStandbyBit) != 0U;
                out.startMs = startMs;
                out.thresholdMs = (thresholdMs == 0U) ? board::kDefaultLongPressMs : thresholdMs;
                return out;
            }
        }
        return out;
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

    /**
     * @brief 把用户确认的动作交给 kLink。
     * @param request: 动作及请求序号。
     * @retval 是否成功入队。
     * @note 仅 OLED 生产者调用，不阻塞。
     */
    [[nodiscard]] bool submitAction(OledActionRequest request) noexcept
    {
        if (actionPending_.load(std::memory_order_acquire)) {
            return false;
        }
        action_ = request;
        actionPending_.store(true, std::memory_order_release);
        return true;
    }

    /** @brief 取走 OLED 动作。 @retval 无动作时返回空值。 @note 仅 kLink 消费者调用。 */
    [[nodiscard]] OledActionRequest takeAction() noexcept
    {
        if (!actionPending_.load(std::memory_order_acquire)) {
            return {};
        }
        const auto request = action_;
        actionPending_.store(false, std::memory_order_release);
        return request;
    }

    /**
     * @brief 请求 OLED 显示软件重启/射频切换过渡提示并静默 I2C，同时唤醒 OLED 任务
     * @param banner: 过渡提示类型；kNone 表示清除
     * @note 任意任务可调用，无锁原子写，不阻塞、不分配堆，非 ISR。
     */
    void requestRebootBanner(RebootBanner banner) noexcept
    {
        rebootQuiesced_.store(false, std::memory_order_release);
        rebootBanner_.store(static_cast<uint8_t>(banner), std::memory_order_release);
        if (banner != RebootBanner::kNone) {
            notifyWake();
        }
    }

    /**
     * @brief 读取当前待执行的软件重启过渡提示类型
     * @retval 当前过渡提示枚举，非重启态为 kNone
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] RebootBanner rebootBanner() const noexcept
    {
        const uint8_t raw = rebootBanner_.load(std::memory_order_acquire);
        if (raw > static_cast<uint8_t>(RebootBanner::kStarting)) {
            return RebootBanner::kNone;
        }
        return static_cast<RebootBanner>(raw);
    }

    /**
     * @brief 标记 OLED 已刷出重启过渡帧并停止后续 I2C 传输，同时唤醒等待重启的任务
     * @note 仅 OLED 刷新任务调用；无锁原子写，不阻塞，非 ISR。
     */
    void markRebootQuiesced() noexcept
    {
        rebootQuiesced_.store(true, std::memory_order_release);
        if (const WakeFn fn = restartWakeFn_.load(std::memory_order_acquire); fn != nullptr) {
            fn(restartWakeCtx_);
        }
    }

    /**
     * @brief 查询 OLED 是否已完成重启过渡帧刷出并静默 I2C
     * @retval true 已静默，可安全执行 esp_restart
     * @retval false 尚未完成
     * @note 任意任务可调用，无锁原子读，不阻塞。
     */
    [[nodiscard]] bool rebootQuiesced() const noexcept
    {
        return rebootQuiesced_.load(std::memory_order_acquire);
    }

    /**
     * @brief 清除重启过渡提示与静默凭据（供主机测试复用实例）
     * @note 非 ISR，不阻塞。
     */
    void clearRebootBanner() noexcept
    {
        rebootBanner_.store(0U, std::memory_order_release);
        rebootQuiesced_.store(false, std::memory_order_release);
    }

private:
    /**
     * @brief 映射关机阶段到 UiBusFlag 之后的独立位
     * @param stage: 关机阶段枚举
     * @retval 对应掩码
     */
    [[nodiscard]] static constexpr uint32_t shutdownStageFlag(board::ShutdownStage stage) noexcept
    {
        return 8U << static_cast<uint8_t>(stage);
    }
    static_assert((kShortKeyQueueCap & kShortKeyQueueMask) == 0U, "短按队列容量须为 2 的幂");
    static constexpr uint32_t kKeyHoldActiveBit = 1U << 0U;
    static constexpr uint32_t kKeyHoldStandbyBit = 1U << 1U;
    static_assert((kLongKeyQueueCap & kLongKeyQueueMask) == 0U, "长按队列容量须为 2 的幂");
    static_assert(fw::kAtomicUsable<uint32_t>, "UI 事件原子须无锁");
    static_assert(fw::kAtomicUsable<WakeFn>, "UI 唤醒钩子指针原子须无锁");

    std::array<uint32_t, kShortKeyQueueCap> shortKeyTimes_{};
    std::atomic<uint32_t> shortKeyWrite_{0U};
    std::atomic<uint32_t> shortKeyRead_{0U};
    std::atomic<uint32_t> shortKeyCount_{0U};
    std::array<uint32_t, kLongKeyQueueCap> longKeyTimes_{};
    std::atomic<uint32_t> longKeyWrite_{0U};
    std::atomic<uint32_t> longKeyRead_{0U};
    std::atomic<uint32_t> longKeyCount_{0U};
    std::atomic<uint32_t> keyHoldSeq_{0U};
    std::atomic<uint32_t> keyHoldStartMs_{0U};
    std::atomic<uint32_t> keyHoldThresholdMs_{0U};
    std::atomic<uint32_t> keyHoldFlags_{0U};
    std::atomic<uint32_t> flags_{0U};
    std::atomic<WakeFn> wakeFn_{nullptr};
    void *wakeCtx_ = nullptr;
    std::atomic<WakeFn> restartWakeFn_{nullptr};
    void *restartWakeCtx_ = nullptr;
    OledActionRequest action_{};
    std::atomic<bool> actionPending_{false};
    std::atomic<uint8_t> rebootBanner_{0U};
    std::atomic<bool> rebootQuiesced_{false};
};

} // namespace app
