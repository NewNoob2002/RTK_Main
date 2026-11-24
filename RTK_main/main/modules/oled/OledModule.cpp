// P2 OLED 显示模块实现（ADR-0020）：WouoUI HAL 桥接、低优先级刷新任务与只读快照采集。
// init 仅启动任务调用；run 仅 oled 低优先级任务调用（Core 1, prio 1, <= 5 Hz）；不阻塞 kLink，非 ISR。
// 堆档位：启动期一次性（任务栈与 1 KB 帧缓冲由 BootArena 仅在含 Feature::kOled 板型上外供；本文件静态状态全零落 .bss）。
#include "modules/oled/OledModule.hpp"

#include <array>
#include <atomic>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "WouoUI_graph.h"
#include "board/BoardProfile.hpp"
#include "board/Sh1106.hpp"
#include "fw/Atomic.hpp"
#include "fw/Log.hpp"
#include "modules/oled/OledUi.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

constexpr UBaseType_t kOledTaskPriority = 1U; // 低于 kLink(10)/kIo(9)/console(2)，绝不抢占业务
constexpr BaseType_t kOledTaskCore = 1;       // 绑 Core 1，远离 Core 0 的 BT/Wi-Fi/GNSS/console
static_assert(sizeof(ScreenBuff) == OledModule::kFrameBufferBytes,
              "WouoUI ScreenBuff size must equal OledModule::kFrameBufferBytes");

// 单实例全零静态状态（均落 .dram0.bss）；任务栈与 1 KB 帧缓冲由 BootArena 按 Feature::kOled 外供。
constinit ScreenBuff *s_active_fb = nullptr;
constinit board::Sh1106 s_panel{};
constinit oled::PageSm s_page_sm{};
constinit board::Sh1106FlushResult s_last_flush{};
constinit bool s_force_next_flush = false;
constinit std::atomic<TaskHandle_t> s_oled_task_handle{nullptr};
static_assert(fw::kAtomicUsable<TaskHandle_t>, "OLED 任务句柄原子须无锁");

/**
 * @brief WouoUI 图形层 HAL 刷屏回调：把 8×128 帧缓冲交由 SH1106 按脏页写出
 * @param fb: WouoUI 帧缓冲（BootArena 外供的 s_active_fb）
 * @note 仅由 OledModule 在 init 或 oled 任务中经 WouoUI_BuffSend 同步调用，非 ISR。
 */
void sendBufferHal(ScreenBuff fb) noexcept
{
    // WouoUI 仅绑定唯一外供缓冲 s_active_fb，避免数组形参退化后的指针强转。
    static_cast<void>(fb);
    if (s_active_fb == nullptr) {
        s_last_flush = board::Sh1106FlushResult::kDisabled;
        s_force_next_flush = false;
        return;
    }
    s_last_flush = s_panel.flush(*s_active_fb, s_force_next_flush);
    s_force_next_flush = false;
}

/**
 * @brief 比较两份 OLED 状态快照是否完全一致
 * @param lhs: 快照 A
 * @param rhs: 快照 B
 * @retval true 全部字段相等
 * @retval false 存在字段变化
 * @note 纯比较，不阻塞。
 */
[[nodiscard]] bool statusEqual(const OledStatus &lhs, const OledStatus &rhs) noexcept
{
    return (lhs.enabled == rhs.enabled) && (lhs.present == rhs.present) &&
           (lhs.poweredOff == rhs.poweredOff) && (lhs.page == rhs.page) &&
           (lhs.framesFlushed == rhs.framesFlushed) && (lhs.framesSkipped == rhs.framesSkipped) &&
           (lhs.i2cErrors == rhs.i2cErrors);
}

/**
 * @brief 将定长 C 字符串（可能未满）转为视图
 * @param str: C 字符串指针，可为 nullptr
 * @retval 字符串视图，nullptr 返回空视图
 * @note 纯函数。
 */
