// 主机单元测试：验证 OLED 页面格式化、翻页/关机状态机、WouoUI 渲染与“内容未变不刷新”逻辑（OL1）。
// 单线程主机测试，无硬件访问，非 ISR。
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "modules/oled/OledUi.hpp"

namespace {

using namespace app;
using namespace app::oled;

int s_failures = 0;

/**
 * @brief 累计并打印失败断言表达式
 * @param ok: 断言结果
 * @param expr: 表达式文本
 * @param line: 行号
 */
void check(bool ok, const char *expr, int line) noexcept
{
    if (!ok) {
        std::fprintf(stderr, "OledUiCheck:%d: FAIL %s\n", line, expr);
        ++s_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

/**
 * @brief 统计 8x128 帧缓冲中非零字节数，用于断言渲染产出有效像素
 * @param fb: 帧缓冲
 * @retval 非零字节数
 */
size_t countLitBytes(const ScreenBuff &fb) noexcept
{
    size_t count = 0U;
    for (size_t p = 0U; p < kPages; ++p) {
        for (size_t x = 0U; x < kWidth; ++x) {
            if (fb[p][x] != 0U) {
                ++count;
            }
        }
    }
    return count;
}

/** @brief 验证文本格式化、数字/单位、截断与缺外设占位。 */
void testFormatting() noexcept
{
    std::array<char, kMaxLineChars + 1U> buf{};

    formatGnssStateLine(false, gnss::State::kRunning, buf);
    CHECK(std::string_view{buf.data()} == "GNSS: OFF");
    formatGnssStateLine(true, gnss::State::kOff, buf);
    CHECK(std::string_view{buf.data()} == "GNSS: OFF");
    formatGnssStateLine(true, gnss::State::kProbing, buf);
    CHECK(std::string_view{buf.data()} == "GNSS: PROBE");
    formatGnssStateLine(true, gnss::State::kConfiguring, buf);
    CHECK(std::string_view{buf.data()} == "GNSS: CFG");
    formatGnssStateLine(true, gnss::State::kRunning, buf);
    CHECK(std::string_view{buf.data()} == "GNSS: RUN");
    formatGnssStateLine(true, gnss::State::kFailed, buf);
    CHECK(std::string_view{buf.data()} == "GNSS: FAIL");

    formatFixLine(false, gnss::State::kRunning, true, gnss::FixQuality::kRtkFixed, buf);
    CHECK(std::string_view{buf.data()} == "FIX : --");
    formatFixLine(true, gnss::State::kProbing, true, gnss::FixQuality::kRtkFixed, buf);
    CHECK(std::string_view{buf.data()} == "FIX : --");
    formatFixLine(true, gnss::State::kRunning, false, gnss::FixQuality::kUnknown, buf);
    CHECK(std::string_view{buf.data()} == "FIX : --");
    formatFixLine(true, gnss::State::kRunning, false, gnss::FixQuality::kInvalid, buf);
    CHECK(std::string_view{buf.data()} == "FIX : NONE");
    formatFixLine(true, gnss::State::kRunning, true, gnss::FixQuality::kSingle, buf);
    CHECK(std::string_view{buf.data()} == "FIX : SINGLE");
    formatFixLine(true, gnss::State::kRunning, true, gnss::FixQuality::kDgps, buf);
    CHECK(std::string_view{buf.data()} == "FIX : DGPS");
    formatFixLine(true, gnss::State::kRunning, true, gnss::FixQuality::kRtkFloat, buf);
    CHECK(std::string_view{buf.data()} == "FIX : FLOAT");
    formatFixLine(true, gnss::State::kRunning, true, gnss::FixQuality::kRtkFixed, buf);
    CHECK(std::string_view{buf.data()} == "FIX : FIXED");

    formatSatsAccLine(false, gnss::State::kRunning, true, 18U, 2U, buf);
    CHECK(std::string_view{buf.data()} == "SAT : --");
    formatSatsAccLine(true, gnss::State::kRunning, false, 18U, gnss::kU16Unknown, buf);
    CHECK(std::string_view{buf.data()} == "SAT : 0");
    formatSatsAccLine(true, gnss::State::kRunning, true, 18U, gnss::kU16Unknown, buf);
    CHECK(std::string_view{buf.data()} == "SAT : 18");
    formatSatsAccLine(true, gnss::State::kRunning, true, 18U, 2U, buf);
    CHECK(std::string_view{buf.data()} == "SAT : 18 0.02m");
    formatSatsAccLine(true, gnss::State::kRunning, true, 12U, 150U, buf);
    CHECK(std::string_view{buf.data()} == "SAT : 12 1.50m");

    formatBatteryShort(board::kBatteryPctUnknown, false, false, buf);
    CHECK(std::string_view{buf.data()} == "--%");
    formatBatteryShort(board::kBatteryPctUnknown, true, false, buf);
    CHECK(std::string_view{buf.data()} == "--%U");
    formatBatteryShort(board::kBatteryPctUnknown, true, true, buf);
    CHECK(std::string_view{buf.data()} == "--%+");
    formatBatteryShort(85U, false, false, buf);
    CHECK(std::string_view{buf.data()} == "85%");
    formatBatteryShort(85U, true, true, buf);
    CHECK(std::string_view{buf.data()} == "85%+");
    formatBatteryShort(100U, true, false, buf);
    CHECK(std::string_view{buf.data()} == "100%U");
    formatBatteryShort(120U, false, false, buf);
    CHECK(std::string_view{buf.data()} == "100%");

    formatBatteryVoltage(0U, buf);
    CHECK(std::string_view{buf.data()} == "--.-V");
    formatBatteryVoltage(4050U, buf);
    CHECK(std::string_view{buf.data()} == "4.05V");
    formatBatteryVoltage(3700U, buf);
    CHECK(std::string_view{buf.data()} == "3.70V");

    formatRadioShort(RadioMode::kWifiConfig, false, 0U, buf);
    CHECK(std::string_view{buf.data()} == "WIFI:AP");
    formatRadioShort(RadioMode::kBluetooth, false, 0U, buf);
    CHECK(std::string_view{buf.data()} == "BT:OFF");
    formatRadioShort(RadioMode::kBluetooth, true, 0U, buf);
    CHECK(std::string_view{buf.data()} == "BT:WAIT");
    formatRadioShort(RadioMode::kBluetooth, true, 1U, buf);
    CHECK(std::string_view{buf.data()} == "BT:CONN");

    // 截断、空值占位与非 ASCII 清洗。
    formatLabelValue("SN :", "", buf);
    CHECK(std::string_view{buf.data()} == "SN : --");
    formatLabelValue("SN :", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", buf);
    CHECK(std::string_view{buf.data()}.size() == kMaxLineChars);
    CHECK(std::string_view{buf.data()} == "SN : ABCDEFGHIJKLMNOP");
    constexpr std::array<char, 4U> kDirtyChars{'A', '\x01', '\x7F', 'B'};
    sanitizeCopy(std::string_view{kDirtyChars.data(), kDirtyChars.size()}, buf);
    CHECK(std::string_view{buf.data()} == "A??B");
}

/** @brief 验证开机自动切换、短按翻页、充电待机锁页与关机断电/超时恢复状态机。 */
void testPageStateMachine() noexcept
{
    PageSm sm{};
    // 1. 正常开机 + uint32 回绕安全。
    constexpr uint32_t kStart = 0xFFFFFC00U;
    sm.reset(board::BootMode::kNormal, kStart, 0U);
    CHECK(sm.page() == OledPage::kSplash);
    auto d = sm.step(kStart + PageSm::kSplashDurationMs - 1U, 0U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kSplash && !d.shouldPowerOff && !d.shouldWake);
    d = sm.step(kStart + PageSm::kSplashDurationMs, 0U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kStatus);

    // 2. 短按翻页循环 kStatus <-> kDeviceInfo。
    d = sm.step(kStart + 2000U, 1U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kDeviceInfo);
    d = sm.step(kStart + 2200U, 2U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kStatus);
    d = sm.step(kStart + 2400U, 4U, board::ShutdownStage::kIdle); // 偶数次回到原页
    CHECK(d.page == OledPage::kStatus);
    d = sm.step(kStart + 2600U, 5U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kDeviceInfo);

    // 3. 开机画面中短按立即跳过到状态页。
    sm.reset(board::BootMode::kNormal, 100U, 10U);
    CHECK(sm.page() == OledPage::kSplash);
    d = sm.step(200U, 11U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kStatus);

    // 4. 关机流程：kAnnounce 立即切关机页，kCutPower 返回精确剩余等待时间并在满 200 ms 后仅触发一次 shouldPowerOff，
    //    超时回退后触发 shouldWake；若唤醒失败则按 kWakeRetryBackoffMs 有界退避重试（ROL1-01 / ROL1-05）。
    d = sm.step(1000U, 11U, board::ShutdownStage::kAnnounce);
    CHECK(d.page == OledPage::kShutdown && !d.shouldPowerOff && d.nextWaitMs == 0U);
    d = sm.step(1050U, 12U, board::ShutdownStage::kSave); // 关机中短按忽略
    CHECK(d.page == OledPage::kShutdown && !d.shouldPowerOff && d.nextWaitMs == 0U);
    d = sm.step(1060U, 12U, board::ShutdownStage::kCutPower); // 剩余 140 ms，返回精确 nextWaitMs
    CHECK(d.page == OledPage::kShutdown && !d.shouldPowerOff && d.nextWaitMs == 140U);
    d = sm.step(1150U, 12U, board::ShutdownStage::kCutPower); // 剩余 50 ms
    CHECK(d.page == OledPage::kShutdown && !d.shouldPowerOff && d.nextWaitMs == 50U);
    d = sm.step(1200U, 12U, board::ShutdownStage::kCutPower); // 恰好 200 ms -> 关屏断电
    CHECK(d.page == OledPage::kShutdown && d.shouldPowerOff && d.nextWaitMs == 0U);
    d = sm.step(1400U, 12U, board::ShutdownStage::kCutPower); // 不重复触发
    CHECK(d.page == OledPage::kShutdown && !d.shouldPowerOff && d.nextWaitMs == 0U);
    d = sm.step(31500U, 12U, board::ShutdownStage::kIdle); // 30 s 超时回退
    CHECK(d.page == OledPage::kStatus && d.shouldWake && !d.shouldPowerOff);
    // 模拟面板唤醒瞬态失败 -> 退避窗内不重试，满 kWakeRetryBackoffMs 后自动重发 shouldWake。
    sm.onWakeFailed(31500U);
    d = sm.step(31700U, 12U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kStatus && !d.shouldWake);
    d = sm.step(31500U + PageSm::kWakeRetryBackoffMs - 1U, 12U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kStatus && !d.shouldWake);
    d = sm.step(31500U + PageSm::kWakeRetryBackoffMs, 12U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kStatus && d.shouldWake && !d.shouldPowerOff);
    d = sm.step(31500U + PageSm::kWakeRetryBackoffMs + 200U, 12U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kStatus && !d.shouldWake);

    // 5. 充电待机：只显示充电画面，忽略短按。
    sm.reset(board::BootMode::kChargeStandby, 500U, 0U);
    CHECK(sm.page() == OledPage::kChargeStandby);
    d = sm.step(5000U, 3U, board::ShutdownStage::kIdle);
    CHECK(d.page == OledPage::kChargeStandby && !d.shouldPowerOff);
}

/** @brief 验证 WouoUI 实际渲染产出有效像素及 FrameTracker “内容未变不刷新”逻辑。 */
void testRenderAndDirtySkip() noexcept
{
    ViewModel vm{};
    sanitizeCopy("P2", vm.product);
    sanitizeCopy("P2 v1.41", vm.boardName);
    sanitizeCopy("V1.41", vm.hwVer);
    sanitizeCopy("V3.1.0", vm.fwVer);
    sanitizeCopy("P2000123", vm.sn);
    vm.radioMode = RadioMode::kBluetooth;
    vm.btReady = true;
    vm.btSessions = 1U;
    vm.gnssEnabled = true;
    vm.gnssState = gnss::State::kRunning;
    vm.fixValid = true;
    vm.fixQuality = gnss::FixQuality::kRtkFixed;
    vm.satsUsed = 18U;
    vm.hAccCm = 2U;
    vm.batteryPct = 85U;
    vm.batteryMv = 4050U;
    vm.usbPresent = true;
    vm.charging = true;

    ScreenBuff fb{};
    ScreenBuff fbOther{};
    FrameTracker tracker{};
    std::array<uint64_t, kPages> hashes{};

    // 所有 5 个页面均能渲染出非空像素。
    for (const OledPage page : {OledPage::kSplash, OledPage::kStatus, OledPage::kDeviceInfo,
                                OledPage::kChargeStandby, OledPage::kShutdown}) {
        renderPage(page, vm, fb);
        CHECK(countLitBytes(fb) > 40U);
    }
    vm.shutdownReason = board::ShutdownReason::kLowBattery;
    renderPage(OledPage::kShutdown, vm, fbOther);
    CHECK(std::memcmp(fb, fbOther, sizeof(ScreenBuff)) != 0);

    // 首次提交全部 8 页脏。
    renderPage(OledPage::kStatus, vm, fb);
    uint8_t dirty = tracker.computeDirtyMask(fb, hashes, false);
    CHECK(dirty == 0xFFU);
    for (uint8_t p = 0U; p < kPages; ++p) {
        tracker.commitPage(p, hashes[p]);
    }

    // 内容未变再次渲染：脏掩码必须为 0（零 I2C 刷新）。
    renderPage(OledPage::kStatus, vm, fb);
    dirty = tracker.computeDirtyMask(fb, hashes, false);
    CHECK(dirty == 0x00U);

    // 强制全刷时返回 0xFF。
    CHECK(tracker.computeDirtyMask(fb, hashes, true) == 0xFFU);

    // 仅改变卫星数：只脏受影响的页（严格子集，非 0 且非全屏）。
    vm.satsUsed = 21U;
    renderPage(OledPage::kStatus, vm, fb);
    dirty = tracker.computeDirtyMask(fb, hashes, false);
    CHECK(dirty != 0x00U && dirty != 0xFFU);

    // 模拟部分页写失败（未 commitPage）：再次检查仍保持脏，自动重试。
    const uint8_t failedDirty = tracker.computeDirtyMask(fb, hashes, false);
    CHECK(failedDirty == dirty);
    for (uint8_t p = 0U; p < kPages; ++p) {
        if ((dirty & static_cast<uint8_t>(1U << p)) != 0U) {
            tracker.commitPage(p, hashes[p]);
        }
    }
    CHECK(tracker.computeDirtyMask(fb, hashes, false) == 0x00U);

    // invalidate 后全部重刷。
    tracker.invalidate();
    CHECK(tracker.computeDirtyMask(fb, hashes, false) == 0xFFU);
}

/** @brief 验证 UiEventBus 短按计数、唤醒钩子注册与解绑。 */
void testUiEventBus() noexcept
{
    UiEventBus bus{};
    uint32_t wakes = 0U;
    CHECK(bus.shortKeyCount() == 0U);
    bus.notifyShortKey();
    CHECK(bus.shortKeyCount() == 1U && wakes == 0U);

    bus.setWakeHook(
        [](void *ctx) noexcept {
            // 测试上下文固定为 uint32_t*。
            ++(*static_cast<uint32_t *>(ctx));
        },
        &wakes);
    bus.notifyShortKey();
    CHECK(bus.shortKeyCount() == 2U && wakes == 1U);
    bus.notifyWake();
    CHECK(bus.shortKeyCount() == 2U && wakes == 2U);

    bus.setWakeHook(nullptr, nullptr);
    bus.notifyShortKey();
    CHECK(bus.shortKeyCount() == 3U && wakes == 2U);
}

} // namespace

/**
 * @brief 执行 OLED UI 格式化、翻页状态机与脏页刷新主机用例
 * @retval EXIT_SUCCESS 全部通过
 * @retval EXIT_FAILURE 存在失败用例
 */
int main()
{
    testFormatting();
    testPageStateMachine();
    testRenderAndDirtySkip();
    testUiEventBus();
    if (s_failures != 0) {
        std::printf("oled_ui_check: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    std::printf("oled_ui_check: OK\n");
    return EXIT_SUCCESS;
}
