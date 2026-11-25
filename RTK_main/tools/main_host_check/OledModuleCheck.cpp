// 真实 OledModule 编排主机回归（R2OL2-03 / R2OL2-05）：直接编译 OledModule.cpp + Sh1106.cpp +
// I2cBus.cpp + OledUi/Anim/Render，在 I2C 与虚拟时钟替身下验证终帧编排（连续错/退避/补写/顺序/零脏页）
// 及 OLED 正忙时的 299/300 ms 生产时间双击判定与绝对截止休眠重算。
#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

#include "AppContext.hpp"
#include "SppServerPairing.h"
#include "app/PowerRequests.hpp"
#include "board/BoardProfile.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw/Log.hpp"
#include "modules/oled/OledAnim.hpp"
#include "modules/oled/OledModule.hpp"
#include "modules/oled/OledUi.hpp"
#include "rtos/EspStatus.hpp"

struct I2cBus {};
struct I2cDevice {
    uint16_t addr = 0U;
    uint32_t speedHz = 0U;
};

namespace app {

constinit AppContext s_app{};

/** @brief 主机测试桩：返回静态应用上下文引用。 @retval s_app 引用。 */
AppContext &appContext() noexcept
{
    return s_app;
}
/** @brief 主机测试桩：启动屏障直接放行。 @param task: 任务枚举。 @param watchdog: 忽略。 @retval true 总是。 */
bool AppContext::awaitBoot(BootTask task, bool watchdog) noexcept
{
    static_cast<void>(task);
    static_cast<void>(watchdog);
    return true;
}

/** @brief 白盒访问真实 OledModule::refreshOnce 与 publishStatus，避免在产品代码中加测试开关。 */
struct ModuleTestAccess {
    /**
     * @brief 调用真实 OledModule::refreshOnce 推进一帧
     * @param oled: 目标 OledModule 实例
     * @param nowMs: 当前单调时间（ms）
     * @param forceFull: 是否强制全刷
     * @retval 本轮返回的精确睡眠时间（ms）
     */
    static uint32_t refresh(OledModule &oled, uint32_t nowMs, bool forceFull = false) noexcept
    {
        return oled.refreshOnce(nowMs, forceFull);
    }

    /**
     * @brief 调用真实 OledModule::publishStatus 发布快照
     * @param oled: 目标 OledModule 实例
     * @param force: 是否强制写槽
     */
    static void publish(OledModule &oled, bool force = false) noexcept
    {
        oled.publishStatus(force);
    }
};

} // namespace app