[[nodiscard]] std::string_view cstrView(const char *str) noexcept
{
    return (str != nullptr) ? std::string_view{str} : std::string_view{};
}

/**
 * @brief 从 DataCenter 与 AppContext 只读快照收集当前 UI 视图模型
 * @param app: 应用上下文（只读访问）
 * @retval 填充好的纯值视图模型
 * @note 仅 init 或 oled 任务调用；只读现有数据槽，不写任何其他模块状态，非 ISR。
 */
[[nodiscard]] oled::ViewModel collectViewModel(const AppContext &app) noexcept
{
    oled::ViewModel vm{};
    const auto &profile = board::current();
    oled::sanitizeCopy(cstrView(board::productName(&profile)), vm.product);
    oled::sanitizeCopy(cstrView(profile.name), vm.boardName);
    oled::sanitizeCopy(cstrView(profile.hardwareVersion), vm.hwVer);
    oled::sanitizeCopy(appVersion(), vm.fwVer);

    const SystemStatus sys = app.data.systemStatus.read();
    oled::sanitizeCopy(cstrView(sys.sn.data()), vm.sn);
    vm.bootMode = app.bootMode;
    vm.radioMode = app.radioMode;
    vm.btReady = sys.btReady;

    const Sessions sess = app.data.sessions.read();
    // kMaxSessions == 2，窄化到 uint8_t 安全。
    vm.btSessions = static_cast<uint8_t>(sess.count);

    const PowerSnapshot pwr = app.data.powerStatus.read();
    vm.batteryPct = pwr.batteryPct;
    vm.batteryMv = pwr.batteryMv;
    vm.usbPresent = pwr.usbPresent;
    vm.charging = pwr.charging;
    vm.shutdownStage = pwr.shutdownStage;
    vm.shutdownReason = pwr.lastShutdown;

#if RTK_GNSS_ENABLED
    const GnssStatus gs = app.data.gnssStatus.read();
    const gnss::Fix fix = app.data.gnssFix.read();
    vm.gnssEnabled = gs.enabled;
    vm.gnssState = gs.state;
    vm.fixValid = fix.valid;
    vm.fixQuality = fix.quality;
    vm.satsUsed = fix.satsUsed;
    vm.hAccCm = fix.hAccCm;
#endif
    return vm;
}

} // namespace

fw::Result OledModule::init() noexcept
{
    if (context_ == nullptr) {
        return fw::Result::kInvalidState;
    }
    enabled_ = false;
    present_ = false;
    framesFlushed_ = 0U;
    framesSkipped_ = 0U;
    lastPublished_ = {};

    const auto &profile = board::current();
    if (!board::hasFeature(profile, board::Feature::kOled) || (profile.oled.i2cPort < 0)) {
        s_active_fb = nullptr;
        publishStatus(true);
        return fw::Result::kOk;
    }
    if (!storageBound_) {
        s_active_fb = nullptr;
        publishStatus(true);
        return fw::Result::kInvalidState;
    }

    enabled_ = true;
    const fw::Status st = s_panel.init(profile.oled);
    if (!fw::isOk(st)) {
        // 面板无应答或初始化失败仅记录一行 W（已由 Sh1106::init 输出），不影响其他模块启动。
        s_active_fb = nullptr;
        publishStatus(true);
        return fw::Result::kOk;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): BootArena 外供的 1 KB 零初始化 uint8_t 缓冲与 ScreenBuff(uint8_t[8][128]) 布局完全一致。
    s_active_fb = reinterpret_cast<ScreenBuff *>(storage_.frameBuffer.data());
    present_ = true;
    WouoUI_GraphSetSendBuffFun(&sendBufferHal);
    s_page_sm.reset(context_->bootMode, rtos::nowMs(), context_->data.uiEvents.shortKeyCount());
    static_cast<void>(refreshOnce(rtos::nowMs(), true));
    publishStatus(true);
    return fw::Result::kOk;
}

