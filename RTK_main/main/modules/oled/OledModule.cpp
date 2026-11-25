// P2 OLED 显示模块实现（ADR-0020 / OL2）：WouoUI HAL 桥接、30 fps 动画/静止 0 写调度与性能统计。
// init 仅启动任务调用；run 仅 oled 低优先级任务调用（Core 1, prio 1）；不阻塞 kLink，非 ISR。
// 堆档位：启动期一次性（任务栈与 1 KB 帧缓冲由 BootArena 仅在含 Feature::kOled 板型上外供；本文件静态状态全零落 .bss）。
#include "modules/oled/OledModule.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <optional>

#include "esp_timer.h"
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
constexpr uint32_t kMsPerSec = 1000U;
constexpr uint32_t kSecPerMin = 60U;
constexpr size_t kBdaSuffixByte0 = 3U;
constexpr size_t kBdaSuffixByte1 = 4U;
constexpr size_t kBdaSuffixByte2 = 5U;
static_assert(sizeof(ScreenBuff) == OledModule::kFrameBufferBytes,
              "WouoUI ScreenBuff size must equal OledModule::kFrameBufferBytes");

// 单实例全零静态状态（均落 .dram0.bss）；任务栈与 1 KB 帧缓冲由 BootArena 按 Feature::kOled 外供。
constinit ScreenBuff *s_active_fb = nullptr;
constinit board::Sh1106 s_panel{};
constinit oled::UiController s_ui_ctrl{};
constinit oled::ModalFrame s_modal_frame{};
constinit board::Sh1106FlushResult s_last_flush{};
constinit bool s_force_next_flush = false;
constinit std::optional<uint32_t> s_flush_deadline_ms{};
constinit std::atomic<TaskHandle_t> s_oled_task_handle{nullptr};
constinit portMUX_TYPE s_perf_mux = portMUX_INITIALIZER_UNLOCKED;
static_assert(fw::kAtomicUsable<TaskHandle_t>, "OLED 任务句柄原子须无锁");

/**
 * @brief WouoUI 图形层 HAL 刷屏回调：把 8×128 帧缓冲交由 SH1106 按脏页写出
 * @param fb: WouoUI 帧缓冲（BootArena 外供的 s_active_fb）
 * @note 仅由 OledModule 在 init 或 oled 任务中经 WouoUI_BuffSend 同步调用，非 ISR。
 */
void sendBufferHal(ScreenBuff fb) noexcept
{
    static_cast<void>(fb);
    s_last_flush = s_active_fb != nullptr
                       ? s_panel.flush(*s_active_fb, s_force_next_flush, s_flush_deadline_ms)
                       : board::Sh1106FlushResult::kDisabled;
    s_force_next_flush = false;
    s_flush_deadline_ms.reset();
}

/**
 * @brief 将 SH1106 底层刷屏结果映射为纯逻辑 FlushOutcome 枚举
 * @param res: SH1106 刷屏结论
 * @retval 对应纯逻辑 FlushOutcome
 * @note 纯函数。
 */