namespace {

using namespace app;
using namespace app::oled;

constinit I2cBus s_bus{};
constinit I2cDevice s_oled_dev{};
constinit StaticSemaphore_t *s_mutex = nullptr;
constinit int64_t s_time_us = 1000000LL;
constinit int64_t s_page_data_delay_us = 1000LL;
constinit std::optional<uint32_t> s_short_key_during_page{};
constinit bool s_fail_all_data = false;
constinit int s_fail_page_once = -1;
constinit bool s_assert_not_flushed_during_tx = false;
constinit uint8_t s_last_page_cmd = 0xFFU;
constinit unsigned s_oled_data_pages = 0U;
constinit unsigned s_oled_tx_count = 0U;
constinit unsigned s_restart_wakes = 0U;
constinit bool s_oled_probe_ok = true;
constinit esp_err_t s_oled_probe_err = ESP_ERR_TIMEOUT;
constinit unsigned s_error_logs = 0U;
constinit spp_server_pair_snapshot_t s_pair_snapshot{};
constinit uint32_t s_decision_id = 0U;
constinit bool s_decision_accept = false;
constinit bool s_web_exit = false;
constinit board::ShutdownReason s_shutdown_reason = board::ShutdownReason::kNone;
constinit bool s_radio_switch = false;
std::jmp_buf s_radio_jump{};

constinit std::array<StackType_t, OledModule::kTaskStackBytes / sizeof(StackType_t)> s_oled_stack{};
constinit std::array<uint8_t, OledModule::kFrameBufferBytes> s_oled_fb{};

/**
 * @brief 断言并在失败时中止
 * @param ok: 条件结果
 * @param expr: 表达式文本
 * @param line: 行号
 */
void check(bool ok, const char *expr, int line) noexcept
{
    if (!ok) {
        std::fprintf(stderr, "OledModuleCheck:%d: FAIL %s\n", line, expr);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

/**
 * @brief 读取当前虚拟单调毫秒时钟
 * @retval 当前毫秒数
 */
uint32_t nowMs() noexcept
{
    return static_cast<uint32_t>(s_time_us / 1000LL);
}

/**
 * @brief 初始化测试用 P2 v1.41 上下文并完成真实 OledModule::init()
 */
void setupOledModule() noexcept
{
    s_app.data.init();
    s_app.boardProfile = board::byId(board::BoardId::kP2V141);
    s_app.bootMode = board::BootMode::kNormal;
    s_app.radioMode = RadioMode::kBluetooth;
    s_app.bootConfig.userNvsReady = true;
    s_app.bootConfig.safeDefaults = false;

    SystemStatus sys{};
    sys.btReady = true;
    sanitizeCopy("P2000123", sys.sn);
    sanitizeCopy("P2-P2000123", sys.deviceName);
    s_app.data.systemStatus.write(sys);

    PowerSnapshot pwr{};
    pwr.active = true;
    pwr.batteryAdcReady = true;
    pwr.chargerPresent = true;
    pwr.chargerOk = true;
    pwr.batteryPct = 85U;
    pwr.batteryMv = 4050U;
    pwr.u3On = true;
    s_app.data.powerStatus.write(pwr);
#if RTK_GNSS_ENABLED
    GnssStatus gs{};
    gs.enabled = true;
    gs.state = gnss::State::kRunning;
    gs.model = gnss::Model::kUm980;
    s_app.data.gnssStatus.write(gs);
#endif
    s_app.oled.bindContext(s_app);
    CHECK(s_app.oled.bindStorage({s_oled_stack, s_oled_fb}) == fw::Status::kOk);
    CHECK(s_app.oled.init() == fw::Result::kOk);
    ModuleTestAccess::publish(s_app.oled, true);
    CHECK(s_app.data.oledStatus.read().enabled && s_app.data.oledStatus.read().present);
}

/**
 * @brief 场景 1：验证真实 OledModule USB 关机终帧编排（连续错->退避->清退避补写->先写后通知->零脏页，R2OL2-05）
 */
void runFinalFrameOrchestration(bool usb) noexcept
{
    setupOledModule();
    s_app.data.uiEvents.setRestartWakeHook([](void * /*ctx*/) noexcept { ++s_restart_wakes; },
                                           nullptr);

    // 1. 进入关机普通阶段并注入 2 次连续 I2C 失败，驱动 SH1106 进入 2 轮退避期。
    PowerSnapshot pwr = s_app.data.powerStatus.read();
    pwr.shutdownStage = board::ShutdownStage::kSave;
    pwr.shutdownDoneMask = board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced);
    s_app.data.powerStatus.write(pwr);
    s_fail_all_data = true;
    s_time_us += 50000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    s_time_us += 50000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    CHECK(!s_app.data.uiEvents.shutdownFinalFrameFlushed());
    // 验证当前确实处于退避态（普通帧不会发起 I2C TX）。
    const unsigned txBeforeBackoff = s_oled_tx_count;
    s_time_us += 50000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    CHECK(s_oled_tx_count == txBeforeBackoff);

    // 2. USB 关机 U3 掉电进入 kUsbOffReady；仍处在退避期内，且注入第 4 页写超时：
    // OledModule 必须先 clearBackoff() 发起写出（写出第 0..3 页），遇第 4 页失败绝不置终帧完成，并返回 2 ms 快速补刷。
    s_fail_all_data = false;
    s_fail_page_once = 4;
    pwr.shutdownStage = board::ShutdownStage::kCutPower;
    pwr.shutdownDoneMask = static_cast<uint8_t>(
        board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced) |
        board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFences) |
        board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed) |
        board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe) |
        board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced) |
        board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower) |
        board::shutdownDoneBit(usb ? board::ShutdownDoneBit::kUsbOffReady
                                   : board::ShutdownDoneBit::kBatteryOffReady));
    s_app.data.powerStatus.write(pwr);

    const unsigned pagesBeforeFinalErr = s_oled_data_pages;
    s_time_us += 10000LL;
    const uint32_t waitOnErr = ModuleTestAccess::refresh(s_app.oled, nowMs());
    CHECK(s_oled_data_pages - pagesBeforeFinalErr == 4U);
    CHECK(!s_app.data.uiEvents.shutdownFinalFrameFlushed(usb));
    CHECK(s_restart_wakes == 0U);
    CHECK(waitOnErr == kShutdownFinalRetryMs);

    // 3. 2 ms 后补刷剩余第 4..7 页：每一页 I2C 传输期间终帧完成位仍必须为 false（严格先刷屏后通知），
    // 返回后才置 true 并触发重启唤醒钩子。
    s_assert_not_flushed_during_tx = true;
    const unsigned pagesBeforeFinalOk = s_oled_data_pages;
    s_time_us += static_cast<int64_t>(waitOnErr) * 1000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    s_assert_not_flushed_during_tx = false;
    CHECK(s_oled_data_pages - pagesBeforeFinalOk == 4U);
    CHECK(s_app.data.uiEvents.shutdownFinalFrameFlushed(usb));
    CHECK(!s_app.data.uiEvents.shutdownFinalFrameFlushed(!usb));
    CHECK(s_restart_wakes == (usb ? 1U : 0U));

    // 4. 零脏页复刷：画面已完全一致（kSkipped），0 I2C 写出且继续保持终帧完成。
    const unsigned txBeforeIdle = s_oled_tx_count;
    s_time_us += 10000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    CHECK(s_oled_tx_count == txBeforeIdle);
    CHECK(s_app.data.uiEvents.shutdownFinalFrameFlushed(usb));
    s_app.data.uiEvents.setRestartWakeHook(nullptr, nullptr);
}

