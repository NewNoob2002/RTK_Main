// P2 OLED 显示模块（ADR-0020 / OL2）：管理 SH1106 与低优先级静态刷新任务（Core 1, prio 1）。
// init 仅启动任务调用；start 在执行器启动阶段创建低优先级刷新任务；动画期 30 fps，静止 0 写，非 ISR。
// 堆档位：启动期一次性（任务栈与 1 KB 帧缓冲由 BootArena 仅在含 Feature::kOled 板型上外供；成员全零落 .bss）。
#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <span>

#include "def/OledDef.hpp"
#include "fw/Result.hpp"
#include "fw/Status.hpp"
#include "modules/oled/OledPerf.hpp"
#include "rtos/BootTask.hpp"

namespace app {
struct AppContext;
struct ModuleTestAccess;
namespace oled {
struct ViewModel;
}

/** OLED 显示模块：仅在板型含 Feature::kOled 且 0x3C 应答时创建低优先级刷新任务。 */
class OledModule final {
    friend struct ModuleTestAccess;

public:
    /** 启动期外供存储视图（OLED 刷新任务栈 + 1 KB 帧缓冲，由 BootArena 仅在含 Feature::kOled 板型分配）。 */
    struct StorageView {
        std::span<StackType_t> taskStack{};
        std::span<uint8_t> frameBuffer{};
    };

    static constexpr size_t kTaskStackBytes = 3072U;   // B，静态任务栈（与 trace_out 一致）
    static constexpr size_t kFrameBufferBytes = 1024U; // B，8×128 单色帧缓冲（sizeof(ScreenBuff)）
    static constexpr uint32_t kRetryPeriodMs = 200U;   // ms，运行期刷屏失败后的重试间隔
    static constexpr uint32_t kMaxBenchSeconds = 60U;
    static constexpr uint8_t kOfflineFlushErrors =
        4U; // 连续 I2C 刷屏失败达此阈值判定面板离线并清除 kOledActive
    static constexpr uint32_t kInitRetryPeriodMs =
        2000U; // ms，启动探测失败后后台重试 SH1106 初始化的周期

    constexpr OledModule() noexcept = default;
    ~OledModule() = default;
    OledModule(const OledModule &) = delete;
    OledModule &operator=(const OledModule &) = delete;
    OledModule(OledModule &&) = delete;
    OledModule &operator=(OledModule &&) = delete;

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
     * @brief 启动前单线程：绑定 BootArena 外供的 OLED 任务栈与 1 KB 帧缓冲
     * @param storage: 外供存储视图，生命周期覆盖固件运行期
     * @retval fw::Status::kOk 绑定成功
     * @retval fw::Status::kInvalidState 模块已初始化在线或任务已运行
     * @retval fw::Status::kInvalidArg 任一缓冲为空或容量不足
     * @note 仅启动任务在 bindBootBuffers 阶段调用，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Status bindStorage(const StorageView &storage) noexcept
    {
        if (task_.running() || present_.load(std::memory_order_acquire)) {
            return fw::Status::kInvalidState;
        }
        if ((storage.taskStack.size_bytes() < kTaskStackBytes) ||
            (storage.frameBuffer.size_bytes() < kFrameBufferBytes)) {
            return fw::Status::kInvalidArg;
        }
        storage_ = storage;
        storageBound_ = true;
        return fw::Status::kOk;
    }

    /**
     * @brief 仅启动绑定回滚、消费者尚未启动时调用；清除外供存储引用
     * @note 仅启动任务回滚阶段调用，不阻塞，非 ISR。
     */
    void unbindStorage() noexcept
    {
        storage_ = {};
        storageBound_ = false;
    }

    /**
     * @brief 检查 Feature::kOled、初始化 SH1106 并绘出首帧（面板离线时仅降级禁用，不阻塞启动）
     * @retval fw::Result::kOk 完成初始化或本板禁用/离线降级
     * @retval fw::Result::kInvalidState 上下文未绑定
     * @note 仅启动任务调用；含 100 ms 上电稳定延时，非 ISR。
     */
    [[nodiscard]] fw::Result init() noexcept;

    /**
     * @brief 面板在线时创建低优先级静态刷新任务并注册 UI 唤醒钩子
     * @retval fw::Result::kOk 任务已启动或面板未启用
     * @retval fw::Result::kBusy 任务已在运行
     * @retval fw::Result::kNoSpace 静态任务创建失败
     * @note 仅执行器启动阶段调用，非 ISR。
     */
    [[nodiscard]] fw::Result start() noexcept;

    /** @brief 在 kLink 发布配对快照并执行 OLED 动作。 @param nowMs: 当前单调时间。 @retval 无积压。 */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;