[[nodiscard]] oled::FlushOutcome toFlushOutcome(board::Sh1106FlushResult res) noexcept
{
    switch (res) {
    case board::Sh1106FlushResult::kWritten:
        return oled::FlushOutcome::kWritten;
    case board::Sh1106FlushResult::kSkipped:
        return oled::FlushOutcome::kSkipped;
    case board::Sh1106FlushResult::kBackoff:
        return oled::FlushOutcome::kBackoff;
    case board::Sh1106FlushResult::kInterrupted:
        return oled::FlushOutcome::kInterrupted;
    case board::Sh1106FlushResult::kError:
        return oled::FlushOutcome::kError;
    case board::Sh1106FlushResult::kDisabled:
    default:
        return oled::FlushOutcome::kDisabled;
    }
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

#if RTK_GNSS_ENABLED
/**
 * @brief 在独立栈帧中读取 512 B 卫星表并汇总到 ViewModel，返回后即释放局部栈空间
 * @param app: 应用上下文（只读）
 * @param[out] vm: 目标视图模型。
 * @param detail: 是否汇总卫星图表数据。
 * @note 仅 init 或 oled 任务调用，不分配堆，非 ISR。
 */
[[gnu::noinline]] void collectSatSummary(const AppContext &app, oled::ViewModel &vm,
                                         bool detail) noexcept
{
    const gnss::SatTable table = app.data.gnssSats.read();
    oled::summarizeSatTable(table, vm, detail);
}
#endif

/**
 * @brief 读取供电状态及本次关机进度。
 * @param app: 应用上下文（只读访问）。
 * @param[out] vm: 目标视图模型。
 */
void collectPowerStatus(const AppContext &app, oled::ViewModel &vm) noexcept
{
    const PowerSnapshot pwr = app.data.powerStatus.read();
    vm.powerActive = pwr.active;
    vm.batteryAdcReady = pwr.batteryAdcReady;
    vm.chargerPresent = pwr.chargerPresent;
    vm.chargerOk = pwr.chargerOk;
    vm.batteryPct = pwr.batteryPct;
    vm.batterySimSession = pwr.batterySimSession;
    vm.batteryMv = pwr.batteryMv;
    vm.usbPresent = pwr.usbPresent;
    vm.charging = pwr.charging;
    vm.usbSource = pwr.usbSource;
    vm.shutdownStage = pwr.shutdownStage;
    vm.shutdownReason = pwr.lastShutdown;
    vm.shutdownDoneMask = pwr.shutdownDoneMask;
    vm.shutdownDegradedMask = pwr.shutdownDegradedMask;
    vm.shutdownDoneCount = pwr.shutdownDoneCount;
    vm.shutdownTotalCount = pwr.shutdownTotalCount;
    vm.keyHold = app.data.uiEvents.readKeyHold();
    vm.rebootBanner = app.data.uiEvents.rebootBanner();
}

/**
 * @brief 收集通信与配对只读状态到视图模型
 * @param app: 应用上下文（只读访问）
 * @param sys: 已读取的系统状态快照
 * @param[out] vm: 目标视图模型
 * @note 仅 collectViewModel 调用，不访问硬件外设。
 */
void collectCommStatus(const AppContext &app, const SystemStatus &sys, oled::ViewModel &vm) noexcept
{
    vm.btReady = sys.btReady;
    vm.wifiReady = vm.radioMode == RadioMode::kWifiConfig &&
                   app.data.wifiReady.load(std::memory_order_acquire);
    vm.pairing = app.data.oledPairing.read();
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    vm.wifiSupported = true;
    if (vm.wifiReady) {
        const auto ap = app.data.wifiAp.read();
        const std::string_view ssid{ap.ssid.data(), ap.ssid.size()};
        oled::sanitizeCopy(ssid.substr(0U, ssid.find('\0')), vm.wifiSsid);
        vm.wifiIpv4 = ap.ipv4;
        vm.wifiClients = app.web.wifiStationCount();
    }
#endif

    const Sessions sess = app.data.sessions.read();
    // kMaxSessions == 2，窄化到 uint8_t 安全。
    vm.btSessions = static_cast<uint8_t>(sess.count);
    for (const auto &entry : sess.entries) {
        if (entry.used) {
            static_cast<void>(
                std::snprintf(vm.btPeerLabel.data(), vm.btPeerLabel.size(), "%02X:%02X:%02X",
                              static_cast<unsigned>(entry.addr.bytes[kBdaSuffixByte0]),
                              static_cast<unsigned>(entry.addr.bytes[kBdaSuffixByte1]),
                              static_cast<unsigned>(entry.addr.bytes[kBdaSuffixByte2])));
            break;
        }
    }
}

#if RTK_GNSS_ENABLED
/**
 * @brief 收集 GNSS 定位、UTC 时间与工作模式只读状态到视图模型
 * @param app: 应用上下文（只读访问）
 * @param[out] vm: 目标视图模型
 * @note 仅 collectViewModel 调用，不访问硬件外设。
 */
void collectGnssStatus(const AppContext &app, oled::ViewModel &vm) noexcept
{
    const GnssStatus gs = app.data.gnssStatus.read();
    const gnss::Fix fix = app.data.gnssFix.read();
    const GnssWorkModeStatus wm = app.data.gnssWorkMode.read();
    vm.gnssEnabled = gs.enabled;
    vm.gnssState = gs.state;
    vm.gnssModel = gs.model;
    oled::sanitizeCopy(cstrView(gs.fwVersion.data()), vm.gnssFwVer);
    vm.fixValid = fix.valid;
    vm.fixQuality = fix.quality;
    vm.utcValid = (fix.utc.year != 0U) || (fix.utc.hour != 0U) || (fix.utc.minute != 0U) ||
                  (fix.utc.ms != 0U) || (fix.valid && (fix.updateMs != 0U));
    vm.utcHour = fix.utc.hour;
    vm.utcMinute = fix.utc.minute;
    vm.utcSecond = static_cast<uint8_t>((fix.utc.ms / kMsPerSec) % kSecPerMin);
    vm.latE9 = fix.latE9;
    vm.lonE9 = fix.lonE9;
    vm.altMm = fix.altMm;
    vm.satsUsed = fix.satsUsed;
    vm.hdopX100 = fix.hdopX100;
    vm.diffAgeDs = fix.diffAgeDs;
    vm.hAccCm = fix.hAccCm;
    vm.workModeActive = wm.active;
    vm.workMode = wm.mode;
    vm.workModeState = wm.state;
    vm.workModeElapsedS = wm.elapsedS;
}
#endif

/**
 * @brief 凭完整帧的传输结果确认关机进度或终帧。
 * @param events: OLED 与 Power 的事件通道。
 * @param vm: 已渲染的快照。
 * @param markFinalFlushed: 本帧是否为已成功刷出的终帧。
 */
void acknowledgeShutdownFrame(UiEventBus &events, const oled::ViewModel &vm,
                              bool markFinalFlushed) noexcept
{
    if (markFinalFlushed) {
        const bool usb = (vm.shutdownDoneMask &
                          board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady)) != 0U;
        if (!events.shutdownFinalFrameFlushed(usb)) {
            events.markShutdownFinalFrameFlushed(usb);
        }
    }
    if (vm.shutdownStage != board::ShutdownStage::kIdle &&
        (s_last_flush == board::Sh1106FlushResult::kWritten ||
         s_last_flush == board::Sh1106FlushResult::kSkipped)) {
        events.markShutdownStageFlushed(vm.shutdownStage);
    }
}