/** @brief 驱动实际 kLink 桥接，验证默认确认开窗、请求序号和两种射频路线。 */
void runCommActions() noexcept
{
    setupOledModule();
    auto &events = s_app.data.uiEvents;
    CHECK(events.submitAction({OledAction::kTogglePair}));
    static_cast<void>(s_app.oled.poll(nowMs()));
    CHECK(s_app.bt_link.pairStatus(nowMs()).confirm);
    CHECK(s_app.data.oledPairing.read().windowOpen);
    s_pair_snapshot = {.id = 73U,
                       .remaining_ms = 15000U,
                       .number = 382475U,
                       .bda = {0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U},
                       .name = "OL3 Peer",
                       .has_number = true};
    static_cast<void>(s_app.oled.poll(nowMs()));
    const auto pair = s_app.data.oledPairing.read();
    CHECK(pair.id == 73U && pair.suffix[0] == 0x03U && pair.hasNumber && pair.number == 382475U);
    CHECK(events.submitAction({OledAction::kPairAccept, pair.id}));
    static_cast<void>(s_app.oled.poll(nowMs()));
    CHECK(s_decision_id == 73U && s_decision_accept);

    s_app.radioMode = RadioMode::kWifiConfig;
    s_app.data.wifiReady.store(true);
    CHECK(events.submitAction({OledAction::kBluetooth}));
    static_cast<void>(s_app.oled.poll(nowMs()));
    CHECK(s_web_exit);

    CHECK(events.submitAction({OledAction::kShutdownKey}));
    static_cast<void>(s_app.oled.poll(nowMs()));
    CHECK(s_shutdown_reason == board::ShutdownReason::kKey);
    s_app.radioMode = RadioMode::kBluetooth;
    CHECK(events.submitAction({OledAction::kWifi}));
    if (setjmp(s_radio_jump) == 0) {
        static_cast<void>(s_app.oled.poll(nowMs()));
        CHECK(false);
    }
    CHECK(s_radio_switch);
}

/**
 * @brief 场景 2：验证 OLED 正忙时 299/300 ms 生产时间双击判定与休眠前绝对截止重算（R2OL2-03）
 */