    /**
     * @brief 查询低优先级 OLED 刷新任务是否已创建
     * @retval true 已创建
     * @retval false 未创建（板型无 OLED 或面板未应答）
     * @note 仅组合根启动屏障阶段读取，不阻塞，非 ISR。
     */
    [[nodiscard]] bool taskRunning() const noexcept
    {
        return task_.running();
    }

    /** @brief 读取滚动性能窗口。 @retval 最近 2 秒统计。 @note 可跨任务调用，非 ISR。 */
    [[nodiscard]] oled::PerfTotals recentPerf() const noexcept;

    /**
     * @brief 通知 OLED 任务执行一次自动转场测试。
     * @param seconds: 测试秒数（1..60）。
     * @retval 请求提交结果，忙时保持原任务。
     * @note 仅 console 调用，非 ISR，不阻塞。
     */
    [[nodiscard]] fw::Status requestBench(uint32_t seconds) noexcept;

    /**
     * @brief 读取已完成的 bench 结果。
     * @param[out] result: 已完成测试的统计。
     * @retval false 测试仍在进行，true 已取得结果。
     * @note 仅已成功提交请求的 console 调用，非 ISR，不阻塞。
     */
    [[nodiscard]] bool benchResult(oled::BenchStats &result) const noexcept;

private:
    /**
     * @brief 记录一轮 OLED 任务工作，分离同步总线等待。
     * @param nowMs: 本轮起始时刻。
     * @param startUs: 本轮开始的微秒时间戳。
     * @param written: 是否成功写出一帧。
     * @param woke: 是否从任务等待中返回。
     * @param rendered: 是否进入实际渲染路径。
     */
    void recordCycle(uint32_t nowMs, int64_t startUs, bool written, bool woke,
                     bool rendered) noexcept;

    /**
     * @brief 从只读应用快照收集 UI 视图模型。
     * @param nowMs: 当前单调时间（ms）。
     * @param[out] vm: 目标视图模型。
     * @note 仅启动或 OLED 任务调用，非 ISR。禁止内联以释放临时快照栈帧。
     */
    void collectViewModel(uint32_t nowMs, oled::ViewModel &vm) const noexcept;

    /**
     * @brief 在唯一的帧缓冲绘制当前页面或 bench 转场。
     * @param nowMs: 当前时刻。
     * @param vm: 本帧输入模型，可补录当前页所需卫星信息。
     * @note OLED 任务或启动任务串行调用。
     */
    void drawFrame(uint32_t nowMs, oled::ViewModel &vm) noexcept;

    /** @brief 在 kLink 汇合状态槽变更，限制普通数据唤醒至 5 Hz。 @param nowMs: 当前时间。 */
    void notifyDisplayData(uint32_t nowMs) noexcept;

    /**
     * @brief 推进 bench 请求与结束回执。
     * @param nowMs: 当前时刻。
     * @retval 是否需要绘制 bench 转场。
     * @note 仅 OLED 任务，关机或软件重启优先。
     */
    [[nodiscard]] bool updateBench(uint32_t nowMs) noexcept;

    /** @brief 发布当前配对窗口和待确认请求。 @param nowMs: 当前单调时间。 @note 仅 kLink。 */
    void publishPairing(uint32_t nowMs) noexcept;

    /** @brief 分派确认后的动作。 @param request: UI 请求。 @param nowMs: 当前单调时间。 @note 仅 kLink。 */
    void applyAction(OledActionRequest request, uint32_t nowMs) noexcept;
    /**
     * @brief OLED 静态任务入口函数
     * @param arg: OledModule 实例指针
     * @note 仅 oled 任务执行，等待启动屏障后进入主循环，非 ISR。
     */
    static void taskEntry(void *arg) noexcept;

    /**
     * @brief 供 UiEventBus 调用的非阻塞任务唤醒钩子
     * @param ctx: OledModule 实例指针
     * @note 任意任务可调用（如 kLink），不阻塞，非 ISR。
     */
    static void wakeFromBus(void *ctx) noexcept;

    /**
     * @brief OLED 低优先级刷新任务主循环
     * @note 仅 oled 任务执行，活动期绝对节拍，静止期等待事件，非 ISR。
     */
    void run() noexcept;

    /**
     * @brief 采集只读快照、推进状态机、经 WouoUI 渲染并按脏页刷屏，同时记录性能统计
     * @param nowMs: 当前单调时间（ms）
     * @param forceFull: true 时强制全 8 页写出
     * @param renderFrame: true 时允许在本轮节拍槽内绘制。
     * @retval 下一轮刷新等待时间（ms）
     * @note 仅 init 或 oled 任务串行调用，非 ISR。
     */
    [[nodiscard]] uint32_t refreshOnce(uint32_t nowMs, bool forceFull,
                                       bool renderFrame = true) noexcept;

