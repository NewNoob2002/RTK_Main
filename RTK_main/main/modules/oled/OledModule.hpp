// P2 OLED 显示模块（ADR-0020）：管理 SH1106 与低优先级静态刷新任务（Core 1, prio 1, <= 5 Hz）。
// init 仅启动任务调用；start 在执行器启动阶段创建低优先级刷新任务；不阻塞 kLink 与按键/充电读数，非 ISR。
// 堆档位：启动期一次性（任务栈与 1 KB 帧缓冲由 BootArena 仅在含 Feature::kOled 板型上外供；成员全零落 .bss）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "def/OledDef.hpp"
#include "fw/Result.hpp"
#include "fw/Status.hpp"
#include "rtos/BootTask.hpp"

namespace app {
struct AppContext;

/** OLED 显示模块：仅在板型含 Feature::kOled 且 0x3C 应答时创建低优先级刷新任务。 */
class OledModule final {
public:
    /** 启动期外供存储视图（OLED 刷新任务栈 + 1 KB 帧缓冲，由 BootArena 仅在含 Feature::kOled 板型分配）。 */
    struct StorageView {
        std::span<StackType_t> taskStack{};
        std::span<uint8_t> frameBuffer{};
    };

    static constexpr size_t kTaskStackBytes = 3072U;   // B，静态任务栈（与 trace_out 一致）
    static constexpr size_t kFrameBufferBytes = 1024U; // B，8×128 单色帧缓冲（sizeof(ScreenBuff)）
    static constexpr uint32_t kRefreshPeriodMs = 200U; // ms，上限 5 Hz，空闲不写 I2C

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
        if (task_.running() || present_) {
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

private:
    /**
     * @brief OLED 静态任务入口函数
     * @param arg: OledModule 实例指针
     * @note 仅 oled 任务执行，等待启动屏障后进入 5 Hz 刷新循环，非 ISR。
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
     * @note 仅 oled 任务执行，通过 ulTaskNotifyTake 限速 <= 5 Hz，非 ISR。
     */
    void run() noexcept;

    /**
     * @brief 采集只读快照、推进状态机、经 WouoUI 渲染并按脏页刷屏
     * @param nowMs: 当前单调时间（ms）
     * @param forceFull: true 时强制全 8 页写出
     * @retval 下一轮刷新等待时间（ms，不超过 kRefreshPeriodMs）
     * @note 仅 init 或 oled 任务串行调用，非 ISR。
     */
    [[nodiscard]] uint32_t refreshOnce(uint32_t nowMs, bool forceFull) noexcept;

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
    bool present_ = false;
    uint32_t framesFlushed_ = 0U;
    uint32_t framesSkipped_ = 0U;
    OledStatus lastPublished_{};
};

} // namespace app