void runBusyClickAndDeadlineSleep() noexcept
{
    setupOledModule();

    // 推进并跳过开机自检进入表盘静止态。
    s_time_us = 10000000LL; // t = 10000 ms
    s_app.data.uiEvents.notifyShortKey(nowMs());
    s_app.data.uiEvents.notifyShortKey(nowMs() + 80U);
    s_time_us += 100000LL; // t = 10100 ms
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    // 等待 300 ms 过渡动画结束进入静止表盘。
    s_time_us += 500000LL; // t = 10600 ms
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    ModuleTestAccess::publish(s_app.oled, true);
    CHECK(s_app.data.oledStatus.read().page == OledPage::kDialplate);

    // 1. 299 ms 边界且 OLED 正忙：
    // 首次短按产生于 t=20000 ms（已被 OLED 消费），第二次短按产生于 t=20299 ms（间隔 299 ms），
    // OLED 因忙于刷屏直到 t=20315 ms（滞后 16 ms）才消费 -> 必须判为双击进入 kSubSat。
    s_time_us = 20000000LL;
    s_app.data.uiEvents.notifyShortKey(20000U);
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 20000U));
    ModuleTestAccess::publish(s_app.oled, true);
    CHECK(s_app.data.oledStatus.read().page == OledPage::kDialplate);

    s_app.data.uiEvents.notifyShortKey(20299U);
    s_time_us = 20315000LL; // OLED 晚 16 ms 才消费
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 20315U));
    ModuleTestAccess::publish(s_app.oled, true);
    CHECK(s_app.data.oledStatus.read().page == OledPage::kSubSat);

    // 双击返回表盘并静止。
    s_app.data.uiEvents.notifyShortKey(21000U);
    s_app.data.uiEvents.notifyShortKey(21100U);
    s_time_us = 21120000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 21120U));
    s_time_us = 21600000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 21600U));
    ModuleTestAccess::publish(s_app.oled, true);
    CHECK(s_app.data.oledStatus.read().page == OledPage::kDialplate);

    // 2. 300 ms 边界且 OLED 正忙：
    // 两次消抖短按相隔恰好 300 ms（t=30000 与 t=30300），OLED 直到 t=30315 ms 才消费 ->
    // 必须判为前次单击（留在 kDialplate，绝不误进子菜单），并以 30300 开启新窗口（截止 30600）。
    s_time_us = 30000000LL;
    s_app.data.uiEvents.notifyShortKey(30000U);
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 30000U));
    s_app.data.uiEvents.notifyShortKey(30300U);
    s_time_us = 30315000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 30315U));
    ModuleTestAccess::publish(s_app.oled, true);
    CHECK(s_app.data.oledStatus.read().page == OledPage::kDialplate);
    // 让 30300 开启的窗口在 30600 到期并让弹簧收敛至静止。
    s_time_us = 30600000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 30600U));
    for (unsigned i = 0U; i < 20U; ++i) {
        s_time_us += 33000LL;
        static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    }

    // 3. 空闲帧休眠前按绝对窗口截止重算剩余时间：
    // 短按产生于 t=40000 ms（截止 40300 ms）；t=40250 ms 时空闲帧写屏耗去 20 ms（完成于 40270 ms），
    // 返回的休眠时间必须缩为 30 ms（而非 50 ms），确保在第 40300 ms 准时确认单击。
    s_time_us = 40000000LL;
    s_app.data.uiEvents.notifyShortKey(40000U);
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, 40000U));
    s_time_us = 40250000LL;
    s_page_data_delay_us = 2500LL; // 强制全刷 8 页 * 2.5 ms = 20 ms -> 完成于 t=40270 ms
    const uint32_t sleepAfterBusyIdle = ModuleTestAccess::refresh(s_app.oled, 40250U, true);
    CHECK(nowMs() == 40270U);
    CHECK(sleepAfterBusyIdle == 30U);

    // 4. 达到截止后不再写下一页，立即进入下一轮确认单击。
    s_time_us = 40280000LL;
    s_page_data_delay_us = 3000LL;
    const unsigned pagesBeforeDeadline = s_oled_data_pages;
    const uint32_t sleepAfterOverrun = ModuleTestAccess::refresh(s_app.oled, 40280U, true);
    CHECK(nowMs() == 40301U);
    CHECK(s_oled_data_pages - pagesBeforeDeadline == 7U);
    CHECK(sleepAfterOverrun == 0U);
}

/**
 * @brief 在正常电量滚动的两个脏页间让出刷新，验证截止边界的导航结果
 * @param secondAtMs: 在第一个脏页传输期间产生第二次短按的毫秒时间戳
 */