    /**
     * @brief 尝试初始化 SH1106 并记录阶段/状态与尝试次数，成功时初始化 WouoUI 与状态机
     * @param nowMs: 当前单调时间（ms）
     * @param initialBoot: true 表示启动期首次尝试，false 表示后台周期重试
     * @retval true 面板初始化成功并就绪
     * @retval false 初始化失败，已更新诊断状态
     * @note 仅 init 或 oled 任务调用，非 ISR。
     */
    [[nodiscard]] bool tryInitPanel(uint32_t nowMs, bool initialBoot) noexcept;

    /**
     * @brief 面板未就绪时按周期在后台重试初始化（不阻塞启动与其他业务任务）
     * @param nowMs: 当前单调时间（ms）
     * @retval 下一轮等待时间（ms）
     * @note 仅 oled 任务调用，非 ISR。
     */
    [[nodiscard]] uint32_t handleBackgroundInitRetry(uint32_t nowMs) noexcept;

    /**
     * @brief 响应软件重启过渡提示：刷出过渡帧（或保持当前终帧）后静默 I2C 并置位回执
     * @param nowMs: 当前单调时间（ms）
     * @param banner: 非 kNone 的过渡提示类型
     * @retval 下一轮等待时间（ms）
     * @note 仅 oled 任务调用，非 ISR。
     */
    [[nodiscard]] uint32_t handleRebootQuiesce(uint32_t nowMs, RebootBanner banner) noexcept;

    /**
     * @brief 记入本次刷屏结果、耗时与 FPS 采样。
     * @param nowMs: 本轮刷新起始的单调时间（ms）。
     * @param frameUs: 采集快照到发送完成的耗时（µs）。
     * @param i2cUs: 发送耗时（µs）。
     */
    void updateFrameStats(uint32_t nowMs, uint32_t frameUs, int64_t i2cUs) noexcept;
    /**
     * @brief 依据最近一次刷屏结果维护面板在线状态，连续 I2C 失败进入离线时清除 kOledActive
     * @note 仅 init 或 oled 任务调用，不阻塞，非 ISR。
     */
    void updatePanelOnlineState() noexcept;

    /**
     * @brief 在状态变化时发布 OLED 诊断快照到 DataCenter
     * @param force: true 强制写入
     * @note 仅 init 或 oled 任务调用，非 ISR。
     */
    void publishStatus(bool force) noexcept;

    AppContext *context_ = nullptr;
    StorageView storage_{};
    bool storageBound_ = false;
    rtos::BootTask task_{};
    bool enabled_ = false;
    std::atomic<bool> present_{false};
    uint8_t consecutiveFlushErrors_ = 0U;
    bool panelOffline_ = false;
    bool wasOledActive_ = false;
    bool rebootQuiesced_ = false;
    OledInitStage initStage_ = OledInitStage::kNone;
    fw::Status initStatus_ = fw::Status::kOk;
    uint32_t initAttempts_ = 0U;
    uint32_t lastInitAttemptMs_ = 0U;
    uint32_t framesFlushed_ = 0U;
    uint32_t framesSkipped_ = 0U;
    uint32_t lastI2cUs_ = 0U;
    uint32_t maxI2cUs_ = 0U;
    uint32_t lastFrameUs_ = 0U;
    uint32_t maxFrameUs_ = 0U;
    uint32_t lastRenderUs_ = 0U;
    uint32_t maxRenderUs_ = 0U;
    uint32_t lastWriteMs_ = 0U;
    uint32_t lastIntervalMs_ = 0U;
    uint32_t maxIntervalMs_ = 0U;
    uint32_t sampleIoUs_ = 0U;
    bool animating_ = false;
    bool statusBarValid_ = false;
    uint32_t lastDataCheckMs_ = 0U;
    struct DisplayVersions {
        uint32_t power = 0U;
        uint32_t system = 0U;
        uint32_t bluetooth = 0U;
        uint32_t gnssFix = 0U;
        uint32_t gnssSats = 0U;
        uint32_t gnssMode = 0U;
        uint32_t gnssState = 0U;
        uint32_t wifi = 0U;

        /** @brief 比较 OLED 数据源的版本和显示状态。 @retval 是否一致。 */
        [[nodiscard]] bool operator==(const DisplayVersions &) const noexcept = default;
    } displayVersions_{};
    oled::PerfWindow perf_{};
    std::atomic<uint32_t> benchDurationMs_{0U};
    oled::BenchStats bench_{};
    uint32_t benchStartMs_ = 0U;
    bool benchActive_ = false;
    OledStatus lastPublished_{};
};

} // namespace app