/**
 * @brief 按生产时间戳处理待消费的按键，推进 UI 并投递确认动作。
 * @param events: 任务间 UI 事件通道。
 * @param nowMs: 当前单调时间。
 * @param vm: 本轮状态快照。
 * @retval 下一步渲染与等待决策。
 */
[[nodiscard]] oled::UiDecision processUiInput(UiEventBus &events, uint32_t nowMs,
                                              const oled::ViewModel &vm) noexcept
{
    std::array<uint32_t, UiEventBus::kShortKeyQueueCap> shortKeys{};
    size_t shortKeyCount = 0U;
    while ((shortKeyCount < shortKeys.size()) && events.popShortKey(shortKeys[shortKeyCount])) {
        ++shortKeyCount;
    }
    bool longKeyArrived = false;
    uint32_t longKeyMs = 0U;
    while (events.popLongKey(longKeyMs)) {
        longKeyArrived = true;
    }
    const oled::UiDecision dec = s_ui_ctrl.step(
        nowMs, std::span<const uint32_t>{shortKeys.data(), shortKeyCount}, vm, longKeyArrived);
    if (dec.action.action != OledAction::kNone) {
        static_cast<void>(events.submitAction(dec.action));
    }

    return dec;
}

/** 任务通知等待的结果，区分管线立即重跑与真正的事件/超时。 */
struct TaskWake {
    bool woke = false;
    bool notified = false;
};

/**
 * @brief 按绝对时刻已计算好的相对毫秒数等待 OLED 任务通知。
 * @param waitMs: 等待时长，kWaitForEvent 表示永久等待。
 * @retval 是否从等待返回及是否收到任务通知。
 */
[[nodiscard]] TaskWake waitForWake(uint32_t waitMs) noexcept
{
    const TickType_t ticks =
        waitMs == oled::kWaitForEvent
            ? portMAX_DELAY
            : std::max<TickType_t>((waitMs != 0U) ? 1U : 0U, pdMS_TO_TICKS(waitMs));
    const bool notified = ulTaskNotifyTake(pdTRUE, ticks) != 0U;
    return {.woke = ticks != 0U || notified, .notified = notified};
}

} // namespace