void runClickPageDeadline(uint32_t secondAtMs) noexcept
{
    setupOledModule();
    s_app.data.uiEvents.notifyShortKey(10000U);
    s_app.data.uiEvents.notifyShortKey(10080U);
    s_time_us = 10100000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    s_time_us = 11000000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));

    s_time_us = 40000000LL;
    s_app.data.uiEvents.notifyShortKey(nowMs());
    uint32_t waitMs = ModuleTestAccess::refresh(s_app.oled, nowMs());
    CHECK(waitMs == 200U);
    s_time_us += static_cast<int64_t>(waitMs) * 1000LL;
    auto snapshot = s_app.data.powerStatus.read();
    snapshot.batteryPct = 20U;
    s_app.data.powerStatus.write(snapshot);
    s_page_data_delay_us = 3000LL;
    for (unsigned frame = 0U; frame < 3U; ++frame) {
        waitMs = ModuleTestAccess::refresh(s_app.oled, nowMs());
        s_time_us += static_cast<int64_t>(waitMs) * 1000LL;
    }
    CHECK(nowMs() == 40299U);
    const unsigned pagesBeforeDeadline = s_oled_data_pages;
    s_short_key_during_page = secondAtMs;
    CHECK(ModuleTestAccess::refresh(s_app.oled, nowMs()) == 0U);
    CHECK(!s_short_key_during_page.has_value());
    CHECK(s_oled_data_pages - pagesBeforeDeadline == 1U);
    CHECK(nowMs() == 40302U);

    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    ModuleTestAccess::publish(s_app.oled, true);
    const auto expected = secondAtMs == 40299U ? OledPage::kSubSat : OledPage::kDialplate;
    CHECK(s_app.data.oledStatus.read().page == expected);
}
/**
 * @brief 场景：验证 OLED 连续 4 次 I2C 刷屏失败进入离线态清除 kOledActive，恢复写出后重新置位
 */
void runOfflineClearsOledActive() noexcept
{
    setupOledModule();
    s_app.data.uiEvents.setOledActive(true);
    CHECK(s_app.data.uiEvents.oledActive());

    // 制造帧变化并注入持续 I2C 失败；前 3 次 kError 尚未满 4 次，第 4 次 kError 清除 kOledActive。
    s_fail_all_data = true;
    auto pwr = s_app.data.powerStatus.read();
    for (unsigned step = 0U; step < 20U; ++step) {
        pwr.batteryPct = static_cast<uint8_t>(50U + (step % 40U));
        s_app.data.powerStatus.write(pwr);
        s_time_us += 200000LL;
        static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
        if (!s_app.data.uiEvents.oledActive()) {
            break;
        }
    }
    CHECK(!s_app.data.uiEvents.oledActive());

    // I2C 恢复后跳过退避轮次并成功刷出一帧，kOledActive 应恢复为 true。
    s_fail_all_data = false;
    for (unsigned step = 0U; step < 10U; ++step) {
        pwr.batteryPct = static_cast<uint8_t>(20U + step);
        s_app.data.powerStatus.write(pwr);
        s_time_us += 200000LL;
        static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
        if (s_app.data.uiEvents.oledActive()) {
            break;
        }
    }
    CHECK(s_app.data.uiEvents.oledActive());
}

/**
 * @brief 场景：启动探测失败记录失败原因并打 E 日志，后台周期重试成功恢复在线
 */
void runBackgroundInitRetry() noexcept
{
    s_oled_probe_ok = false;
    s_oled_probe_err = ESP_ERR_TIMEOUT;
    s_app.data.init();
    s_app.boardProfile = board::byId(board::BoardId::kP2V141);
    s_app.bootMode = board::BootMode::kNormal;
    s_app.radioMode = RadioMode::kWifiConfig;
    s_app.data.wifiReady.store(true);
    s_app.bootConfig.userNvsReady = true;
    s_app.bootConfig.safeDefaults = false;
    s_app.oled.bindContext(s_app);
    CHECK(s_app.oled.bindStorage({s_oled_stack, s_oled_fb}) == fw::Status::kOk);
    CHECK(s_app.oled.init() == fw::Result::kOk);

    OledStatus st = s_app.data.oledStatus.read();
    CHECK(st.enabled && !st.present && st.initStage == OledInitStage::kProbe &&
          st.initStatus == fw::Status::kTimeout && st.initAttempts == 1U);
    CHECK(s_error_logs >= 1U && !s_app.data.uiEvents.oledActive());
    // 即使首检失败，start() 仍启动后台任务以定期重试。
    CHECK(s_app.oled.start() == fw::Result::kOk && s_app.oled.taskRunning());

    // 未满重试周期不重探。
    s_time_us += 1000000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    ModuleTestAccess::publish(s_app.oled, true);
    CHECK(!s_app.data.oledStatus.read().present && s_app.data.oledStatus.read().initAttempts == 1U);

    // 满 2 s 且面板恢复应答 -> 后台重试成功并置位 oledActive、首帧刷出。
    s_oled_probe_ok = true;
    s_time_us += static_cast<int64_t>(OledModule::kInitRetryPeriodMs) * 1000LL;
    const unsigned pagesBefore = s_oled_data_pages;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    ModuleTestAccess::publish(s_app.oled, true);
    st = s_app.data.oledStatus.read();
    CHECK(st.present && st.initStage == OledInitStage::kOk && st.initStatus == fw::Status::kOk &&
          st.initAttempts == 2U);
    CHECK(s_app.data.uiEvents.oledActive() && (s_oled_data_pages > pagesBefore));
}