fw::Result OledModule::start() noexcept
{
    if (!present_) {
        return fw::Result::kOk;
    }
    if (!storageBound_) {
        return fw::Result::kInvalidState;
    }
    if (task_.running()) {
        return fw::Result::kBusy;
    }
    const fw::Status status = task_.start(storage_.taskStack, "oled", kOledTaskPriority,
                                          kOledTaskCore, &OledModule::taskEntry, this);
    if (!fw::isOk(status)) {
        FW_LOGW(fw::TraceCat::kSys, "<oled> task start failed (%s), display static",
                fw::toString(status));
        return status == fw::Status::kNoSpace ? fw::Result::kNoSpace : fw::Result::kInvalidArg;
    }
    return fw::Result::kOk;
}

void OledModule::wakeFromBus(void * /*ctx*/) noexcept
{
    if (const TaskHandle_t handle = s_oled_task_handle.load(std::memory_order_acquire);
        handle != nullptr) {
        static_cast<void>(xTaskNotifyGive(handle));
    }
}

void OledModule::taskEntry(void *arg) noexcept
{
    // arg 由 BootTask::start 固定传入 OledModule*，恢复类型安全。
    auto *self = static_cast<OledModule *>(arg);
    if ((self == nullptr) || (self->context_ == nullptr)) {
        vTaskDelete(nullptr);
        return;
    }
    if (!self->context_->awaitBoot(BootTask::kOled, false)) {
        vTaskDelete(nullptr);
        return;
    }
    s_oled_task_handle.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    self->context_->data.uiEvents.setWakeHook(&OledModule::wakeFromBus, self);
    self->run();
}

void OledModule::run() noexcept
{
    while (true) {
        const uint32_t waitMs = refreshOnce(rtos::nowMs(), false);
        publishStatus(false);
        const TickType_t waitTicks = pdMS_TO_TICKS(waitMs);
        static_cast<void>(ulTaskNotifyTake(pdTRUE, waitTicks == 0U ? 1U : waitTicks));
    }
}

uint32_t OledModule::refreshOnce(uint32_t nowMs, bool forceFull) noexcept
{
    const oled::ViewModel vm = collectViewModel(*context_);
    const oled::PageDecision dec =
        s_page_sm.step(nowMs, context_->data.uiEvents.shortKeyCount(), vm.shutdownStage);
    const uint32_t nextWaitMs = ((dec.nextWaitMs != 0U) && (dec.nextWaitMs < kRefreshPeriodMs))
                                    ? dec.nextWaitMs
                                    : kRefreshPeriodMs;

    if (dec.shouldWake) {
        if (fw::isOk(s_panel.wakeFromPowerOff())) {
            forceFull = true;
        } else {
            s_page_sm.onWakeFailed(nowMs);
        }
    }
    if (dec.shouldPowerOff) {
        s_panel.powerOff();
        return kRefreshPeriodMs;
    }
    if (s_panel.poweredOff() || (s_active_fb == nullptr)) {
        return nextWaitMs;
    }

    oled::renderPage(dec.page, vm, *s_active_fb);
    s_force_next_flush = forceFull;
    WouoUI_BuffSend();

    switch (s_last_flush) {
    case board::Sh1106FlushResult::kWritten:
        ++framesFlushed_;
        break;
    case board::Sh1106FlushResult::kSkipped:
        ++framesSkipped_;
        break;
    case board::Sh1106FlushResult::kError:
    case board::Sh1106FlushResult::kDisabled:
    default:
        break;
    }
    return nextWaitMs;
}

void OledModule::publishStatus(bool force) noexcept
{
    const OledStatus next{
        .enabled = enabled_,
        .present = present_,
        .poweredOff = s_panel.poweredOff(),
        .page = s_page_sm.page(),
        .framesFlushed = framesFlushed_,
        .framesSkipped = framesSkipped_,
        .i2cErrors = s_panel.i2cErrors(),
    };
    if (force || !statusEqual(next, lastPublished_)) {
        lastPublished_ = next;
        context_->data.oledStatus.write(next);
    }
}

} // namespace app