[[gnu::noinline]] void OledModule::collectViewModel(uint32_t nowMs,
                                                    oled::ViewModel &vm) const noexcept
{
    const auto &app = *context_;
    vm = {};
    vm.uptimeMs = nowMs;
    const auto &profile = board::current();
    oled::sanitizeCopy(cstrView(board::productName(&profile)), vm.product);
    oled::sanitizeCopy(cstrView(profile.name), vm.boardName);
    oled::sanitizeCopy(cstrView(profile.hardwareVersion), vm.hwVer);
    oled::sanitizeCopy(appVersion(), vm.fwVer);

    const SystemStatus sys = app.data.systemStatus.read();
    oled::sanitizeCopy(cstrView(sys.sn.data()), vm.sn);
    oled::sanitizeCopy(cstrView(sys.deviceName.data()), vm.deviceName);
    vm.bootMode = app.bootMode;
    vm.radioMode = app.radioMode;
    vm.nvsReady = app.bootConfig.userNvsReady && !app.bootConfig.safeDefaults;

    collectCommStatus(app, sys, vm);
    collectPowerStatus(app, vm);
#if RTK_GNSS_ENABLED
    collectGnssStatus(app, vm);
#endif
}

bool OledModule::tryInitPanel(uint32_t nowMs, bool initialBoot) noexcept
{
    const auto &profile = board::current();
    const OledInitStage prevStage = initStage_;
    const fw::Status prevStatus = initStatus_;
    ++initAttempts_;
    lastInitAttemptMs_ = nowMs;

    const fw::Status st = s_panel.init(profile.oled, !initialBoot);
    initStage_ = s_panel.lastInitStage();
    initStatus_ = s_panel.lastInitStatus();
    if (!fw::isOk(st)) {
        s_active_fb = nullptr;
        present_.store(false, std::memory_order_release);
        if (initialBoot) {
            FW_LOGE(fw::TraceCat::kSys,
                    "<oled> init failed at %s (%s), background retry every %" PRIu32 " ms",
                    oledInitStageName(initStage_), fw::toString(initStatus_), kInitRetryPeriodMs);
        } else if ((initStage_ != prevStage) || (initStatus_ != prevStatus)) {
            FW_LOGW(fw::TraceCat::kSys, "<oled> background retry #%" PRIu32 " failed at %s (%s)",
                    initAttempts_, oledInitStageName(initStage_), fw::toString(initStatus_));
        }
        return false;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): BootArena 外供的 1 KB 零初始化 uint8_t 缓冲与 ScreenBuff(uint8_t[8][128]) 布局完全一致。
    s_active_fb = reinterpret_cast<ScreenBuff *>(storage_.frameBuffer.data());
    present_.store(true, std::memory_order_release);
    panelOffline_ = false;
    consecutiveFlushErrors_ = 0U;
    statusBarValid_ = false;
    s_modal_frame = {};
    WouoUI_GraphSetSendBuffFun(&sendBufferHal);
    context_->data.uiEvents.drainShortKeys();
    context_->data.uiEvents.drainLongKeys();
    s_ui_ctrl.reset(context_->bootMode, nowMs);
    if (!initialBoot) {
        context_->data.uiEvents.setOledActive(true);
        FW_LOGI(fw::TraceCat::kSys, "<oled> SH1106 recovered in background (attempt %" PRIu32 ")",
                initAttempts_);
    }
    return true;
}

fw::Result OledModule::init() noexcept
{
    if (context_ == nullptr) {
        return fw::Result::kInvalidState;
    }
    enabled_ = false;
    present_.store(false, std::memory_order_release);
    consecutiveFlushErrors_ = 0U;
    panelOffline_ = false;
    wasOledActive_ = false;
    rebootQuiesced_ = false;
    initStage_ = OledInitStage::kNone;
    initStatus_ = fw::Status::kOk;
    initAttempts_ = 0U;
    lastInitAttemptMs_ = 0U;
    framesFlushed_ = 0U;
    framesSkipped_ = 0U;
    lastI2cUs_ = 0U;
    maxI2cUs_ = 0U;
    lastFrameUs_ = 0U;
    maxFrameUs_ = 0U;
    lastRenderUs_ = 0U;
    maxRenderUs_ = 0U;
    lastWriteMs_ = 0U;
    lastIntervalMs_ = 0U;
    maxIntervalMs_ = 0U;
    sampleIoUs_ = 0U;
    animating_ = false;
    statusBarValid_ = false;
    s_modal_frame = {};
    perf_ = {};
    lastPublished_ = {};

    const auto &profile = board::current();
    if (!board::hasFeature(profile, board::Feature::kOled) || (profile.oled.i2cPort < 0)) {
        s_active_fb = nullptr;
        initStage_ = OledInitStage::kDisabled;
        initStatus_ = fw::Status::kNotFound;
        publishStatus(true);
        return fw::Result::kOk;
    }
    if (!storageBound_) {
        s_active_fb = nullptr;
        initStage_ = OledInitStage::kStorage;
        initStatus_ = fw::Status::kInvalidState;
        FW_LOGE(fw::TraceCat::kSys, "<oled> storage not bound, display disabled");
        publishStatus(true);
        return fw::Result::kInvalidState;
    }

    enabled_ = true;
    const uint32_t now = rtos::nowMs();
    if (tryInitPanel(now, true)) {
        static_cast<void>(refreshOnce(rtos::nowMs(), true));
    }
    publishStatus(true);
    return fw::Result::kOk;
}