/**
 * @brief 场景：软件重启过渡帧（SWITCHING WI-FI）刷出后置位 rebootQuiesced 并停止一切后续 I2C 传输
 */
void runRebootBannerQuiesce() noexcept
{
    setupOledModule();
    s_app.data.uiEvents.setRestartWakeHook([](void * /*ctx*/) noexcept { ++s_restart_wakes; },
                                           nullptr);
    s_app.data.uiEvents.requestRebootBanner(RebootBanner::kSwitchingWifi);
    CHECK(!s_app.data.uiEvents.rebootQuiesced());

    const unsigned pagesBefore = s_oled_data_pages;
    s_time_us += 50000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    CHECK(s_oled_data_pages > pagesBefore);
    CHECK(s_app.data.uiEvents.rebootQuiesced() && s_restart_wakes == 1U);

    // 静默后即使数据变化也不再发起任何 I2C 写操作。
    const unsigned txAfterQuiesce = s_oled_tx_count;
    auto pwr = s_app.data.powerStatus.read();
    pwr.batteryPct = 12U;
    s_app.data.powerStatus.write(pwr);
    s_time_us += 50000LL;
    static_cast<void>(ModuleTestAccess::refresh(s_app.oled, nowMs()));
    CHECK(s_oled_tx_count == txAfterQuiesce);
    s_app.data.uiEvents.setRestartWakeHook(nullptr, nullptr);
    s_app.data.uiEvents.clearRebootBanner();
}

} // namespace

namespace app {

void BtLinkModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    CHECK(cmd.verb == verb(LinkVerb::kPair) && cmd.argLen == 3U && cmd.args[2] == 1U);
    core::openPairWindow(pair_window_, rtos::nowMs(), core::kPairWindowMs, true);
    result.status = fw::Result::kOk;
}

bool WebModule::requestOledExit(uint32_t /*nowMs*/) noexcept
{
    s_web_exit = true;
    return true;
}

void requestRadioMode(RadioMode mode, RadioReason reason) noexcept
{
    CHECK(mode == RadioMode::kWifiConfig && reason == RadioReason::kUi);
    CHECK(!power::s_power_requests.canScheduleRestart());
    s_radio_switch = true;
    std::longjmp(s_radio_jump, 1);
}

namespace power {
/** @brief 主机测试桩：记录 OLED 提交的按键关机原因。 @param reason: 关机原因。 @retval true 总是。 */

bool requestShutdown(board::ShutdownReason reason) noexcept
{
    s_shutdown_reason = reason;
    return true;
}

} // namespace power
} // namespace app

void spp_server_pair_snapshot(spp_server_pair_snapshot_t *out)
{
    *out = s_pair_snapshot;
}

bool spp_server_pair_decide_for(uint32_t id, bool accept)
{
    s_decision_id = id;
    s_decision_accept = accept;
    s_pair_snapshot = {};
    return true;
}

namespace rtos {

uint32_t nowMs() noexcept
{
    return static_cast<uint32_t>(s_time_us / 1000LL);
}

fw::Status fromEspErr(esp_err_t err) noexcept
{
    if (err == ESP_OK) {
        return fw::Status::kOk;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return fw::Status::kTimeout;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return fw::Status::kNotFound;
    }
    return fw::Status::kIoError;
}

} // namespace rtos

int64_t esp_timer_get_time(void)
{
    return s_time_us;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    CHECK(s_mutex == nullptr);
    s_mutex = storage;
    return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t /*ticks*/)
{
    CHECK(semaphore == s_mutex);
    if (semaphore->held) {
        return pdFALSE;
    }
    semaphore->held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore == s_mutex && semaphore->held);
    semaphore->held = false;
    return pdTRUE;
}

extern "C" {

void vTaskDelay(TickType_t ticks)
{
    s_time_us += static_cast<int64_t>(ticks) * 1000LL;
}

void vTaskDelete(TaskHandle_t /*task*/) {}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return nullptr;
}

void xTaskNotifyGive(TaskHandle_t /*task*/) {}

uint32_t ulTaskNotifyTake(BaseType_t /*clearOnExit*/, TickType_t /*ticks*/)
{
    return 0U;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t /*fn*/, const char * /*name*/,
                                           uint32_t /*stackDepth*/, void * /*arg*/,
                                           UBaseType_t /*prio*/, StackType_t * /*stack*/,
                                           StaticTask_t *tcb, BaseType_t /*core*/)
{
    return tcb;
}
void fake_crit_enter(void) {}

void fake_crit_exit(void) {}

} // extern "C"
int gpio_get_level(gpio_num_t /*gpio*/)
{
    return 1;
}

esp_err_t gpio_set_level(gpio_num_t /*gpio*/, uint32_t /*level*/)
{
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t * /*config*/)
{
    return ESP_OK;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t * /*config*/,
                             i2c_master_bus_handle_t *out)
{
    *out = &s_bus;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int /*timeoutMs*/)
{
    CHECK(bus == &s_bus && s_mutex->held);
    if (address != 0x3CU) {
        return ESP_ERR_NOT_FOUND;
    }
    return s_oled_probe_ok ? ESP_OK : s_oled_probe_err;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out)
{
    CHECK(bus == &s_bus && config->device_address == 0x3CU);
    s_oled_dev = {config->device_address, config->scl_speed_hz};
    *out = &s_oled_dev;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t /*handle*/)
{
    return ESP_OK;
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus)
{
    CHECK(bus == &s_bus && s_mutex->held);
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t count,
                              int timeoutMs)
{
    CHECK(dev == &s_oled_dev && s_mutex->held && count > 0U && timeoutMs > 0);
    if (s_assert_not_flushed_during_tx) {
        CHECK(!s_app.data.uiEvents.shutdownFinalFrameFlushed() &&
              !s_app.data.uiEvents.shutdownFinalFrameFlushed(false));
    }
    ++s_oled_tx_count;
    if (tx[0] == 0x00U) {
        if ((count == 4U) && ((tx[1] & 0xF8U) == 0xB0U)) {
            s_last_page_cmd = static_cast<uint8_t>(tx[1] & 0x07U);
        }
        return ESP_OK;
    }
    if (tx[0] == 0x40U) {
        CHECK(count == 129U);
        if (s_fail_all_data) {
            return ESP_ERR_TIMEOUT;
        }
        if ((s_fail_page_once >= 0) && (static_cast<int>(s_last_page_cmd) == s_fail_page_once)) {
            s_fail_page_once = -1;
            return ESP_ERR_TIMEOUT;
        }
        s_time_us += s_page_data_delay_us;
        if (s_short_key_during_page.has_value() && nowMs() >= *s_short_key_during_page) {
            s_app.data.uiEvents.notifyShortKey(*s_short_key_during_page);
            s_short_key_during_page.reset();
        }
        ++s_oled_data_pages;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

const char *esp_err_to_name(esp_err_t err)
{
    return (err == ESP_OK) ? "OK" : "ERR";
}

/**
 * @brief 执行真实 OledModule 编排主机测试场景
 * @param argc: 参数个数
 * @param argv: 场景名
 * @retval 0 通过
 */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::string_view scenario{argv[1]};
    CHECK(fw::isOk(fw::logInstallDirect(
        [](void *, const fw::LogHeader &hdr, std::span<const uint8_t>) noexcept {
            if (hdr.level == fw::LogLevel::kError) {
                ++s_error_logs;
            }
        },
        nullptr, &rtos::nowMs)));
    if (scenario == "final_frame_orchestration") {
        runFinalFrameOrchestration(true);
    } else if (scenario == "battery_final_frame") {
        runFinalFrameOrchestration(false);
    } else if (scenario == "comm_actions") {
        runCommActions();
    } else if (scenario == "busy_click_299_300_and_deadline_sleep") {
        runBusyClickAndDeadlineSleep();
    } else if (scenario == "click_page_deadline_299") {
        runClickPageDeadline(40299U);
    } else if (scenario == "click_page_deadline_300") {
        runClickPageDeadline(40300U);
    } else if (scenario == "offline_clears_oled_active") {
        runOfflineClearsOledActive();
    } else if (scenario == "background_init_retry") {
        runBackgroundInitRetry();
    } else if (scenario == "reboot_banner_quiesce") {
        runRebootBannerQuiesce();
    } else {
        std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