/**
 * @brief 面板启用时创建低优先级静态刷新任务（含离线后台恢复）
 * @retval fw::Result::kOk 任务已启动或面板未启用；其他见 OledModule.hpp 声明
 */
fw::Result OledModule::start() noexcept
{
    if (!enabled_) {
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
    self->context_->data.uiEvents.setOledActive(self->present_.load(std::memory_order_acquire) &&
                                                !self->panelOffline_);
    self->context_->data.uiEvents.setWakeHook(&OledModule::wakeFromBus, self);
    self->run();
}

oled::PerfTotals OledModule::recentPerf() const noexcept
{
    taskENTER_CRITICAL(&s_perf_mux);
    const auto snapshot = perf_;
    taskEXIT_CRITICAL(&s_perf_mux);
    return snapshot.recent(rtos::nowMs());
}

fw::Status OledModule::requestBench(uint32_t seconds) noexcept
{
    if (seconds == 0U || seconds > kMaxBenchSeconds) {
        return fw::Status::kInvalidArg;
    }
    if (s_oled_task_handle.load(std::memory_order_acquire) == nullptr ||
        !context_->data.uiEvents.oledActive() || context_->bootMode != board::BootMode::kNormal ||
        context_->data.uiEvents.rebootBanner() != RebootBanner::kNone ||
        context_->data.powerStatus.read().shutdownStage != board::ShutdownStage::kIdle) {
        return fw::Status::kInvalidState;
    }
    uint32_t pending = 0U;
    if (!benchDurationMs_.compare_exchange_strong(
            pending, seconds * kMsPerSec, std::memory_order_release, std::memory_order_relaxed)) {
        return fw::Status::kBusy;
    }
    wakeFromBus(this);
    return fw::Status::kOk;
}

bool OledModule::benchResult(oled::BenchStats &result) const noexcept
{
    if (benchDurationMs_.load(std::memory_order_acquire) != 0U) {
        return false;
    }
    // console 串行提交与读取，OLED 发布结束后到下一次请求前不再写 bench_。
    result = bench_;
    return true;
}

bool OledModule::updateBench(uint32_t nowMs) noexcept
{
    const uint32_t duration = benchDurationMs_.load(std::memory_order_acquire);
    if (duration == 0U) {
        return false;
    }
    if (!benchActive_) {
        bench_ = {};
        bench_.i2cErrors = s_panel.i2cErrors();
        benchStartMs_ = nowMs;
        benchActive_ = true;
    }
    const bool interrupted =
        context_->data.uiEvents.rebootBanner() != RebootBanner::kNone ||
        context_->data.powerStatus.read().shutdownStage != board::ShutdownStage::kIdle;
    if (!interrupted && nowMs - benchStartMs_ < duration) {
        return true;
    }
    bench_.interrupted = interrupted;
    bench_.elapsedMs = nowMs - benchStartMs_;
    bench_.i2cErrors = s_panel.i2cErrors() - bench_.i2cErrors;
    benchActive_ = false;
    benchDurationMs_.store(0U, std::memory_order_release);
    return false;
}

void OledModule::recordCycle(uint32_t nowMs, int64_t startUs, bool written, bool woke,
                             bool rendered) noexcept
{
    // 墙钟工作量扣除 I2C 和锁阻塞，保留摘要、封包及轮询等任务开销。
    const auto elapsedUs = static_cast<uint32_t>(esp_timer_get_time() - startUs);
    const uint32_t workUs = rendered ? elapsedUs - std::min(elapsedUs, sampleIoUs_) : elapsedUs;
    taskENTER_CRITICAL(&s_perf_mux);
    perf_.record(nowMs, workUs, written, woke);
    taskEXIT_CRITICAL(&s_perf_mux);
    if (benchActive_) {
        bench_.record(nowMs, workUs, rendered ? lastRenderUs_ : 0U,
                      rendered ? s_panel.lastTxBytes() : 0U, written, woke);
    }
}

void OledModule::run() noexcept
{
    oled::FramePacer pacer{};
    TaskWake wake{};
    uint32_t controlWait = 0U;
    uint32_t controlDeadlineMs = rtos::nowMs();
    while (true) {
        const int64_t workStartUs = esp_timer_get_time();
        const uint32_t nowMs = rtos::nowMs();
        const bool wasBench = benchActive_;
        const bool bench = updateBench(nowMs);
        const uint32_t frameWait = pacer.waitMs(nowMs);
        const bool paced = frameWait != oled::kWaitForEvent;
        const bool controlDue =
            controlWait != oled::kWaitForEvent && oled::remainingMs(nowMs, controlDeadlineMs) == 0U;
        const bool urgent =
            wake.notified &&
            (context_->data.uiEvents.rebootBanner() != RebootBanner::kNone ||
             context_->data.powerStatus.read().shutdownStage != board::ShutdownStage::kIdle);
        const bool frameDue = frameWait == 0U || urgent;
        const bool process =
            controlDue || frameDue || (!paced && (wake.notified || wasBench != bench));
        const bool draw = !paced || frameDue;
        const uint32_t prevFrames = framesFlushed_;
        if (process) {
            controlWait = refreshOnce(nowMs, false, draw);
            const uint32_t endMs = rtos::nowMs();
            controlDeadlineMs = endMs + controlWait;
            if (draw) {
                if (animating_ || bench) {
                    pacer.advance(nowMs, endMs);
                } else {
                    pacer = {};
                }
                publishStatus(false);
            }
        }
        const uint32_t endMs = rtos::nowMs();
        const uint32_t controlRemaining = controlWait == oled::kWaitForEvent
                                              ? controlWait
                                              : oled::remainingMs(endMs, controlDeadlineMs);
        uint32_t waitMs = std::min(pacer.waitMs(endMs), controlRemaining);
        if (bench) {
            const uint32_t duration = benchDurationMs_.load(std::memory_order_relaxed);
            waitMs = std::min(waitMs, oled::remainingMs(endMs, benchStartMs_ + duration));
        }
        recordCycle(nowMs, workStartUs, prevFrames != framesFlushed_, wake.woke, process && draw);
        wake = waitForWake(waitMs);
    }
}

uint32_t OledModule::handleBackgroundInitRetry(uint32_t nowMs) noexcept
{
    if ((nowMs - lastInitAttemptMs_) >= kInitRetryPeriodMs) {
        if (tryInitPanel(nowMs, false)) {
            return refreshOnce(rtos::nowMs(), true);
        }
    }
    return oled::remainingMs(rtos::nowMs(), lastInitAttemptMs_ + kInitRetryPeriodMs);
}

uint32_t OledModule::handleRebootQuiesce(uint32_t nowMs, RebootBanner banner) noexcept
{
    if (rebootQuiesced_) {
        return oled::kWaitForEvent;
    }
    if (!present_.load(std::memory_order_acquire) || s_panel.poweredOff() ||
        (s_active_fb == nullptr) || (banner == RebootBanner::kKeepFrame)) {
        rebootQuiesced_ = true;
        context_->data.uiEvents.markRebootQuiesced();
        return oled::kWaitForEvent;
    }

    const int64_t frameStartUs = esp_timer_get_time();
    oled::ViewModel vm{};
    collectViewModel(nowMs, vm);
    static_cast<void>(s_ui_ctrl.step(nowMs, {}, vm, false));
    const oled::UiRenderState rstate = s_ui_ctrl.renderState(nowMs);
    const int64_t renderStartUs = esp_timer_get_time();
    static_cast<void>(oled::renderUi(rstate, vm, *s_active_fb));
    statusBarValid_ = false;
    s_modal_frame = {};
    lastRenderUs_ = static_cast<uint32_t>(esp_timer_get_time() - renderStartUs);
    s_force_next_flush = false;
    s_flush_deadline_ms.reset();
    s_panel.clearBackoff();

    const int64_t i2cStartUs = esp_timer_get_time();
    WouoUI_BuffSend();
    const int64_t i2cEndUs = esp_timer_get_time();
    const uint32_t thisFrameUs =
        (i2cEndUs > frameStartUs) ? static_cast<uint32_t>(i2cEndUs - frameStartUs) : 0U;
    updateFrameStats(nowMs, thisFrameUs, i2cEndUs - i2cStartUs);
    if ((s_last_flush == board::Sh1106FlushResult::kWritten) ||
        (s_last_flush == board::Sh1106FlushResult::kSkipped)) {
        rebootQuiesced_ = true;
        context_->data.uiEvents.markRebootQuiesced();
        return oled::kWaitForEvent;
    }
    return oled::kShutdownFinalRetryMs;
}

void OledModule::drawFrame(uint32_t nowMs, oled::ViewModel &vm) noexcept
{
    oled::UiRenderState rstate = s_ui_ctrl.renderState(nowMs);
    oled::PageTransition benchTransition{};
    if (benchActive_) {
        constexpr uint8_t kPowerDock = static_cast<uint8_t>(oled::kDockItemCount - 1U);
        constexpr int16_t kFocusX = oled::kDockStartX + kPowerDock * oled::kDockStepX;
        constexpr oled::Rect kIcon{kFocusX, oled::kDockY, oled::kDockW, oled::kDockH};
        const uint32_t elapsed = nowMs - benchStartMs_;
        const uint32_t segment = elapsed / oled::kPageTransitionMs;
        const bool opening = (segment & 1U) == 0U;
        benchTransition.start(opening ? oled::TransitionKind::kExpandFromIcon
                                      : oled::TransitionKind::kCollapseToIcon,
                              opening ? OledPage::kDialplate : OledPage::kSubPower,
                              opening ? OledPage::kSubPower : OledPage::kDialplate, kIcon,
                              benchStartMs_ + segment * oled::kPageTransitionMs);
        rstate = oled::UiRenderState{.page = benchTransition.toPage,
                                     .dockSelected = kPowerDock,
                                     .dockFocusX = kFocusX,
                                     .nowMs = nowMs,
                                     .transition = &benchTransition};
    }
#if RTK_GNSS_ENABLED
    const auto *transition = rstate.transition;
    const bool transitioning =
        transition != nullptr && transition->kind != oled::TransitionKind::kNone;
    const bool satellitePage = rstate.page == OledPage::kSubSat ||
                               (transitioning && transition->fromPage == OledPage::kSubSat);
    if (satellitePage || rstate.page == OledPage::kDialplate || transitioning) {
        collectSatSummary(*context_, vm, satellitePage);
    }
#endif
    const int64_t renderStartUs = esp_timer_get_time();
    statusBarValid_ = oled::renderUi(rstate, vm, *s_active_fb, statusBarValid_, &s_modal_frame);
    lastRenderUs_ = static_cast<uint32_t>(esp_timer_get_time() - renderStartUs);
}

uint32_t OledModule::refreshOnce(uint32_t nowMs, bool forceFull, bool renderFrame) noexcept
{
    if (renderFrame) {
        animating_ = false;
        lastRenderUs_ = 0U;
        sampleIoUs_ = 0U;
    }
    const RebootBanner banner = context_->data.uiEvents.rebootBanner();
    if (banner != RebootBanner::kNone) {
        return handleRebootQuiesce(nowMs, banner);
    }
    rebootQuiesced_ = false;
    if (!present_.load(std::memory_order_acquire)) {
        return handleBackgroundInitRetry(nowMs);
    }

    const int64_t frameStartUs = esp_timer_get_time();
    oled::ViewModel vm{};
    collectViewModel(nowMs, vm);
    const auto dec = processUiInput(context_->data.uiEvents, nowMs, vm);

    if (!renderFrame) {
        return oled::settleFrameWait(dec, oled::FlushOutcome::kDisabled, nowMs, rtos::nowMs())
            .nextWaitMs;
    }
    if (s_panel.poweredOff() || (s_active_fb == nullptr)) {
        return dec.nextWaitMs;
    }

    animating_ = dec.animating;
    drawFrame(nowMs, vm);
    s_force_next_flush = forceFull;
    if (dec.hasClickDeadline) {
        s_flush_deadline_ms = dec.clickDeadlineMs;
    }
    if (dec.markShutdownFinalFrame) {
        s_panel.clearBackoff();
    }

    const int64_t i2cStartUs = esp_timer_get_time();
    WouoUI_BuffSend();
    const int64_t i2cEndUs = esp_timer_get_time();
    updatePanelOnlineState();

    const uint32_t thisFrameUs =
        (i2cEndUs > frameStartUs) ? static_cast<uint32_t>(i2cEndUs - frameStartUs) : 0U;
    const oled::PostFlushDecision post =
        oled::settleFrameWait(dec, toFlushOutcome(s_last_flush), nowMs, rtos::nowMs());
    acknowledgeShutdownFrame(context_->data.uiEvents, vm, post.markFinalFlushed);
    updateFrameStats(nowMs, thisFrameUs, i2cEndUs - i2cStartUs);
    if (s_last_flush == board::Sh1106FlushResult::kError ||
        s_last_flush == board::Sh1106FlushResult::kBackoff) {
        return std::min(post.nextWaitMs, kRetryPeriodMs);
    }
    return post.nextWaitMs;
}
void OledModule::updatePanelOnlineState() noexcept
{
    static_assert(kOfflineFlushErrors == board::Sh1106::kMaxErrorBackoffTicks,
                  "OLED offline error threshold must match Sh1106 backoff ceiling");
    if (s_last_flush == board::Sh1106FlushResult::kError) {
        if (consecutiveFlushErrors_ < kOfflineFlushErrors) {
            ++consecutiveFlushErrors_;
        }
        if ((consecutiveFlushErrors_ < kOfflineFlushErrors) || panelOffline_) {
            return;
        }
        panelOffline_ = true;
        wasOledActive_ = context_->data.uiEvents.oledActive();
        context_->data.uiEvents.setOledActive(false);
        if (!benchActive_) {
            FW_LOGW(fw::TraceCat::kSys, "<oled> panel offline after %u consecutive I2C errors",
                    static_cast<unsigned>(consecutiveFlushErrors_));
        }
        return;
    }
    if (s_last_flush != board::Sh1106FlushResult::kWritten) {
        return;
    }
    consecutiveFlushErrors_ = 0U;
    if (!panelOffline_) {
        return;
    }
    panelOffline_ = false;
    if (wasOledActive_) {
        context_->data.uiEvents.setOledActive(true);
    }
    if (!benchActive_) {
        FW_LOGI(fw::TraceCat::kSys, "<oled> panel back online");
    }
}

void OledModule::updateFrameStats(uint32_t nowMs, uint32_t frameUs, int64_t i2cUs) noexcept
{
    sampleIoUs_ = s_panel.lastIoUs();
    maxRenderUs_ = std::max(maxRenderUs_, lastRenderUs_);
    if (s_last_flush == board::Sh1106FlushResult::kWritten) {
        if (framesFlushed_ != 0U && nowMs - lastWriteMs_ < oled::kPerfWindowMs) {
            lastIntervalMs_ = nowMs - lastWriteMs_;
            maxIntervalMs_ = std::max(maxIntervalMs_, lastIntervalMs_);
        }
        lastWriteMs_ = nowMs;
        ++framesFlushed_;
        // 单帧耗时 < 1 s，微秒差值转为 uint32_t 无损。
        lastI2cUs_ = (i2cUs > 0) ? static_cast<uint32_t>(i2cUs) : 0U;
        lastFrameUs_ = frameUs;
        maxI2cUs_ = std::max(maxI2cUs_, lastI2cUs_);
        maxFrameUs_ = std::max(maxFrameUs_, lastFrameUs_);
    } else if (s_last_flush == board::Sh1106FlushResult::kSkipped) {
        ++framesSkipped_;
    }
}

void OledModule::publishStatus(bool force) noexcept
{
    const OledStatus next{
        .enabled = enabled_,
        .present = present_.load(std::memory_order_acquire),
        .poweredOff = s_panel.poweredOff(),
        .page = s_ui_ctrl.page(),
        .lastTxBytes = s_panel.lastTxBytes(),
        .lastDirtyPages = s_panel.lastDirtyPages(),
        .framesFlushed = framesFlushed_,
        .framesSkipped = framesSkipped_,
        .totalDirtyPages = s_panel.totalDirtyPages(),
        .lastI2cUs = lastI2cUs_,
        .maxI2cUs = maxI2cUs_,
        .lastFrameUs = lastFrameUs_,
        .maxFrameUs = maxFrameUs_,
        .lastRenderUs = lastRenderUs_,
        .maxRenderUs = maxRenderUs_,
        .lastIntervalMs = lastIntervalMs_,
        .maxIntervalMs = maxIntervalMs_,
        .i2cErrors = s_panel.i2cErrors(),
        .initStage = initStage_,
        .initStatus = initStatus_,
        .initAttempts = initAttempts_,
    };
    if (force || next != lastPublished_) {
        lastPublished_ = next;
        context_->data.oledStatus.write(next);
    }
}

} // namespace app
