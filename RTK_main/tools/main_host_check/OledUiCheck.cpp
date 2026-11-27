// 主机单元测试（OL2）：双击/短按消歧、开机自检状态机、关机进度与 USB 终帧、
// 定点缓动/弹簧/滚动数字、33 ms/静止 0 写帧调度、全页面/过渡渲染与脏页跳过。
// 单线程主机测试，无硬件访问，非 ISR。
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "board/PowerLogic.hpp"
#include "def/NetStatusText.hpp"
#include "modules/oled/OledAnim.hpp"
#include "modules/oled/OledIcons.hpp"
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

/**
 * @brief 比较两帧指定页与列区间内的像素是否完全一致（用于独立校验单路滚动数字区域，R2OL2-05）
 * @param lhs: 帧缓冲 A
 * @param rhs: 帧缓冲 B
 * @param page: 页索引（0..7）
 * @param colBegin: 起始列（含）
 * @param colEnd: 结束列（不含）
 * @retval true 该区域像素逐字节相等
 * @retval false 存在像素差异
 */
bool regionEqual(const ScreenBuff &lhs, const ScreenBuff &rhs, size_t page, size_t colBegin,
                 size_t colEnd) noexcept
{
    for (size_t x = colBegin; x < colEnd; ++x) {
        if (lhs[page][x] != rhs[page][x]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 构造全部外设就绪且含完整 GNSS/卫星/电池信息的基线 ViewModel
 * @retval 填充好的 ViewModel
 */
ViewModel makeHealthyVm() noexcept
{
    ViewModel vm{};
    sanitizeCopy("P2", vm.product);
    sanitizeCopy("P2 v1.41", vm.boardName);
    sanitizeCopy("V1.41", vm.hwVer);
    sanitizeCopy("V3.1.0", vm.fwVer);
    sanitizeCopy("P2000123", vm.sn);
    sanitizeCopy("P2-P2000123", vm.deviceName);
    sanitizeCopy("R4.10", vm.gnssFwVer);
    vm.uptimeMs = 3723000U; // 01:02:03
    vm.radioMode = RadioMode::kBluetooth;
    vm.nvsReady = true;
    vm.btReady = true;
    vm.btSessions = 1U;
    vm.powerActive = true;
    vm.batteryAdcReady = true;
    vm.chargerPresent = true;
    vm.chargerOk = true;
    vm.gnssEnabled = true;
    vm.gnssState = gnss::State::kRunning;
    vm.gnssModel = gnss::Model::kUm980;
    vm.fixValid = true;
    vm.fixQuality = gnss::FixQuality::kRtkFixed;
    vm.latE9 = 31230415000LL;
    vm.lonE9 = 121473701000LL;
    vm.altMm = 12340;
    vm.satsUsed = 18U;
    vm.hdopX100 = 85U;
    vm.diffAgeDs = 12U;
    vm.hAccCm = 2U;
    vm.batteryPct = 85U;
    vm.batteryMv = 4050U;
    vm.usbPresent = true;
    vm.charging = true;

    gnss::SatTable table{};
    table.count = 6U;
    table.sats[0] = {.system = gnss::Constellation::kGps, .prn = 1U, .snrDbhz = 45U, .used = true};
    table.sats[1] = {.system = gnss::Constellation::kGps, .prn = 2U, .snrDbhz = 38U, .used = false};
    table.sats[2] = {
        .system = gnss::Constellation::kBeidou, .prn = 11U, .snrDbhz = 48U, .used = true};
    table.sats[3] = {
        .system = gnss::Constellation::kBeidou, .prn = 12U, .snrDbhz = 41U, .used = true};
    table.sats[4] = {
        .system = gnss::Constellation::kGlonass, .prn = 21U, .snrDbhz = 35U, .used = true};
    table.sats[5] = {
        .system = gnss::Constellation::kGalileo, .prn = 31U, .snrDbhz = 42U, .used = false};
    summarizeSatTable(table, vm);
    return vm;
}

/** @brief 验证按生产时间戳消歧双击/短按（299 vs 300 ms 边界、OLED 消费滞后、回绕、连按、长按不误判，R2OL2-03）。 */
void testClickDetectorAndLongPress() noexcept
{
    ClickDetector det{};
    CHECK(!det.pending() && det.remainingMs(0U) == 0U);

    // 1. 单击：t=100 产生，t=399（299 ms）仍等待，t=400（300 ms）到期确认 kSingle。
    CHECK(det.onPress(100U) == ClickEvent::kNone);
    CHECK(det.pending() && det.deadlineMs() == 400U &&
          det.remainingMs(100U) == kDoubleClickWindowMs);
    CHECK(det.pollTimeout(399U) == ClickEvent::kNone);
    CHECK(det.remainingMs(399U) == 1U);
    CHECK(det.pollTimeout(400U) == ClickEvent::kSingle);
    CHECK(!det.pending());

    // 2. 双击边界（299 ms）且消费滞后（t=1315 才处理）：先消费 onPress(1299) 仍判 kDouble。
    CHECK(det.onPress(1000U) == ClickEvent::kNone);
    CHECK(det.onPress(1299U) == ClickEvent::kDouble);
    CHECK(det.pollTimeout(1315U) == ClickEvent::kNone);
    CHECK(!det.pending());

    // 3. 恰好 300 ms 到期又按一次：前一次判 kSingle，新按压以 2300 开启下一轮窗口（截止 2600）。
    CHECK(det.onPress(2000U) == ClickEvent::kNone);
    CHECK(det.onPress(2300U) == ClickEvent::kSingle);
    CHECK(det.pending() && det.deadlineMs() == 2600U &&
          det.remainingMs(2300U) == kDoubleClickWindowMs);
    CHECK(det.pollTimeout(2600U) == ClickEvent::kSingle);
    CHECK(!det.pending());

    // 4. uint32_t 回绕安全：0xFFFFFF00 -> 0x0000002B (299 ms) 仍判双击。
    const uint32_t wrapStart = 0xFFFFFF00U;
    CHECK(det.onPress(wrapStart) == ClickEvent::kNone);
    CHECK(det.onPress(wrapStart + kDoubleClickWindowMs - 1U) == ClickEvent::kDouble);

    // 5. 同轮队列到达 2 次短按（相隔 120 ms）：顺序消费后直接判双击。
    CHECK(det.onPress(5000U) == ClickEvent::kNone);
    CHECK(det.onPress(5120U) == ClickEvent::kDouble);

    // 6. 与 PowerKeySm 联调：2.5 s 长按触发 kLongHold，松开不发 kShort，ClickDetector 绝不误判。
    board::PowerKeySm keySm{board::kDefaultLongPressMs};
    CHECK(keySm.poll(0U, false) == board::KeyEvent::kNone);
    CHECK(keySm.poll(30U, false) == board::KeyEvent::kNone); // armed
    CHECK(keySm.poll(100U, true) == board::KeyEvent::kNone);
    CHECK(keySm.poll(130U, true) == board::KeyEvent::kNone);
    CHECK(keySm.poll(2600U, true) == board::KeyEvent::kLongHold);
    // 松开后不产生短按。
    CHECK(keySm.poll(2800U, false) == board::KeyEvent::kNone);
    CHECK(keySm.poll(2830U, false) == board::KeyEvent::kNone);
    CHECK(det.pollTimeout(2830U) == ClickEvent::kNone);
}

/** @brief 验证定点 ease-out cubic 单调性与终值、定点弹簧收敛与数字滚动状态。 */
void testFixedPointMathAndAnim() noexcept
{
    CHECK(easeOutCubic(0U, 300U) == 0U);
    CHECK(easeOutCubic(300U, 300U) == kEaseScale);
    CHECK(easeOutCubic(500U, 300U) == kEaseScale);
    CHECK(easeOutCubic(10U, 0U) == kEaseScale);

    uint16_t prev = 0U;
    for (uint32_t t = 0U; t <= 1024U; ++t) {
        const uint16_t cur = easeOutCubic(t, 1024U);
        CHECK(cur >= prev);
        CHECK(cur <= kEaseScale);
        prev = cur;
    }
    CHECK(prev == kEaseScale);

    // 缓出特性：中点 t=50% 时进度应明显大于 50%（1 - 0.5^3 = 0.875 = 896）。
    CHECK(easeOutCubic(150U, 300U) >= 890U && easeOutCubic(150U, 300U) <= 902U);
    CHECK(lerpFixed(2, 98, 0U) == 2);
    CHECK(lerpFixed(2, 98, kEaseScale) == 98);
    CHECK(lerpFixed(98, 2, kEaseScale) == 2);

    // 定点弹簧：覆盖 dt=1/2/16/33 ms 与小/大、正/负位移，校验逐帧单调不回退、无过冲且必收敛（ROL2-04）。
    constexpr std::array<uint32_t, 4U> kDts{1U, 2U, 16U, kAnimFramePeriodMs};
    constexpr std::array<int16_t, 7U> kDeltas{1, 5, 10, 32, 96, -10, -96};
    for (const uint32_t dt : kDts) {
        for (const int16_t delta : kDeltas) {
            FixedSpring sp{};
            const int16_t startPx = 100;
            const int16_t targetPx = static_cast<int16_t>(startPx + delta);
            sp.snap(startPx);
            CHECK(sp.position() == startPx && sp.settled());
            sp.retarget(targetPx);
            CHECK(!sp.settled());
            int16_t prevPx = startPx;
            uint32_t elapsed = 0U;
            while (!sp.settled() && (elapsed < 800U)) {
                sp.step(dt);
                elapsed += dt;
                const int16_t curPx = sp.position();
                if (delta > 0) {
                    CHECK(curPx >= prevPx && curPx <= targetPx);
                } else {
                    CHECK(curPx <= prevPx && curPx >= targetPx);
                }
                prevPx = curPx;
            }
            CHECK(sp.settled() && sp.position() == targetPx);
        }
    }
    // NumberRoll 首次喂入不播动画，数值变化后 180 ms 内活跃，到期静止。
    NumberRoll nr{};
    nr.update(12, 100U);
    CHECK(!nr.active(100U) && nr.current() == 12);
    nr.update(15, 200U);
    CHECK(nr.active(200U) && nr.previous() == 12 && nr.current() == 15 && nr.increasing());
    CHECK(nr.active(379U));
    CHECK(!nr.active(380U));
}

/** @brief 验证开机自检状态机：全部成功、GNSS 失败不阻塞并挂告警、充电芯片缺失显示 "-" 不算失败。 */
void testSystemCheckStateMachine() noexcept
{
    // 1. 全部成功路径（含最短展示时间验证）。
    ViewModel vm = makeHealthyVm();
    SystemCheckSm sm{};
    sm.reset(1000U);
    CHECK(sm.phase() == CheckPhase::kLogo);
    CHECK(sm.logoRevealQ10(1000U) == 0U);
    CHECK(sm.logoRevealQ10(1450U) == kEaseScale);

    // Logo 期结束（650 ms）进入 kChecking。
    CHECK(!sm.step(1649U, vm));
    CHECK(sm.phase() == CheckPhase::kLogo);
    CHECK(!sm.step(1650U, vm));
    CHECK(sm.phase() == CheckPhase::kChecking && sm.currentIndex() == 0U);

    // 逐步推进 5 项，每项至少保持 kMinStepMs (200 ms <= 300 ms)。
    uint32_t t = 1650U;
    for (size_t i = 0U; i < kCheckItemCount; ++i) {
        CHECK(!sm.step(t, vm));
        CHECK(sm.itemStatus(i) == CheckStatus::kOk);
        CHECK(!sm.step(t + SystemCheckSm::kMinStepMs - 1U, vm));
        CHECK(sm.currentIndex() == i);
        t += SystemCheckSm::kMinStepMs;
        CHECK(!sm.step(t, vm));
    }
    CHECK(sm.phase() == CheckPhase::kSummary);
    CHECK(!sm.hasWarning());
    CHECK(sm.step(t + SystemCheckSm::kSummaryHoldMs, vm));
    CHECK(sm.phase() == CheckPhase::kDone);

    // 2. 充电芯片缺失（chargerPresent=false, chargerOk=false）显示 "-" 且不算失败告警。
    vm.chargerPresent = false;
    vm.chargerOk = false;
    sm.reset(0U);
    for (uint32_t now = 0U; now <= 3000U; now += 33U) {
        if (sm.step(now, vm)) {
            break;
        }
    }
    CHECK(sm.phase() == CheckPhase::kDone);
    CHECK(sm.itemStatus(3U) == CheckStatus::kAbsent);
    CHECK(!sm.hasWarning());

    // 3. GNSS 探测先 Pending 后 Failed：不阻塞启动，完成后置 hasWarning=true。
    vm = makeHealthyVm();
    vm.gnssState = gnss::State::kProbing;
    sm.reset(0U);
    uint32_t now = 0U;
    while (now < 2000U) {
        static_cast<void>(sm.step(now, vm));
        now += 50U;
    }
    // 前 4 项已完成，第 5 项（GNSS）正处于 Pending 等待探测。
    CHECK(sm.phase() == CheckPhase::kChecking && sm.currentIndex() == 4U);
    CHECK(sm.itemStatus(4U) == CheckStatus::kPending);

    // GNSS 控制器探测超时进入 kFailed -> 立刻判 kFail 并推进至完成。
    vm.gnssState = gnss::State::kFailed;
    bool finished = false;
    for (unsigned i = 0U; i < 30U; ++i) {
        now += 33U;
        if (sm.step(now, vm)) {
            finished = true;
            break;
        }
    }
    CHECK(finished && sm.itemStatus(4U) == CheckStatus::kFail && sm.hasWarning());

    // 4. GNSS 未启用（gnssEnabled=false）：显示 "-" 且不算失败告警。
    vm = makeHealthyVm();
    vm.gnssEnabled = false;
    sm.reset(0U);
    for (uint32_t tOff = 0U; tOff <= 3000U; tOff += 33U) {
        if (sm.step(tOff, vm)) {
            break;
        }
    }
    CHECK(sm.phase() == CheckPhase::kDone);
    CHECK(sm.itemStatus(4U) == CheckStatus::kAbsent && !sm.hasWarning());
}

/** @brief 验证表盘导航、双击展开/收回子菜单、帧调度（33 ms vs 200 ms）、关机进度与超时回退。 */
void testControllerNavigationAndShutdown() noexcept
{
    constexpr std::span<const uint32_t> kNoKeys{};
    const auto oneKey = [](uint32_t t) noexcept { return std::array<uint32_t, 1U>{t}; };

    ViewModel vm = makeHealthyVm();
    UiController ctrl{};
    ctrl.reset(board::BootMode::kNormal, 0U);
    CHECK(ctrl.page() == OledPage::kSystemCheck);

    // 自检期间帧周期应为 33 ms（动画 30 fps）。
    auto dec = ctrl.step(33U, kNoKeys, vm);
    CHECK(dec.animating && dec.nextWaitMs == kAnimFramePeriodMs);

    // 推完自检并自动滑入表盘（启动 kSlideLeft 300 ms 过渡）。
    uint32_t now = 66U;
    while ((ctrl.page() == OledPage::kSystemCheck) && (now < 4000U)) {
        dec = ctrl.step(now, kNoKeys, vm);
        now += 33U;
    }
    CHECK(ctrl.page() == OledPage::kDialplate);
    CHECK(dec.animating && dec.nextWaitMs == kAnimFramePeriodMs);

    // 过渡结束后静止：animating=false，进入事件等待。
    now += kPageTransitionMs + 33U;
    dec = ctrl.step(now, kNoKeys, vm);
    CHECK(!dec.animating && dec.nextWaitMs == kWaitForEvent);

    // 短按一次：等待双击窗口时记录绝对截止并缩短 nextWaitMs（250 ms 后剩余 50 ms，ROL2-02/R2OL2-03），
    // 满 300 ms 确认单击后选中框切换至入口 1（弹簧激活 33 ms）。
    const uint32_t pressAt = now + 10U;
    const auto k1 = oneKey(pressAt);
    dec = ctrl.step(pressAt, k1, vm);
    CHECK(ctrl.dockSelected() == 0U && !dec.animating && dec.hasClickDeadline &&
          dec.clickDeadlineMs == (pressAt + kDoubleClickWindowMs) &&
          dec.nextWaitMs == kDoubleClickWindowMs);
    dec = ctrl.step(pressAt + 250U, kNoKeys, vm);
    CHECK(ctrl.dockSelected() == 0U && !dec.animating && dec.hasClickDeadline &&
          dec.nextWaitMs == 50U);
    dec = ctrl.step(pressAt + kDoubleClickWindowMs, kNoKeys, vm);
    CHECK(ctrl.dockSelected() == 1U && dec.animating && !dec.hasClickDeadline &&
          dec.nextWaitMs == kAnimFramePeriodMs);
    // 弹簧收敛后回到静止等待。
    now += 10U + kDoubleClickWindowMs;
    for (unsigned i = 0U; i < 20U; ++i) {
        now += kAnimFramePeriodMs;
        dec = ctrl.step(now, kNoKeys, vm);
    }
    CHECK(!dec.animating && dec.nextWaitMs == kWaitForEvent);

    // R2OL2-03：OLED 正忙滞后消费场景——两次按压间隔 299 ms（now 与 now+299），
    // 第二次晚 16 ms（now+315）才被消费，仍须判为双击进入位置子菜单（kSubPos）。
    now += 100U;
    const auto k2a = oneKey(now);
    dec = ctrl.step(now, k2a, vm);
    CHECK(ctrl.page() == OledPage::kDialplate);
    const auto k2b = oneKey(now + 299U);
    dec = ctrl.step(now + 315U, k2b, vm);
    CHECK(ctrl.page() == OledPage::kSubPos && dec.animating &&
          dec.nextWaitMs == kAnimFramePeriodMs);

    // 再次双击返回表盘（收回动画）。
    now += 600U;
    const auto k3a = oneKey(now);
    dec = ctrl.step(now, k3a, vm);
    const auto k3b = oneKey(now + 100U);
    dec = ctrl.step(now + 100U, k3b, vm);
    CHECK(ctrl.page() == OledPage::kDialplate && dec.animating);

    // 关机进度：按阶段与 fence 推进；无 USB 保持亮屏，USB 终帧触发 markShutdownFinalFrame，超时恢复表盘。
    now += 500U;
    vm.shutdownStage = board::ShutdownStage::kAnnounce;
    vm.shutdownDoneMask = board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced);
    dec = ctrl.step(now, kNoKeys, vm);
    CHECK(dec.page == OledPage::kShutdown && !dec.markShutdownFinalFrame && !dec.hasClickDeadline);

    vm.shutdownStage = board::ShutdownStage::kSave;
    vm.shutdownDoneMask |= board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFences);
    const auto kIgnored = oneKey(now + 50U);
    dec = ctrl.step(now + 50U, kIgnored, vm); // 关机中短按忽略
    CHECK(dec.page == OledPage::kShutdown && !dec.markShutdownFinalFrame);
    CHECK(shutdownTargetPercent(vm.shutdownStage, vm.shutdownDoneMask) == 45U);

    vm.shutdownStage = board::ShutdownStage::kQuiesce;
    vm.shutdownDoneMask = static_cast<uint8_t>(
        vm.shutdownDoneMask | board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed) |
        board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe) |
        board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced));
    dec = ctrl.step(now + 100U, kNoKeys, vm);
    CHECK(shutdownTargetPercent(vm.shutdownStage, vm.shutdownDoneMask) == 80U);
    CHECK(std::string_view{shutdownStageText(vm.shutdownStage, vm.shutdownDoneMask, 0U,
                                             RadioMode::kBluetooth)} == "SCAN REQ OK");
    CHECK(std::string_view{shutdownStageText(vm.shutdownStage, vm.shutdownDoneMask, 0U,
                                             RadioMode::kWifiConfig)} == "AP MODE OFF");
    CHECK(std::string_view{shutdownStageText(vm.shutdownStage, 0U, 0U, RadioMode::kBluetooth)} ==
          "CLOSING SPP");

    // R2OL2-02：保存/射频超时与安全电平失败必须输出对应降级文案而非伪装正常。
    CHECK(std::string_view{shutdownStageText(
              board::ShutdownStage::kQuiesce,
              board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced),
              board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout),
              RadioMode::kBluetooth)} == "SAVE TIMEOUT");
    CHECK(std::string_view{shutdownStageText(
              board::ShutdownStage::kCutPower,
              board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced) |
                  board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower),
              board::shutdownDegradedBit(board::ShutdownDegradedBit::kRadioTimeout),
              RadioMode::kBluetooth)} == "RF TIMEOUT");
    CHECK(std::string_view{shutdownStageText(
              board::ShutdownStage::kCutPower,
              board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced) |
                  board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower) |
                  board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady),
              board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed),
              RadioMode::kBluetooth)} == "SAFE IO ERR");

    // 无 USB 到达 kCutPower：不触发 markShutdownFinalFrame，保持亮屏。
    vm.shutdownStage = board::ShutdownStage::kCutPower;
    vm.shutdownDoneMask |= board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower);
    dec = ctrl.step(now + 150U, kNoKeys, vm);
    CHECK(dec.page == OledPage::kShutdown && !dec.markShutdownFinalFrame);

    // USB 路径全部完成进入 kUsbOffReady：进度 100%，置位 markShutdownFinalFrame。
    vm.shutdownDoneMask |= board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady);
    dec = ctrl.step(now + 180U, kNoKeys, vm);
    CHECK(dec.page == OledPage::kShutdown && dec.markShutdownFinalFrame);
    CHECK(ctrl.renderState(now + 180U).shutdownPct == 100U);
    // PW1 A：由参与者完成数/总数派生的进度必须单调非降（15..92 线性映射），且终帧就绪到达 100%。
    const uint8_t announcedOnly = board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced);
    CHECK(shutdownTargetPercent(board::ShutdownStage::kSave, announcedOnly, 2U, 8U) == 34U);
    CHECK(shutdownTargetPercent(board::ShutdownStage::kQuiesce, announcedOnly, 6U, 8U) == 72U);
    CHECK(shutdownTargetPercent(board::ShutdownStage::kCutPower,
                                board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady), 7U,
                                8U) == 100U);
    CHECK(shutdownTargetPercent(board::ShutdownStage::kCutPower,
                                board::shutdownDoneBit(board::ShutdownDoneBit::kBatteryOffReady),
                                7U, 8U) == 100U);

    // 评审 7：kCutPower 未到终态（终帧未就绪）时进度保持 92%，不得被未完成计数拉回 83%。
    CHECK(shutdownTargetPercent(board::ShutdownStage::kCutPower, announcedOnly, 7U, 8U) == 92U);
    CHECK(shutdownTargetPercent(board::ShutdownStage::kCutPower, announcedOnly, 0U, 8U) == 92U);
    CHECK(shutdownTargetPercent(board::ShutdownStage::kCutPower, announcedOnly, 0U, 0U) == 92U);

    // 30 s 超时回退至 kIdle：恢复表盘。
    vm.shutdownStage = board::ShutdownStage::kIdle;
    vm.shutdownDoneMask = 0U;
    dec = ctrl.step(now + 30500U, kNoKeys, vm);
    CHECK(dec.page == OledPage::kDialplate);

    // ROL2-06：在 Logo/自检期双击跳过时 GNSS 仍在 kProbing，随后 GNSS 失败也必须补挂状态栏告警图标。
    UiController skipCtrl{};
    ViewModel skipVm = makeHealthyVm();
    skipVm.gnssState = gnss::State::kProbing;
    skipCtrl.reset(board::BootMode::kNormal, 0U);
    const auto s1 = oneKey(100U);
    const auto s2 = oneKey(200U);
    dec = skipCtrl.step(100U, s1, skipVm);
    dec = skipCtrl.step(200U, s2, skipVm);
    CHECK(skipCtrl.page() == OledPage::kDialplate);
    CHECK(!skipCtrl.hasWarning() && !skipCtrl.renderState(200U).hasCheckWarning);
    skipVm.gnssState = gnss::State::kFailed;
    dec = skipCtrl.step(400U, kNoKeys, skipVm);
    CHECK(skipCtrl.hasWarning() && skipCtrl.renderState(400U).hasCheckWarning);

    // ROL2-03 / R2OL2-05：仅 kWritten/kSkipped 置位终帧完成；kError/kBackoff 不置位并缩短至 2 ms 重试。
    UiDecision finalDec{
        .page = OledPage::kShutdown,
        .markShutdownFinalFrame = true,
        .animating = false,
        .nextWaitMs = kWaitForEvent,
    };
    const PostFlushDecision onErr = settleFrameWait(finalDec, FlushOutcome::kError, 8000U, 1000U);
    CHECK(!onErr.markFinalFlushed && onErr.nextWaitMs == kShutdownFinalRetryMs);
    const PostFlushDecision onBackoff =
        settleFrameWait(finalDec, FlushOutcome::kBackoff, 1000U, 1000U);
    CHECK(!onBackoff.markFinalFlushed && onBackoff.nextWaitMs == kShutdownFinalRetryMs);
    const PostFlushDecision onDisabled =
        settleFrameWait(finalDec, FlushOutcome::kDisabled, 1000U, 1000U);
    CHECK(!onDisabled.markFinalFlushed && onDisabled.nextWaitMs == kWaitForEvent);
    const PostFlushDecision onWritten =
        settleFrameWait(finalDec, FlushOutcome::kWritten, 18000U, 1000U);
    CHECK(onWritten.markFinalFlushed && onWritten.nextWaitMs == kWaitForEvent);
    const PostFlushDecision onSkipped =
        settleFrameWait(finalDec, FlushOutcome::kSkipped, 1000U, 1000U);
    CHECK(onSkipped.markFinalFlushed && onSkipped.nextWaitMs == kWaitForEvent);
    UiDecision animDec{
        .page = OledPage::kDialplate,
        .markShutdownFinalFrame = false,
        .animating = true,
        .nextWaitMs = kAnimFramePeriodMs,
    };
    CHECK(settleFrameWait(animDec, FlushOutcome::kWritten, 988U, 1000U).nextWaitMs ==
          kWaitForEvent);
    CHECK(settleFrameWait(animDec, FlushOutcome::kWritten, 950U, 1000U).nextWaitMs ==
          kWaitForEvent);

    // R2OL2-03：空闲帧休眠前按双击绝对截止重算剩余时间（t=1250 决定睡 50 ms，刷屏耗 20 ms 到 t=1270 -> 仅睡 30 ms；超期到 1305 -> 睡 0 ms）。
    UiDecision idleClickDec{
        .page = OledPage::kDialplate,
        .markShutdownFinalFrame = false,
        .animating = false,
        .hasClickDeadline = true,
        .clickDeadlineMs = 1300U,
        .nextWaitMs = 50U,
    };
    CHECK(settleFrameWait(idleClickDec, FlushOutcome::kWritten, 1250U, 1270U).nextWaitMs == 30U);
    CHECK(settleFrameWait(idleClickDec, FlushOutcome::kWritten, 1250U, 1305U).nextWaitMs == 0U);
}

/** @brief 验证新增纯整数格式化函数（经纬高、HDOP/差分龄期、运行时长、解状态徽章与卫星摘要）。 */
void testFormattingAndSatSummary() noexcept
{
    std::array<char, 32U> buf{};

    formatCoordE9(31230415000LL, 'N', 'S', true, buf);
    CHECK(std::string_view{buf.data()} == "31.230415N");
    formatCoordE9(-121473701000LL, 'E', 'W', true, buf);
    CHECK(std::string_view{buf.data()} == "121.473701W");
    formatCoordE9(0, 'N', 'S', false, buf);
    CHECK(std::string_view{buf.data()} == "--.------");

    formatAltMm(12340, true, buf);
    CHECK(std::string_view{buf.data()} == "12.34m");
    formatAltMm(-5200, true, buf);
    CHECK(std::string_view{buf.data()} == "-5.20m");
    formatAltMm(0, false, buf);
    CHECK(std::string_view{buf.data()} == "--.--m");

    formatHdopAge(85U, 12U, true, buf);
    CHECK(std::string_view{buf.data()} == "HDOP 0.85 AGE 1.2s");
    formatHdopAge(gnss::kU16Unknown, gnss::kU16Unknown, true, buf);
    CHECK(std::string_view{buf.data()} == "HDOP -- AGE --");

    formatUptime(3723000U, buf);
    CHECK(std::string_view{buf.data()} == "01:02:03");
    formatUptime((86400U * 2U + 3U * 3600U + 15U * 60U) * 1000U, buf);
    CHECK(std::string_view{buf.data()} == "2d 03:15");

    ViewModel vm = makeHealthyVm();
    CHECK(isGnssRunningLike(gnss::State::kRunning) && isGnssRunningLike(gnss::State::kReadback));
    CHECK(!isGnssRunningLike(gnss::State::kOff) && !isGnssRunningLike(gnss::State::kPowerUp));
    CHECK(!isGnssRunningLike(gnss::State::kOutput) &&
          !isGnssRunningLike(gnss::State::kMaintenance));
    CHECK(std::string_view{dialHeroBadge(vm)} == "FIX");
    vm.gnssState = gnss::State::kReadback;
    CHECK(std::string_view{dialHeroBadge(vm)} == "FIX");
    vm.gnssState = gnss::State::kPowerUp;
    CHECK(std::string_view{dialHeroBadge(vm)} == "BOOT");
    vm.gnssState = gnss::State::kProbing;
    CHECK(std::string_view{dialHeroBadge(vm)} == "PROBE");
    vm.gnssState = gnss::State::kConfiguring;
    CHECK(std::string_view{dialHeroBadge(vm)} == "CFG");
    vm.gnssState = gnss::State::kOutput;
    CHECK(std::string_view{dialHeroBadge(vm)} == "CFG");
    vm.gnssState = gnss::State::kMaintenance;
    CHECK(std::string_view{dialHeroBadge(vm)} == "CFG");
    vm.gnssState = gnss::State::kFailed;
    CHECK(std::string_view{dialHeroBadge(vm)} == "FAIL");
    vm.gnssState = gnss::State::kOff;
    CHECK(std::string_view{dialHeroBadge(vm)} == "OFF");
    vm.gnssState = gnss::State::kRunning;
    vm.fixQuality = gnss::FixQuality::kRtkFloat;
    CHECK(std::string_view{dialHeroBadge(vm)} == "FLOAT");
    vm.fixQuality = gnss::FixQuality::kSingle;
    CHECK(std::string_view{dialHeroBadge(vm)} == "SPS");
    vm.workModeActive = true;
    vm.workMode = WorkModeKind::kBase;
    vm.workModeState = WorkModeState::kBaseTx;
    CHECK(std::string_view{dialHeroBadge(vm)} == "BASE");
    // 卫星摘要：6 颗可见，Top-8 按 SNR 降序排列，首个为 BDS 48 dB-Hz。
    CHECK(vm.satsVisible == 6U);
    CHECK(vm.satGroups[0].visible == 2U && vm.satGroups[0].used == 1U);
    CHECK(vm.satGroups[1].visible == 2U && vm.satGroups[1].used == 2U);
    CHECK(vm.topSnrCount == 6U && vm.topSnr[0].snrDbhz == 48U && vm.topSnr[0].sysLetter == 'C');
}

/** @brief 验证全页面与三种过渡动画渲染非空像素及 FrameTracker 脏页跳过逻辑。 */
void testRenderAllPagesAndDirtySkip() noexcept
{
    ViewModel vm = makeHealthyVm();
    ScreenBuff fb{};
    ScreenBuff fb2{};
    FrameTracker tracker{};
    FrameTracker::PageHashes hashes{};

    // 1. 全部 8 种页面静止渲染非空。
    for (const OledPage page : {OledPage::kSystemCheck, OledPage::kDialplate, OledPage::kSubSat,
                                OledPage::kSubPos, OledPage::kSubDevice, OledPage::kSubPower,
                                OledPage::kChargeStandby, OledPage::kShutdown}) {
        renderPage(page, vm, fb);
        CHECK(countLitBytes(fb) > 40U);
    }

    // 2. 卫星子菜单 CN0 标签页非空且与星座柱状图不同。
    UiRenderState st{};
    st.page = OledPage::kSubSat;
    st.satSubTab = 0U;
    renderUi(st, vm, fb);
    st.satSubTab = 1U;
    renderUi(st, vm, fb2);
    CHECK(countLitBytes(fb2) > 40U && std::memcmp(fb, fb2, sizeof(ScreenBuff)) != 0);

    // 3. 三种过渡动画中点帧均产出非空画面。
    PageTransition trans{};
    st.nowMs = 150U;
    st.transition = &trans;
    trans.start(TransitionKind::kSlideLeft, OledPage::kSystemCheck, OledPage::kDialplate, {}, 0U);
    renderUi(st, vm, fb);
    CHECK(countLitBytes(fb) > 40U);

    trans.start(TransitionKind::kExpandFromIcon, OledPage::kDialplate, OledPage::kSubSat,
                Rect{.x = 2, .y = 44, .w = 28, .h = 19}, 0U);
    renderUi(st, vm, fb);
    CHECK(countLitBytes(fb) > 40U);

    trans.start(TransitionKind::kCollapseToIcon, OledPage::kSubSat, OledPage::kDialplate,
                Rect{.x = 2, .y = 44, .w = 28, .h = 19}, 0U);
    renderUi(st, vm, fb);
    CHECK(countLitBytes(fb) > 40U);

    // 4. 静止 0 写与局部脏页验证。
    st.transition = nullptr;
    st.page = OledPage::kDialplate;
    st.dockFocusX = 2;
    renderUi(st, vm, fb);
    for (uint8_t page = 0U; page < kPages; ++page) {
        const auto cols = tracker.dirtyColumns(page, fb[page], hashes);
        CHECK(cols.first == 0U && cols.end == kWidth);
        tracker.commitPage(page, hashes);
    }
    renderUi(st, vm, fb);
    for (uint8_t page = 0U; page < kPages; ++page) {
        CHECK(tracker.dirtyColumns(page, fb[page], hashes).end == 0U);
    }

    // 变化的卫星数须被顶部页列组覆盖。
    vm.satsUsed = 22U;
    renderUi(st, vm, fb);
    CHECK(tracker.dirtyColumns(0U, fb[0], hashes).end != 0U);
    CHECK(tracker.dirtyColumns(7U, fb[7], hashes).end == 0U);

    // 5. ROL2-02：覆盖活跃 SystemCheckSm（kChecking 含 kOk/kFail/kAbsent/kPending+waitS>=1s 与 kSummary 含 GNSS 型号固件）。
    ViewModel chkVm = makeHealthyVm();
    chkVm.nvsReady = false;
    chkVm.chargerPresent = false;
    chkVm.chargerOk = false;
    chkVm.gnssState = gnss::State::kProbing;
    SystemCheckSm chkSm{};
    chkSm.reset(0U);
    uint32_t chkNow = 0U;
    while ((chkSm.phase() != CheckPhase::kChecking || chkSm.currentIndex() < 4U) &&
           (chkNow < 3000U)) {
        static_cast<void>(chkSm.step(chkNow, chkVm));
        chkNow += 50U;
    }
    CHECK(chkSm.phase() == CheckPhase::kChecking && chkSm.currentIndex() == 4U);
    CHECK(chkSm.itemStatus(0U) == CheckStatus::kOk && chkSm.itemStatus(1U) == CheckStatus::kFail &&
          chkSm.itemStatus(3U) == CheckStatus::kAbsent &&
          chkSm.itemStatus(4U) == CheckStatus::kPending);
    UiRenderState chkSt{};
    chkSt.page = OledPage::kSystemCheck;
    chkSt.checkSm = &chkSm;
    chkSt.nowMs = chkNow;
    renderUi(chkSt, chkVm, fb);
    CHECK(countLitBytes(fb) > 80U);
    // 等待满 1 s 后渲染，应画出等待秒数与旋转符，画面与 0 s 帧不同。
    chkNow += 1200U;
    static_cast<void>(chkSm.step(chkNow, chkVm));
    chkSt.nowMs = chkNow;
    renderUi(chkSt, chkVm, fb2);
    CHECK(countLitBytes(fb2) > 80U && std::memcmp(fb, fb2, sizeof(ScreenBuff)) != 0);
    // GNSS 落地进入 kSummary，显示型号 + 固件短串且与无固件文本画面不同。
    chkVm.gnssState = gnss::State::kRunning;
    while ((chkSm.phase() != CheckPhase::kSummary) && (chkNow < 6000U)) {
        chkNow += 50U;
        static_cast<void>(chkSm.step(chkNow, chkVm));
    }
    CHECK(chkSm.phase() == CheckPhase::kSummary && chkSm.itemStatus(4U) == CheckStatus::kOk);
    chkSt.nowMs = chkNow;
    renderUi(chkSt, chkVm, fb);
    ViewModel noFwVm = chkVm;
    noFwVm.gnssFwVer[0] = '\0';
    renderUi(chkSt, noFwVm, fb2);
    CHECK(countLitBytes(fb) > 80U && std::memcmp(fb, fb2, sizeof(ScreenBuff)) != 0);

    // 6. R2OL2-05：独立分区域断言卫星数与电量滚动（含方向差异与终值一致），任一路动画被删均会直接失败。
    constexpr size_t kSatColBegin = 10U;
    constexpr size_t kSatColEnd = 30U;
    constexpr size_t kBatColBegin = 100U;
    constexpr size_t kBatColEnd = 128U;
    ViewModel rollVm = makeHealthyVm();
    rollVm.satsUsed = 18U;
    rollVm.batteryPct = 72U;

    // 6a. 仅卫星数滚动（12->18 递增 vs 24->18 递减）：卫星区域中点 != 终帧、递增 != 递减、终帧 == 静态无滚帧，且电量区域不受影响。
    NumberRoll satsUp{};
    satsUp.update(12, 0U);
    satsUp.update(18, 100U);
    NumberRoll satsDown{};
    satsDown.update(24, 0U);
    satsDown.update(18, 100U);
    UiRenderState satSt{};
    satSt.page = OledPage::kDialplate;
    satSt.satsRoll = &satsUp;
    satSt.nowMs = 160U;
    renderUi(satSt, rollVm, fb);
    satSt.nowMs = 350U;
    renderUi(satSt, rollVm, fb2);
    CHECK(!regionEqual(fb, fb2, 0U, kSatColBegin, kSatColEnd));
    CHECK(regionEqual(fb, fb2, 0U, kBatColBegin, kBatColEnd));
    ScreenBuff fbDir{};
    satSt.satsRoll = &satsDown;
    satSt.nowMs = 160U;
    renderUi(satSt, rollVm, fbDir);
    CHECK(!regionEqual(fb, fbDir, 0U, kSatColBegin, kSatColEnd));
    satSt.satsRoll = nullptr;
    renderUi(satSt, rollVm, fbDir);
    CHECK(regionEqual(fb2, fbDir, 0U, kSatColBegin, kSatColEnd));

    // 6b. OL7：电量只用 5 格表示，表头电量区域随格数变化；百分比数字不再绘制（同格数、不同百分比像素一致）。
    UiRenderState batSt{};
    batSt.page = OledPage::kDialplate;
    batSt.batBars = board::BatteryBarsView{.bars = 4U};
    renderUi(batSt, rollVm, fb);
    batSt.batBars = board::BatteryBarsView{.bars = 2U};
    renderUi(batSt, rollVm, fb2);
    CHECK(!regionEqual(fb, fb2, 0U, kBatColBegin, kBatColEnd));
    CHECK(regionEqual(fb, fb2, 0U, kSatColBegin, kSatColEnd));
    batSt.batBars = board::BatteryBarsView{.bars = 4U};
    ViewModel samePctBars = rollVm;
    samePctBars.batteryPct = 61U;
    renderUi(batSt, samePctBars, fbDir);
    CHECK(std::memcmp(fb, fbDir, sizeof(ScreenBuff)) == 0);
    // 低电闪烁熄灭相位少画最后一格；未知（0 格）仅画外框。
    batSt.batBars = board::BatteryBarsView{.bars = 1U, .blinking = true, .blinkOff = true};
    renderUi(batSt, rollVm, fb);
    batSt.batBars = board::BatteryBarsView{.bars = 1U, .blinking = true, .blinkOff = false};
    renderUi(batSt, rollVm, fb2);
    CHECK(!regionEqual(fb, fb2, 0U, kBatColBegin, kBatColEnd));
    batSt.batBars = board::BatteryBarsView{};
    renderUi(batSt, rollVm, fbDir);
    CHECK(regionEqual(fb, fbDir, 0U, kBatColBegin, kBatColEnd));

    // 7. ROL2-05 / R2OL2-06：kReadback 与 kRunning 一致；非运行态按枚举分别展示精准右侧文案（非笼统 GNSS WAIT）。
    ViewModel rbVm = makeHealthyVm();
    UiRenderState rbSt{};
    rbSt.page = OledPage::kDialplate;
    rbVm.gnssState = gnss::State::kRunning;
    renderUi(rbSt, rbVm, fb);
    rbVm.gnssState = gnss::State::kReadback;
    renderUi(rbSt, rbVm, fb2);
    CHECK(std::memcmp(fb, fb2, sizeof(ScreenBuff)) == 0);

    rbVm.gnssState = gnss::State::kOff;
    CHECK(std::string_view{dialGnssDetail(rbVm).bot} == "DISABLED");
    rbVm.gnssState = gnss::State::kPowerUp;
    CHECK(std::string_view{dialGnssDetail(rbVm).bot} == "POWER UP");
    rbVm.gnssState = gnss::State::kProbing;
    CHECK(std::string_view{dialGnssDetail(rbVm).bot} == "PROBING");
    rbVm.gnssState = gnss::State::kConfiguring;
    CHECK(std::string_view{dialGnssDetail(rbVm).bot} == "CONFIG");
    rbVm.gnssState = gnss::State::kOutput;
    CHECK(std::string_view{dialGnssDetail(rbVm).bot} == "SET OUT");
    renderUi(rbSt, rbVm, fb);
    rbVm.gnssState = gnss::State::kMaintenance;
    CHECK(std::string_view{dialGnssDetail(rbVm).bot} == "MAINT");
    renderUi(rbSt, rbVm, fb2);
    // 左侧徽章同为 "CFG"，右侧区域必须因 SET OUT vs MAINT 产生像素差异。
    CHECK(!regionEqual(fb, fb2, 3U, 69U, 128U));
    rbVm.gnssState = gnss::State::kFailed;
    CHECK(std::string_view{dialGnssDetail(rbVm).bot} == "FAULT");

    // 8. R2OL2-02：关机页降级渲染（SAVE TIMEOUT / S:!）必须与正常关机完成帧（S:+）不同。
    ViewModel shutVm = makeHealthyVm();
    shutVm.shutdownStage = board::ShutdownStage::kCutPower;
    shutVm.shutdownDoneMask =
        static_cast<uint8_t>(board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced) |
                             board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed) |
                             board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced) |
                             board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe) |
                             board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower));
    UiRenderState shutSt{};
    shutSt.page = OledPage::kShutdown;
    shutSt.shutdownPct = 92U;
    renderUi(shutSt, shutVm, fb);
    shutVm.shutdownDoneMask =
        static_cast<uint8_t>(board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced) |
                             board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower));
    shutVm.shutdownDegradedMask =
        board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout);
    renderUi(shutSt, shutVm, fb2);
    CHECK(std::memcmp(fb, fb2, sizeof(ScreenBuff)) != 0);
}

/** @brief 验证 33 ms 采样的缓动步长及展开、收回的镜像几何。 */
void testTransitionPose() noexcept
{
    constexpr Rect screen{0, 0, 128, 64};
    constexpr Rect icon{102, 44, 24, 19};
    PageTransition open{};
    PageTransition close{};
    PageTransition slide{};
    open.start(TransitionKind::kExpandFromIcon, OledPage::kDialplate, OledPage::kSubPower, icon,
               0U);
    close.start(TransitionKind::kCollapseToIcon, OledPage::kSubPower, OledPage::kDialplate, icon,
                0U);
    slide.start(TransitionKind::kSlideLeft, OledPage::kSystemCheck, OledPage::kDialplate, {}, 0U);
    CHECK(open.durationMs == 420U);
    CHECK(open.pose(0U, screen).viewport == icon);
    CHECK(open.pose(40U, screen).contentOffsetY == kTransitionContentLiftPx);
    CHECK(open.rowOffsetY(0U, 0U) == kTransitionContentLiftPx);
    CHECK(open.rowOffsetY(40U, 0U) == kTransitionContentLiftPx);
    CHECK(open.rowOffsetY(80U, 1U) == kTransitionContentLiftPx);
    CHECK(open.rowOffsetY(kPageTransitionMs, 2U) == 0);
    CHECK(open.rowOffsetY(180U, 1U) == close.rowOffsetY(kPageTransitionMs - 180U, 1U));
    CHECK(open.pose(kPageTransitionMs, screen).viewport == screen);
    CHECK(open.pose(kPageTransitionMs, screen).contentOffsetY == 0);
    CHECK(open.progressQ10(kPageTransitionMs / 2U) == kEaseScale / 2U);
    CHECK(open.progressQ10(40U) < 8U);
    auto prev = icon;
    int16_t lastSlide = screen.w;
    for (uint32_t at = 0U; at <= kPageTransitionMs + kAnimFramePeriodMs; at += kAnimFramePeriodMs) {
        const uint32_t time = std::min(at, kPageTransitionMs);
        const auto pose = open.pose(time, screen);
        const auto reverse = close.pose(kPageTransitionMs - time, screen);
        CHECK(pose.viewport == reverse.viewport && pose.contentOffsetY == reverse.contentOffsetY);
        CHECK(pose.viewport.x <= prev.x && prev.x - pose.viewport.x <= 32);
        CHECK(pose.viewport.y <= prev.y && prev.y - pose.viewport.y <= 16);
        CHECK(pose.viewport.w >= prev.w && pose.viewport.w - prev.w <= 32);
        const int16_t nextSlide = slide.pose(time, screen).viewport.x;
        CHECK(nextSlide <= lastSlide && lastSlide - nextSlide <= 32);
        prev = pose.viewport;
        lastSlide = nextSlide;
    }
}

/** @brief 验证绝对节拍、整数时间回绕、超时跳帧及 2 秒性能统计。 */
void testPacingAndPerf() noexcept
{
    FramePacer pacer{};
    CHECK(pacer.waitMs(1000U) == kWaitForEvent);
    pacer.advance(1000U, 1012U);
    CHECK(pacer.waitMs(1012U) == 22U);
    CHECK(pacer.waitMs(1020U) == 14U);
    pacer.advance(1020U, 1021U);
    CHECK(pacer.waitMs(1021U) == 13U);
    pacer.advance(1034U, 1059U);
    CHECK(pacer.waitMs(1059U) == 8U);
    pacer.advance(1067U, 1112U);
    CHECK(pacer.waitMs(1112U) == 22U);

    FramePacer wrap{};
    wrap.advance(UINT32_MAX - 10U, UINT32_MAX - 3U);
    CHECK(wrap.waitMs(0U) == 23U);
    CHECK(wrap.waitMs(23U) == 0U);

    PerfWindow perf{};
    for (uint32_t i = 0U; i < 60U; ++i) {
        perf.record(1000U + (i * 100U / 3U), 2500U, true, true);
    }
    const auto sample = perf.recent(2999U);
    CHECK(sample.fpsX10(kPerfWindowMs) == 300U);
    CHECK(sample.wakeupsX10(kPerfWindowMs) == 300U);
    CHECK(sample.cpuX10(kPerfWindowMs) == 75U);
    CHECK(perf.recent(5000U).frames == 0U);
}

/** @brief 顶栏像素在转场中复用，即使此时电池变化也只修改内容区。 */
void testTransitionRetainsStatusBar() noexcept
{
    auto vm = makeHealthyVm();
    ScreenBuff fb{};
    UiRenderState state{.page = OledPage::kDialplate};
    CHECK(renderUi(state, vm, fb));
    std::array<uint8_t, kWidth> header{};
    std::ranges::copy(fb[0], header.begin());
    PageTransition transition{};
    transition.start(TransitionKind::kExpandFromIcon, OledPage::kDialplate, OledPage::kSubPower, {},
                     1000U);
    state.page = OledPage::kSubPower;
    state.transition = &transition;
    state.nowMs = 1250U;
    state.batBars = board::BatteryBarsView{.bars = 1U}; // 电量格变化（OL7 不再画百分比）
    CHECK(renderUi(state, vm, fb, true));
    CHECK(std::ranges::equal(fb[0], header));
    state.transition = nullptr;
    CHECK(renderUi(state, vm, fb, true));
    CHECK(!std::ranges::equal(fb[0], header));
}

/** @brief 验证通信操作、配对弹窗的局部状态和返回页。 */
void testCommAndPairing() noexcept
{
    auto vm = makeHealthyVm();
    vm.wifiSupported = true;
    UiController ui{};
    ui.reset(board::BootMode::kNormal, 0U);
    const auto twice = [&](uint32_t now) {
        const std::array<uint32_t, 2U> keys{now - 100U, now};
        return ui.step(now, keys, vm);
    };
    const auto once = [&](uint32_t press) {
        const std::array<uint32_t, 1U> keys{press};
        return ui.step(press + kDoubleClickWindowMs, keys, vm);
    };
    CHECK(twice(200U).page == OledPage::kDialplate);
    static_cast<void>(once(1000U));
    static_cast<void>(once(1400U));
    CHECK(twice(1900U).page == OledPage::kSubComm);
    CHECK(twice(2500U).action.action == OledAction::kNone);
    CHECK(ui.renderState(2500U).modal == UiModal::kWifiConfirm);
    CHECK(ui.renderState(2500U).modalFocusX == 0);

    // 三次单击依次选择 OK、Cancel、OK，第四次回到 Cancel。静止时待单击或双击通知。
    for (uint32_t i = 0U; i < 4U; ++i) {
        const uint32_t press = 2600U + i * 600U;
        const uint32_t changeMs = press + kDoubleClickWindowMs;
        const auto selection = once(press);
        CHECK(selection.animating && selection.action.action == OledAction::kNone);
        const auto moving = ui.step(changeMs + kAnimFramePeriodMs, {}, vm);
        const auto pos = ui.renderState(changeMs + kAnimFramePeriodMs).modalFocusX;
        CHECK(moving.animating && pos > 0 && pos < kModalOptionStepX);
        for (uint32_t t = changeMs + 2U * kAnimFramePeriodMs; t <= changeMs + 264U;
             t += kAnimFramePeriodMs) {
            static_cast<void>(ui.step(t, {}, vm));
        }
        const auto settled = ui.step(changeMs + 280U, {}, vm);
        CHECK(ui.renderState(changeMs + 280U).modal == UiModal::kWifiConfirm);
        CHECK(ui.renderState(changeMs + 280U).modalFocusX ==
              ((i & 1U) == 0U ? kModalOptionStepX : 0));
        CHECK(!settled.animating);
    }
    CHECK(twice(5200U).action.action == OledAction::kNone);
    CHECK(ui.renderState(5200U).modal == UiModal::kNone);
    CHECK(twice(5500U).action.action == OledAction::kNone);
    CHECK(once(5600U).action.action == OledAction::kNone);
    CHECK(twice(6200U).action.action == OledAction::kWifi);
    CHECK(ui.renderState(6200U).modal == UiModal::kNone);

    static_cast<void>(once(6300U));
    CHECK(twice(6800U).action.action == OledAction::kTogglePair);
    vm.pairing.id = 41U;
    vm.pairing.requestDeadlineMs = 37000U;
    CHECK(twice(7000U).action.action == OledAction::kNone);
    CHECK(ui.renderState(7000U).modal == UiModal::kPair);
    CHECK(ui.renderState(7000U).modalFocusX == 0);
    CHECK(once(7100U).action.action == OledAction::kNone);
    const auto accept = twice(7700U).action;
    CHECK(accept.action == OledAction::kPairAccept && accept.pairId == 41U);
    CHECK(ui.renderState(7700U).modal == UiModal::kNone);
    CHECK(twice(8000U).action.action == OledAction::kNone);
    vm.pairing.id = 42U;
    CHECK(ui.step(8100U, {}, vm).action.action == OledAction::kNone);
    const auto reject = twice(8400U).action;
    CHECK(reject.action == OledAction::kPairReject && reject.pairId == 42U);
    CHECK(ui.renderState(8400U).modal == UiModal::kNone);

    // 选中 Accept 也不能越过配对绝对截止，底层 SPP worker 按超时拒绝。
    vm.pairing.id = 43U;
    vm.pairing.requestDeadlineMs = 39000U;
    CHECK(ui.step(9000U, {}, vm).action.action == OledAction::kNone);
    CHECK(once(9200U).action.action == OledAction::kNone);
    for (uint32_t t = 9533U; t < 9800U; t += kAnimFramePeriodMs) {
        static_cast<void>(ui.step(t, {}, vm));
    }
    CHECK(ui.step(38999U, {}, vm).nextWaitMs == 1U);
    CHECK(ui.renderState(38999U).modalFocusX == kModalOptionStepX);
    CHECK(twice(39000U).action.action == OledAction::kNone);
    CHECK(ui.renderState(39000U).modal == UiModal::kNone);

    // BT 保持 4 项。Wi-Fi 展示 SSID 与地址，Back 仍可达，Mode 动作保持现有语义。
    static_cast<void>(once(39200U));
    static_cast<void>(once(39600U));
    CHECK(twice(40100U).page == OledPage::kDialplate);
    vm.radioMode = RadioMode::kWifiConfig;
    CHECK(twice(40700U).page == OledPage::kSubComm);
    CHECK(twice(41300U).action.action == OledAction::kBluetooth);
    for (uint32_t i = 0U; i < 3U; ++i) {
        static_cast<void>(once(41400U + i * 400U));
        CHECK(ui.subSelected() == i + 1U);
    }
    CHECK(twice(42700U).page == OledPage::kDialplate);
}

/** @brief 验证弹窗选中框只改变底部按钮页，配对名称晚到时重绘提示区。 */
void testModalDirtyPages() noexcept
{
    auto vm = makeHealthyVm();
    vm.pairing.id = 7U;
    vm.pairing.hasNumber = true;
    vm.pairing.number = 123456U;
    sanitizeCopy("Phone", vm.pairing.name);
    UiRenderState state{.modal = UiModal::kPair};
    ScreenBuff fb{};
    ModalFrame history{};
    renderUi(state, vm, fb, false, &history);
    FrameTracker tracker{};
    FrameTracker::PageHashes hashes{};
    for (uint8_t page = 0U; page < kPages; ++page) {
        static_cast<void>(tracker.dirtyColumns(page, fb[page], hashes));
        tracker.commitPage(page, hashes);
    }
    state.modalFocusX = kModalOptionStepX;
    vm.pairing.requestDeadlineMs += 100U;
    renderUi(state, vm, fb, false, &history);
    constexpr uint8_t kFirstChoicesPage = 6U;
    for (uint8_t page = 0U; page < kFirstChoicesPage; ++page) {
        CHECK(tracker.dirtyColumns(page, fb[page], hashes).end == 0U);
    }
    CHECK(tracker.dirtyColumns(kFirstChoicesPage, fb[kFirstChoicesPage], hashes).end != 0U);
    sanitizeCopy("New Name", vm.pairing.name);
    renderUi(state, vm, fb, false, &history);
    constexpr uint8_t kPairNamePage = 2U;
    CHECK(tracker.dirtyColumns(kPairNamePage, fb[kPairNamePage], hashes).end != 0U);
}

/** @brief 验证 UiEventBus 短按时间戳队列、OLED 在线标志、关机凭据和 kLink 动作。 */
void testUiEventBus() noexcept
{
    UiEventBus bus{};
    CHECK(!bus.oledActive() && !bus.shutdownFinalFrameFlushed());
    uint32_t ts = 0U;
    CHECK(!bus.popShortKey(ts));
    bus.notifyShortKey(1000U);
    bus.notifyShortKey(1299U);
    CHECK(bus.shortKeyCount() == 2U);
    CHECK(bus.popShortKey(ts) && ts == 1000U);
    CHECK(bus.popShortKey(ts) && ts == 1299U);
    CHECK(!bus.popShortKey(ts));
    bus.notifyShortKey(2000U);
    bus.drainShortKeys();
    CHECK(!bus.popShortKey(ts));

    unsigned restartWakes = 0U;
    bus.setRestartWakeHook([](void *arg) noexcept { ++(*static_cast<unsigned *>(arg)); },
                           &restartWakes);
    bus.setOledActive(true);
    CHECK(bus.oledActive() && !bus.shutdownFinalFrameFlushed());
    bus.markShutdownFinalFrameFlushed();
    CHECK(bus.oledActive() && bus.shutdownFinalFrameFlushed() && restartWakes == 1U);
    bus.resetShutdownFinalFrame();
    CHECK(bus.oledActive() && !bus.shutdownFinalFrameFlushed());
    bus.setRestartWakeHook(nullptr, nullptr);
    bus.setOledActive(false);
    CHECK(!bus.oledActive());
    CHECK(bus.submitAction({OledAction::kPairAccept, 11U}));
    CHECK(!bus.submitAction({OledAction::kPairReject, 12U}));
    const auto request = bus.takeAction();
    CHECK(request.action == OledAction::kPairAccept && request.pairId == 11U);
    CHECK(bus.takeAction().action == OledAction::kNone);
    bus.notifyLongKey(3000U);
    CHECK(bus.longKeyCount() == 1U);
    CHECK(bus.popLongKey(ts) && ts == 3000U && !bus.popLongKey(ts));
    bus.updateKeyHold(true, 4000U, 2500U, false);
    const KeyHoldSnapshot holdSnap = bus.readKeyHold();
    CHECK(holdSnap.active && !holdSnap.standbyConfirm && holdSnap.startMs == 4000U &&
          holdSnap.thresholdMs == 2500U);
    bus.updateKeyHold(false, 4000U, 2500U, true);
    const KeyHoldSnapshot wakeSnap = bus.readKeyHold();
    CHECK(!wakeSnap.active && wakeSnap.standbyConfirm);
}

/** @brief 验证 OL4 长按 600..2500 ms 进度浮层、提前松开收起、POWER OFF? 确认/取消/5 s 超时与配对排队。 */
void testHoldOverlayAndPowerOffConfirm() noexcept
{
    ViewModel vm = makeHealthyVm();
    UiController ui{};
    ui.reset(board::BootMode::kNormal, 0U);
    const auto twice = [&](uint32_t now) {
        const std::array<uint32_t, 2U> keys{now - 80U, now};
        return ui.step(now, keys, vm);
    };
    const auto once = [&](uint32_t press) {
        const std::array<uint32_t, 1U> keys{press};
        return ui.step(press + kDoubleClickWindowMs, keys, vm);
    };
    CHECK(twice(200U).page == OledPage::kDialplate);

    // 1. 按住 < 600 ms：不显浮层，精准预约 600 ms 唤醒。
    vm.keyHold = {.active = true, .standbyConfirm = false, .startMs = 1000U, .thresholdMs = 2500U};
    auto dec = ui.step(1400U, {}, vm);
    CHECK(!ui.renderState(1400U).holdOverlayVisible && dec.nextWaitMs <= 200U);
    dec = ui.step(1550U, {}, vm);
    CHECK(!ui.renderState(1550U).holdOverlayVisible && dec.nextWaitMs == 50U);

    // 2. 按住 600..2499 ms：浮层线性填充，提前松开不关机且进入 150 ms 收起动画。
    dec = ui.step(1600U, {}, vm);
    CHECK(ui.renderState(1600U).holdOverlayVisible && ui.renderState(1600U).holdProgressQ10 == 0U);
    dec = ui.step(2550U, {}, vm);
    CHECK(ui.renderState(2550U).holdOverlayVisible &&
          ui.renderState(2550U).holdProgressQ10 == 512U);
    vm.keyHold.active = false;
    dec = ui.step(2600U, {}, vm);
    CHECK(dec.action.action == OledAction::kNone && ui.renderState(2600U).holdOverlayVisible &&
          ui.renderState(2600U).holdOverlayCollapsing);
    dec = ui.step(2750U, {}, vm);
    CHECK(!ui.renderState(2750U).holdOverlayVisible &&
          !ui.renderState(2750U).holdOverlayCollapsing);

    // 3. 长按到达 2500 ms：进入 POWER OFF? 确认页而非直接关机；期间配对弹窗排队不抢占。
    dec = ui.step(3000U, {}, vm, true);
    CHECK(dec.action.action == OledAction::kNone &&
          ui.renderState(3000U).modal == UiModal::kPowerOffConfirm);
    vm.pairing.id = 88U;
    vm.pairing.requestDeadlineMs = 30000U;
    dec = ui.step(3200U, {}, vm);
    CHECK(ui.renderState(3200U).modal == UiModal::kPowerOffConfirm);
    // OL7：关机弹窗只有一个确认项；5 s 超时取消后同一轮显示排队中的配对弹窗，焦点不偏移。
    dec = ui.step(7999U, {}, vm);
    CHECK(ui.renderState(7999U).modal == UiModal::kPowerOffConfirm);
    dec = ui.step(8000U, {}, vm);
    CHECK(dec.action.action == OledAction::kNone && ui.renderState(8000U).modal == UiModal::kPair);
    CHECK(ui.renderState(8000U).modalFocusX == 0);
    vm.pairing.id = 0U;
    static_cast<void>(ui.step(8100U, {}, vm));

    // 4. 5 s 倒计时超时自动取消，且不触发关机。
    dec = ui.step(10000U, {}, vm, true);
    CHECK(ui.renderState(10000U).modal == UiModal::kPowerOffConfirm);
    CHECK(ui.renderState(12000U).powerOffRemainMs == 3000U);
    dec = ui.step(14999U, {}, vm);
    CHECK(ui.renderState(14999U).modal == UiModal::kPowerOffConfirm);
    dec = ui.step(15000U, {}, vm);
    CHECK(ui.renderState(15000U).modal == UiModal::kNone && dec.action.action == OledAction::kNone);

    // 5. 单击确认（经双击窗口消歧后）提交 OledAction::kShutdownKey。
    dec = ui.step(16000U, {}, vm, true);
    CHECK(ui.renderState(16000U).modal == UiModal::kPowerOffConfirm);
    // 按下当刻与窗口到期前 1 ms 都不得提前确认（等待双击消歧）。
    const std::array<uint32_t, 1U> pressKey{16100U};
    dec = ui.step(16100U, pressKey, vm);
    CHECK(dec.action.action == OledAction::kNone &&
          ui.renderState(16100U).modal == UiModal::kPowerOffConfirm);
    dec = ui.step(16100U + kDoubleClickWindowMs - 1U, {}, vm);
    CHECK(dec.action.action == OledAction::kNone &&
          ui.renderState(16100U + kDoubleClickWindowMs - 1U).modal == UiModal::kPowerOffConfirm);
    dec = ui.step(16100U + kDoubleClickWindowMs, {}, vm);
    CHECK(dec.action.action == OledAction::kShutdownKey &&
          ui.renderState(16100U + kDoubleClickWindowMs).modal == UiModal::kNone);
    // 双击同样确认。
    dec = ui.step(18000U, {}, vm, true);
    CHECK(ui.renderState(18000U).modal == UiModal::kPowerOffConfirm);
    dec = twice(18800U);
    CHECK(dec.action.action == OledAction::kShutdownKey &&
          ui.renderState(18800U).modal == UiModal::kNone);

    // 6. 电源子页 Power off 项（索引 3）双击同样进入 POWER OFF? 确认页。
    // 从 dock 0 单击 4 次选到 Power(4)，双击进入 kSubPower。
    static_cast<void>(once(21000U));
    static_cast<void>(once(21400U));
    static_cast<void>(once(21800U));
    static_cast<void>(once(22200U));
    CHECK(twice(22700U).page == OledPage::kSubPower);
    static_cast<void>(once(23200U));
    static_cast<void>(once(23600U));
    static_cast<void>(once(24000U));
    CHECK(ui.subSelected() == 3U);
    dec = twice(24500U);
    CHECK(ui.renderState(24500U).modal == UiModal::kPowerOffConfirm);
    CHECK(once(24600U).action.action == OledAction::kShutdownKey);
}

/** @brief 验证 OL4 充电待机页唤醒环进度、松开回落、<= 2 fps 充电动画与充满静止 0 写入。 */
void testStandbyRingAndChargeAnim() noexcept
{
    ViewModel vm = makeHealthyVm();
    vm.bootMode = board::BootMode::kChargeStandby;
    vm.usbPresent = true;
    vm.charging = true;
    vm.batteryPct = 76U;
    vm.batteryMv = 4020U;

    UiController ui{};
    ui.reset(board::BootMode::kChargeStandby, 0U);
    auto dec = ui.step(0U, {}, vm);
    CHECK(dec.page == OledPage::kChargeStandby && !dec.animating &&
          dec.nextWaitMs == kChargeAnimPeriodMs);

    // 充电中 500 ms 步进（<= 2 fps），帧内容有流动变化。
    ScreenBuff fb0{};
    ScreenBuff fb1{};
    renderUi(ui.renderState(0U), vm, fb0);
    dec = ui.step(500U, {}, vm);
    CHECK(dec.nextWaitMs == kChargeAnimPeriodMs);
    renderUi(ui.renderState(500U), vm, fb1);
    CHECK(std::memcmp(fb0, fb1, sizeof(ScreenBuff)) != 0);

    // 充满后停止动画，恢复 200 ms 静止轮询且 0 像素变化。
    vm.batteryPct = 100U;
    vm.charging = false;
    static_cast<void>(ui.step(1000U, {}, vm));
    dec = ui.step(1300U, {}, vm); // 等待 NumberRoll 完成
    CHECK(!dec.animating && dec.nextWaitMs == kWaitForEvent);
    renderUi(ui.renderState(1300U), vm, fb0);
    renderUi(ui.renderState(1800U), vm, fb1);
    CHECK(std::memcmp(fb0, fb1, sizeof(ScreenBuff)) == 0);

    // 按住按键：待机唤醒环实时填充，松开后平滑回落至 0。
    vm.keyHold = {.active = true, .standbyConfirm = false, .startMs = 2000U, .thresholdMs = 2500U};
    dec = ui.step(3250U, {}, vm);
    CHECK(dec.animating && dec.nextWaitMs == kAnimFramePeriodMs &&
          ui.renderState(3250U).standbyRingQ10 == 512U);
    renderUi(ui.renderState(3250U), vm, fb1);
    CHECK(std::memcmp(fb0, fb1, sizeof(ScreenBuff)) != 0);

    vm.keyHold.active = false;
    for (uint32_t t = 3283U; t <= 3600U; t += 33U) {
        dec = ui.step(t, {}, vm);
    }
    CHECK(ui.renderState(3600U).standbyRingQ10 == 0U && !dec.animating);

    // HIL-1: 长按触发待机开机确认（standbyConfirm=true）后即使松开物理按键，环锁定 100% 不回退且显示 STARTING。
    vm.keyHold = {.active = false, .standbyConfirm = true, .startMs = 2000U, .thresholdMs = 2500U};
    ScreenBuff fbHold{};
    ScreenBuff fbStarting{};
    vm.keyHold.standbyConfirm = false;
    vm.keyHold.active = true;
    static_cast<void>(ui.step(4500U, {}, vm));
    renderUi(ui.renderState(4500U), vm, fbHold);
    vm.keyHold.active = false;
    vm.keyHold.standbyConfirm = true;
    for (uint32_t t = 4533U; t <= 4900U; t += 33U) {
        dec = ui.step(t, {}, vm);
        CHECK(ui.renderState(t).standbyRingQ10 == kEaseScale);
    }
    renderUi(ui.renderState(4900U), vm, fbStarting);
    CHECK(std::memcmp(fbHold, fbStarting, sizeof(ScreenBuff)) != 0);
}

/** @brief 验证 OL4 行弹簧 <= 180 ms、挤压回弹 <= 120 ms、60 s 自动回表盘、关机原因文案与 UTC 显示。 */
void testSubpageSpringSquishAndIdleTimeout() noexcept
{
    // 1. 行间弹簧（17 px）须在 <= 180 ms 内收敛吸附。
    FixedSpring rowSpring{};
    rowSpring.snap(0);
    rowSpring.retarget(kSubRowStepY);
    for (uint32_t t = 0U; t < 180U; t += 15U) {
        rowSpring.step(15U, FixedSpring::kRowStiffness, FixedSpring::kRowDamping);
    }
    CHECK(rowSpring.settled() && rowSpring.position() == kSubRowStepY);

    // 2. 挤压回弹反馈：收缩 2 px 且 <= 120 ms 复原。
    SquishAnim sq{};
    sq.trigger(1000U);
    CHECK(sq.pollActive(1000U) && sq.insetPx(1045U) == kSquishMaxPx);
    CHECK(sq.pollActive(1090U) && sq.insetPx(1090U) < kSquishMaxPx);
    CHECK(!sq.pollActive(1120U) && sq.insetPx(1120U) == 0);

    // 3. 子页无操作 60 s 自动回表盘。
    ViewModel vm = makeHealthyVm();
    vm.utcValid = true;
    vm.utcHour = 8U;
    vm.utcMinute = 30U;
    vm.utcSecond = 45U;
    UiController ui{};
    ui.reset(board::BootMode::kNormal, 0U);
    const std::array<uint32_t, 2U> skipKeys{100U, 180U};
    CHECK(ui.step(180U, skipKeys, vm).page == OledPage::kDialplate);
    const std::array<uint32_t, 2U> enterSat{1000U, 1080U};
    CHECK(ui.step(1080U, enterSat, vm).page == OledPage::kSubSat);
    CHECK(ui.step(1080U + kSubpageIdleTimeoutMs - 1U, {}, vm).page == OledPage::kSubSat);
    CHECK(ui.step(1080U + kSubpageIdleTimeoutMs, {}, vm).page == OledPage::kDialplate);

    // 4. 关机原因文案与不同原因帧差异。
    CHECK(std::string_view{shutdownReasonText(board::ShutdownReason::kKey)} == "KEY");
    CHECK(std::string_view{shutdownReasonText(board::ShutdownReason::kConsole)} == "CONSOLE");
    CHECK(std::string_view{shutdownReasonText(board::ShutdownReason::kLowBattery)} ==
          "LOW BATTERY");
    CHECK(std::string_view{shutdownReasonText(board::ShutdownReason::kPhone)} == "PHONE");
    CHECK(std::string_view{shutdownReasonText(board::ShutdownReason::kWeb)} == "WEB");
    ScreenBuff fbWeb{};
    vm.shutdownStage = board::ShutdownStage::kSave;
    vm.shutdownReason = board::ShutdownReason::kWeb;
    renderPage(OledPage::kShutdown, vm, fbWeb);
    CHECK(countLitBytes(fbWeb) > 40U);
    ScreenBuff fbKey{};
    ScreenBuff fbLow{};
    vm.shutdownStage = board::ShutdownStage::kSave;
    vm.shutdownReason = board::ShutdownReason::kKey;
    renderPage(OledPage::kShutdown, vm, fbKey);
    vm.shutdownReason = board::ShutdownReason::kLowBattery;
    renderPage(OledPage::kShutdown, vm, fbLow);
    CHECK(std::memcmp(fbKey, fbLow, sizeof(ScreenBuff)) != 0);

    // 5. 表盘 UTC 显示差异与解状态切换滚动。
    ScreenBuff fbNoUtc{};
    ScreenBuff fbUtc{};
    vm.shutdownStage = board::ShutdownStage::kIdle;
    vm.utcValid = false;
    renderPage(OledPage::kDialplate, vm, fbNoUtc);
    vm.utcValid = true;
    renderPage(OledPage::kDialplate, vm, fbUtc);
    CHECK(std::memcmp(fbNoUtc, fbUtc, sizeof(ScreenBuff)) != 0);

    // 6. 表盘入口名枚举完备性。
    CHECK(std::string_view{dockItemName(0U)} == "SATS");
    CHECK(std::string_view{dockItemName(4U)} == "POWER");
}

/** @brief 验证 HIL-2：无充电芯片时 USB 插拔变化状态栏图标、电源页主卡插头/CHG 文案与待机页流动动画。 */
void testNoChargerUsbIndicator() noexcept
{
    ViewModel vm = makeHealthyVm();
    vm.chargerPresent = false;
    vm.chargerOk = false;
    vm.charging = false;
    vm.batteryPct = 78U;

    ScreenBuff fbBat{};
    ScreenBuff fbUsb{};
    ScreenBuff fbUsbFull{};
    vm.usbPresent = false;
    CHECK(!isEffectiveCharging(vm));
    renderPage(OledPage::kDialplate, vm, fbBat);
    vm.usbPresent = true;
    CHECK(isEffectiveCharging(vm));
    renderPage(OledPage::kDialplate, vm, fbUsb);
    // 状态栏充电/USB 图标位（page 0, x=95..99）在 USB 插入后必须变化。
    CHECK(!regionEqual(fbBat, fbUsb, 0U, 95U, 100U));
    vm.batteryPct = 100U;
    CHECK(!isEffectiveCharging(vm));
    renderPage(OledPage::kDialplate, vm, fbUsbFull);
    CHECK(!regionEqual(fbBat, fbUsbFull, 0U, 95U, 100U));
    CHECK(!regionEqual(fbUsb, fbUsbFull, 0U, 95U, 100U));

    // 电源子页：无充电芯片但 USB 在位时主卡显示 USB 插头且 CHG 文案区别于无 USB。
    vm.batteryPct = 78U;
    ScreenBuff pwrBat{};
    ScreenBuff pwrUsb{};
    vm.usbPresent = false;
    renderPage(OledPage::kSubPower, vm, pwrBat);
    vm.usbPresent = true;
    renderPage(OledPage::kSubPower, vm, pwrUsb);
    CHECK(std::memcmp(pwrBat, pwrUsb, sizeof(ScreenBuff)) != 0);

    // 充电待机页：无充电芯片但 USB 在位且未满电，仍启用 <=2 fps 流动动画。
    vm.bootMode = board::BootMode::kChargeStandby;
    UiController standbyUi{};
    standbyUi.reset(board::BootMode::kChargeStandby, 0U);
    ScreenBuff sb0{};
    ScreenBuff sb1{};
    static_cast<void>(standbyUi.step(0U, {}, vm));
    renderUi(standbyUi.renderState(0U), vm, sb0);
    static_cast<void>(standbyUi.step(500U, {}, vm));
    renderUi(standbyUi.renderState(500U), vm, sb1);
    CHECK(std::memcmp(sb0, sb1, sizeof(ScreenBuff)) != 0);
}

/** @brief 验证 HIL-3c/d：软件重启切换过渡页渲染与 Wi-Fi 模式正常走完自检进入表盘。 */
void testRebootBannerAndWifiSelfTest() noexcept
{
    ViewModel vm = makeHealthyVm();
    vm.radioMode = RadioMode::kWifiConfig;
    vm.btReady = false;
    vm.wifiReady = true;
    sanitizeCopy("P2-P2000123", vm.wifiSsid);

    UiController ui{};
    ui.reset(board::BootMode::kNormal, 0U);
    UiDecision dec{};
    for (uint32_t t = 0U; t <= 4000U; t += 100U) {
        dec = ui.step(t, {}, vm);
        if (dec.page == OledPage::kDialplate) {
            break;
        }
    }
    CHECK(dec.page == OledPage::kDialplate && !ui.renderState(4000U).hasCheckWarning);

    ScreenBuff fbDial{};
    ScreenBuff fbSwitch{};
    renderUi(ui.renderState(4000U), vm, fbDial);
    vm.rebootBanner = RebootBanner::kSwitchingWifi;
    dec = ui.step(4100U, {}, vm);
    CHECK(dec.markRebootFinalFrame);
    renderUi(ui.renderState(4100U), vm, fbSwitch);
    CHECK(std::memcmp(fbDial, fbSwitch, sizeof(ScreenBuff)) != 0);
    CHECK(countLitBytes(fbSwitch) > 0U);
}

/** @brief 验证 HIL-4：F9P 无定位饱和 65534 cm 不显示 655，有效定位 >=99.99 m 显示 >99m，2 cm 显示 0.02m。 */
void testHorizontalAccFormatting() noexcept
{
    std::array<char, 16U> acc{};
    CHECK(!formatHorizontalAcc(false, 65534U, acc) && acc[0] == '\0');
    CHECK(!formatHorizontalAcc(true, gnss::kU16Unknown, acc) && acc[0] == '\0');
    CHECK(formatHorizontalAcc(true, 65534U, acc) && std::string_view{acc.data()} == ">99m");
    CHECK(formatHorizontalAcc(true, 9999U, acc) && std::string_view{acc.data()} == ">99m");
    CHECK(formatHorizontalAcc(true, 9998U, acc) && std::string_view{acc.data()} == "99.98m");
    CHECK(formatHorizontalAcc(true, 2U, acc) && std::string_view{acc.data()} == "0.02m");

    ViewModel vm = makeHealthyVm();
    std::array<char, 20U> top{};
    std::array<char, 20U> bot{};

    // 无定位 + 65534 饱和值（带 UTC 与不带 UTC 两种情况）：绝不出现 "655"，显示 NO FIX。
    vm.fixValid = false;
    vm.hAccCm = 65534U;
    vm.utcValid = true;
    vm.utcHour = 12U;
    vm.utcMinute = 34U;
    vm.utcSecond = 56U;
    formatDialRightSummary(vm, top, bot);
    CHECK(std::string_view{top.data()}.find("655") == std::string_view::npos);
    CHECK(std::string_view{bot.data()} == "NO FIX");

    vm.utcValid = false;
    formatDialRightSummary(vm, top, bot);
    CHECK(std::string_view{top.data()} == "NO FIX");
    CHECK(std::string_view{bot.data()}.find("655") == std::string_view::npos);

    // 渲染核对：无定位 65534 与 kU16Unknown 渲染逐像素一致（表盘与位置页）。
    ScreenBuff dialSat{};
    ScreenBuff dialUnk{};
    ScreenBuff posSat{};
    ScreenBuff posUnk{};
    renderPage(OledPage::kDialplate, vm, dialSat);
    renderPage(OledPage::kSubPos, vm, posSat);
    vm.hAccCm = gnss::kU16Unknown;
    renderPage(OledPage::kDialplate, vm, dialUnk);
    renderPage(OledPage::kSubPos, vm, posUnk);
    CHECK(std::memcmp(dialSat, dialUnk, sizeof(ScreenBuff)) == 0);
    CHECK(std::memcmp(posSat, posUnk, sizeof(ScreenBuff)) == 0);

    // 有效定位 + 65534 -> ">99m"；有效定位 + 2 -> "0.02m"。
    vm.fixValid = true;
    vm.hAccCm = 65534U;
    vm.utcValid = false;
    formatDialRightSummary(vm, top, bot);
    CHECK(std::string_view{top.data()} == ">99m");
    vm.utcValid = true;
    formatDialRightSummary(vm, top, bot);
    CHECK(std::string_view{bot.data()}.find(">99m") != std::string_view::npos);

    vm.hAccCm = 2U;
    vm.utcValid = false;
    formatDialRightSummary(vm, top, bot);
    CHECK(std::string_view{top.data()} == "0.02m");
    vm.utcValid = true;
    formatDialRightSummary(vm, top, bot);
    CHECK(std::string_view{bot.data()}.find("0.02m") != std::string_view::npos);
}

/** @brief 验证 OTA 覆盖弹窗、丢弃手势、失败回原页，且关机终帧优先。 */
void testOtaPage()
{
    using ota::OtaPhase;
    using web_ota::UploadCode;
    UiController ctrl{};
    auto vm = makeHealthyVm();
    ctrl.reset(board::BootMode::kNormal, 0U);
    const std::array<uint32_t, 2U> skip{100U, 200U};
    CHECK(ctrl.step(200U, skip, vm).page == OledPage::kDialplate);
    static_cast<void>(ctrl.step(500U, {}, vm, true));
    CHECK(ctrl.renderState(500U).modal == UiModal::kPowerOffConfirm);

    vm.otaProgress = {.receivedBytes = 200U,
                      .totalBytes = 400U,
                      .changedMs = 600U,
                      .phase = OtaPhase::kReceiving};
    const std::array<uint32_t, 2U> clicks{610U, 700U};
    const auto receiving = ctrl.step(700U, clicks, vm, true);
    CHECK(receiving.page == OledPage::kOta && receiving.action.action == OledAction::kNone);
    CHECK(!receiving.hasClickDeadline && !receiving.animating);
    CHECK(ctrl.renderState(700U).modal == UiModal::kNone);
    CHECK(ctrl.dockSelected() == 0U && vm.otaProgress.percent() == 50U);

    vm.otaProgress.phase = OtaPhase::kVerifying;
    CHECK(ctrl.step(900U, {}, vm).page == OledPage::kOta);
    vm.otaProgress = {.changedMs = 1000U, .phase = OtaPhase::kFailed, .code = UploadCode::kTimeout};
    CHECK(ctrl.step(1000U, {}, vm).nextWaitMs == 3000U);
    CHECK(ctrl.step(3999U, {}, vm).nextWaitMs == 1U);
    CHECK(ctrl.step(4000U, clicks, vm, true).page == OledPage::kDialplate);
    CHECK(ctrl.dockSelected() == 0U && ctrl.renderState(4000U).modal == UiModal::kNone);

    vm.otaProgress.phase = OtaPhase::kSuccess;
    ScreenBuff success{};
    ScreenBuff reboot{};
    CHECK(ctrl.step(5000U, {}, vm).page == OledPage::kOta);
    renderUi(ctrl.renderState(5000U), vm, success);
    vm.rebootBanner = RebootBanner::kRebooting;
    CHECK(ctrl.step(6500U, {}, vm).markRebootFinalFrame);
    renderUi(ctrl.renderState(6500U), vm, reboot);
    CHECK(std::memcmp(success, reboot, sizeof(ScreenBuff)) == 0);
    vm.shutdownStage = board::ShutdownStage::kCutPower;
    vm.shutdownDoneMask = board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady);
    const auto shutdown = ctrl.step(6501U, {}, vm);
    CHECK(shutdown.page == OledPage::kShutdown && shutdown.markShutdownFinalFrame);
}

/** @brief 验证 CSQ 边界、首次即时显示、升降格、未知恢复和毫秒回绕。 */
void testSignalBars() noexcept
{
    CHECK(csqBars(0U) == 1U && csqBars(9U) == 1U);
    CHECK(csqBars(10U) == 2U && csqBars(14U) == 2U);
    CHECK(csqBars(15U) == 3U && csqBars(19U) == 3U);
    CHECK(csqBars(20U) == 4U && csqBars(31U) == 4U);
    CHECK(csqBars(99U) == 0U && csqBars(255U) == 0U);
    constexpr uint32_t kStart = UINT32_MAX - 100U;
    auto bars = updateSignalBars({}, 2U, kStart - 1U);
    CHECK(bars.shown == 2U);
    bars = updateSignalBars(bars, 4U, kStart);
    CHECK(updateSignalBars(bars, 4U, kStart + 9999U).shown == 2U);
    bars = updateSignalBars(bars, 4U, kStart + 10000U);
    CHECK(bars.shown == 4U);
    bars = updateSignalBars(bars, 1U, 15000U);
    CHECK(bars.shown == 1U);
    bars = updateSignalBars(bars, csqBars(99U), 15001U);
    CHECK(bars.shown == 0U);
    CHECK(updateSignalBars(bars, 3U, 15002U).shown == 3U);
}

/** @brief 验证高位抖动按窗口最小值升格，回落至当前格数时结束窗口。 */
void testSignalBarsFluctuation() noexcept
{
    auto bars = updateSignalBars({}, 2U, 0U);
    bars = updateSignalBars(bars, 4U, 1000U);
    bars = updateSignalBars(bars, 3U, 2000U);
    bars = updateSignalBars(bars, 4U, 6000U);
    bars = updateSignalBars(bars, 3U, 9000U);
    bars = updateSignalBars(bars, 4U, 10999U);
    CHECK(bars.shown == 2U);
    bars = updateSignalBars(bars, 4U, 11000U);
    CHECK(bars.shown == 3U && bars.candidate == 4U && bars.sinceMs == 11000U);
    CHECK(updateSignalBars(bars, 4U, 20999U).shown == 3U);
    CHECK(updateSignalBars(bars, 4U, 21000U).shown == 4U);

    auto interrupted = updateSignalBars(bars, 3U, 20000U);
    interrupted = updateSignalBars(interrupted, 4U, 20001U);
    CHECK(updateSignalBars(interrupted, 4U, 21000U).shown == 3U);
    CHECK(updateSignalBars(interrupted, 4U, 30001U).shown == 4U);
}

/** @brief 验证蜂窝图标选择，以及状态栏 4G、搜索帧、未来箭头只占预留区域。 */
void testCellularIcon() noexcept
{
    using cellular::Phase;
    using cellular::SimStatus;
    auto vm = makeHealthyVm();
    auto &net = vm.net;
    CHECK(cellularIcon(net) == CellularIcon::kHidden);
    net.supported = true;
    net.phase = Phase::kPowerOn;
    CHECK(cellularIcon(net) == CellularIcon::kHidden);
    UiRenderState state{.page = OledPage::kDialplate};
    ScreenBuff hidden{}, search{}, next{}, signal{}, pdp{}, arrows{}, simError{};
    renderUi(state, vm, hidden);

    net.enabled = true;
    for (const auto phase :
         {Phase::kPowerCycle, Phase::kPowerOn, Phase::kSync, Phase::kSimCheck, Phase::kRegister}) {
        net.phase = phase;
        CHECK(cellularIcon(net) == CellularIcon::kSearching);
    }
    renderUi(state, vm, search);
    state.nowMs = kSignalSearchFrameMs;
    renderUi(state, vm, next);
    CHECK(!regionEqual(search, next, 0U, 74U, 85U));
    CHECK(regionEqual(hidden, search, 0U, 0U, 64U));
    CHECK(regionEqual(hidden, search, 0U, 94U, kWidth));

    net.sim = SimStatus::kReady;
    net.reg = cellular::RegStatus::kHome;
    net.phase = Phase::kPdpUp;
    state.cellularBars = 3U;
    CHECK(cellularIcon(net) == CellularIcon::kSignal);
    renderUi(state, vm, signal);
    CHECK(regionEqual(signal, search, 0U, 86U, 93U));
    net.phase = Phase::kReady;
    net.pdpUp = true;
    CHECK(cellularIcon(net) == CellularIcon::kPdp);
    renderUi(state, vm, pdp);
    CHECK(!regionEqual(signal, pdp, 0U, 86U, 93U));
    net.ntripConnected = true;
    net.mqttConnected = true;
    renderUi(state, vm, arrows);
    CHECK(!regionEqual(pdp, arrows, 0U, 86U, 93U));
    for (const auto sim : {SimStatus::kAbsent, SimStatus::kPinRequired, SimStatus::kError}) {
        net.sim = sim;
        CHECK(cellularIcon(net) == CellularIcon::kSimError);
    }
    state.cellularBars = 0U;
    renderUi(state, vm, simError);
    CHECK(!regionEqual(simError, signal, 0U, 86U, 93U));
    net.sim = SimStatus::kReady;
    net.phase = Phase::kPowerFault;
    CHECK(cellularIcon(net) == CellularIcon::kFault);
}

/** @brief 取单列状态栏像素，含 y=8/9，排除 y=10 分隔线。 */
unsigned statusColumn(const ScreenBuff &fb, size_t x) noexcept
{
    return fb[0][x] | ((fb[1][x] & 0x03U) << 8U);
}

struct StatusSlot {
    size_t first;
    size_t last;
};

/** @brief 逐列证明槽位不相交，槽位外全黑，而且每个槽位内都有墨点。 */
void checkStatusSlots(const ScreenBuff &fb, std::span<const StatusSlot> slots) noexcept
{
    for (size_t x = 0U; x < kWidth; ++x) {
        size_t owners = 0U;
        for (const auto &slot : slots) {
            if (x >= slot.first && x <= slot.last) {
                ++owners;
            }
        }
        CHECK(owners <= 1U);
        if (owners == 0U) {
            CHECK(statusColumn(fb, x) == 0U);
        }
    }
    for (const auto &slot : slots) {
        unsigned pixels = 0U;
        for (size_t x = slot.first; x <= slot.last; ++x) {
            pixels |= statusColumn(fb, x);
        }
        CHECK(pixels != 0U);
    }
}

/** @brief 验证槽位外逐列保持不变，以及指定槽位确实变化。 */
void checkStatusChange(const ScreenBuff &before, const ScreenBuff &after, StatusSlot slot) noexcept
{
    bool changed = false;
    for (size_t x = 0U; x < kWidth; ++x) {
        const bool equal = statusColumn(before, x) == statusColumn(after, x);
        if (x < slot.first || x > slot.last) {
            CHECK(equal);
        } else {
            changed = changed || !equal;
        }
    }
    CHECK(changed);
}

/** @brief 验证两种顶栏槽位、4G 四格几何以及 USB、搜网、隐藏与射频切换的列范围。 */
void testStatusBarSlots() noexcept
{
    auto vm = makeHealthyVm();
    vm.fixQuality = gnss::FixQuality::kRtkFloat;
    vm.charging = true;
    vm.net.supported = true;
    vm.net.enabled = true;
    vm.net.phase = cellular::Phase::kReady;
    vm.net.pdpUp = true;
    vm.net.ntripConnected = true;
    vm.net.mqttConnected = true;
    UiRenderState state{.page = OledPage::kDialplate,
                        .hasCheckWarning = true,
                        .cellularBars = 4U,
                        .batBars = {.bars = 5U}};
    ScreenBuff full{}, frame{}, hidden{};
    CHECK(renderUi(state, vm, full));
    constexpr std::array<StatusSlot, 12U> kCellularSlots{{
        {0U, 7U},
        {10U, 21U},
        {23U, 29U},
        {32U, 62U},
        {64U, 71U},
        {74U, 75U},
        {77U, 78U},
        {80U, 81U},
        {83U, 84U},
        {86U, 92U},
        {95U, 99U},
        {102U, 126U},
    }};
    checkStatusSlots(full, kCellularSlots);
    CHECK(statusColumn(full, 102U) != 0U && statusColumn(full, 126U) != 0U);
    constexpr std::array kBarMasks{0x1C0U, 0x1E0U, 0x1F8U, 0x1FEU}; // 3/4/6/8 px，底边 y=8。
    for (size_t i = 0U; i < kBarMasks.size(); ++i) {
        const size_t x = 74U + 3U * i;
        CHECK(statusColumn(full, x) == kBarMasks[i]);
        CHECK(statusColumn(full, x + 1U) == kBarMasks[i]);
    }
    CHECK((statusColumn(full, 87U) & 0x0FU) == 0x0FU);
    CHECK((statusColumn(full, 91U) & 0x0FU) == 0x0FU);
    CHECK(statusColumn(full, 89U) == 0U); // 4G 字形及两个独立箭头中间有空列。

    vm.usbPresent = false; // 即使充电位仍为真，USB 槽也必须立即清空。
    CHECK(renderUi(state, vm, frame));
    checkStatusChange(full, frame, {95U, 99U});
    for (size_t x = 95U; x <= 99U; ++x) {
        CHECK(statusColumn(frame, x) == 0U);
    }
    vm.usbPresent = true;
    vm.charging = false;
    CHECK(renderUi(state, vm, frame));
    checkStatusChange(full, frame, {95U, 99U});
    vm.charging = true;

    vm.net.phase = cellular::Phase::kRegister;
    vm.net.pdpUp = false;
    CHECK(renderUi(state, vm, frame));
    checkStatusChange(full, frame, {74U, 92U});
    vm.net.enabled = false;
    CHECK(renderUi(state, vm, hidden));
    checkStatusChange(full, hidden, {64U, 93U});
    constexpr std::array<StatusSlot, 7U> kHiddenSlots{{
        {0U, 7U},
        {10U, 21U},
        {23U, 29U},
        {32U, 62U},
        {86U, 93U},
        {95U, 99U},
        {102U, 126U},
    }};
    checkStatusSlots(hidden, kHiddenSlots);
    for (size_t i = 0U; i < 8U; ++i) {
        CHECK(statusColumn(full, 64U + i) == statusColumn(hidden, 86U + i));
    }
    vm.net.enabled = true;
    vm.net.phase = cellular::Phase::kOff;
    CHECK(renderUi(state, vm, frame));
    CHECK(std::memcmp(hidden, frame, sizeof(frame)) == 0);
    vm.net.supported = false;
    vm.net.phase = cellular::Phase::kReady;
    CHECK(renderUi(state, vm, frame));
    CHECK(std::memcmp(hidden[0], frame[0], sizeof(frame[0])) == 0);

    vm.radioMode = RadioMode::kWifiConfig;
    CHECK(renderUi(state, vm, frame));
    checkStatusChange(hidden, frame, {86U, 93U});
    vm.net.supported = true;
    vm.net.pdpUp = true;
    CHECK(renderUi(state, vm, frame));
    checkStatusChange(full, frame, {64U, 71U});
}

/** @brief 验证最长短词居中、三位计数与滚动裁剪、蜂窝板卫星图表的计数边界。 */
void testStatusBarText() noexcept
{
    CHECK(WouoUI_GetStrWidth("PROBE", Font_6_8) == 30U);
    CHECK(WouoUI_GetStrWidth("FLOAT", Font_6_8) == 30U);
    auto vm = makeHealthyVm();
    vm.fixQuality = gnss::FixQuality::kRtkFloat;
    vm.satsUsed = 99U;
    UiRenderState state{.page = OledPage::kDialplate, .hasCheckWarning = true};
    ScreenBuff initial{}, frame{};
    CHECK(renderUi(state, vm, initial));
    CHECK(statusColumn(initial, 33U) == 0U); // 字框从 x33 开始，字体每字首列留白。
    CHECK(statusColumn(initial, 34U) != 0U && statusColumn(initial, 62U) != 0U);
    CHECK(statusColumn(initial, 63U) == 0U);

    NumberRoll roll{};
    roll.update(99, 0U);
    roll.update(255, 100U);
    vm.satsUsed = 255U;
    state.satsRoll = &roll;
    for (const uint32_t now : {100U, 133U, 166U, 200U, 500U}) {
        state.nowMs = now;
        CHECK(renderUi(state, vm, frame));
        for (size_t x = 0U; x < kWidth; ++x) {
            if (x < 10U || x > 21U) {
                CHECK(statusColumn(frame, x) == statusColumn(initial, x));
            }
        }
    }
    for (const size_t x : {10U, 14U, 18U}) {
        CHECK(statusColumn(frame, x) != 0U && statusColumn(frame, x + 2U) != 0U);
        CHECK(statusColumn(frame, x + 3U) == 0U);
    }

    state.page = OledPage::kSubSat;
    vm.satsVisible = 255U;
    vm.net.supported = true;
    vm.net.enabled = true;
    vm.net.phase = cellular::Phase::kReady;
    vm.net.pdpUp = true;
    CHECK(renderUi(state, vm, initial));
    CHECK(statusColumn(initial, 72U) == 0U && statusColumn(initial, 73U) == 0U);
    vm.net.enabled = false;
    CHECK(renderUi(state, vm, frame));
    checkStatusChange(initial, frame, {74U, 92U});
}

/** @brief 验证两个板型的 Dock 步距、回绕，以及 NET 三个详情都能进出。 */
void testNetNavigation() noexcept
{
    auto vm = makeHealthyVm();
    UiController ctrl{};
    const auto enter = [&ctrl, &vm](uint32_t now) {
        const std::array keys{now - 100U, now};
        return ctrl.step(now, keys, vm).page;
    };
    const auto advance = [&ctrl, &vm](uint32_t now) {
        const std::array keys{now - kDoubleClickWindowMs};
        static_cast<void>(ctrl.step(now, keys, vm));
    };
    ctrl.reset(board::BootMode::kNormal, 0U);
    CHECK(enter(200U) == OledPage::kDialplate);
    uint32_t now = 1000U;
    for (uint8_t index = 0U; index < kDockItemCount; ++index) {
        CHECK(ctrl.dockSelected() == index);
        CHECK(dockRect(index, false).x == 2 + index * 25 && dockRect(index, false).w == 24);
        advance(now);
        now += 500U;
    }
    CHECK(ctrl.dockSelected() == 0U);

    // 有蜂窝硬件但 net.enable=0，仍可查看 NET。
    vm.net.supported = true;
    for (uint8_t index = 0U; index < kDockItemCount; ++index) {
        advance(now);
        now += 500U;
    }
    CHECK(ctrl.dockSelected() == kDockItemCount);
    CHECK(enter(now) == OledPage::kSubNet);
    for (const auto detail : {OledPage::kNetCellular, OledPage::kNetNtrip, OledPage::kNetMqtt}) {
        now += 500U;
        CHECK(enter(now) == detail);
        ScreenBuff fb{};
        renderUi(ctrl.renderState(now), vm, fb);
        CHECK(countLitBytes(fb) > 0U);
        now += 500U;
        CHECK(enter(now) == OledPage::kSubNet);
        now += 500U;
        advance(now);
    }
    CHECK(enter(now + 500U) == OledPage::kDialplate);
    advance(now + 1000U);
    CHECK(ctrl.dockSelected() == 0U);
}

/** @brief 验证 ICCID 与控制台共用的掩码，包含空、短、最大长和未终止输入。 */
void testIccidMask() noexcept
{
    std::array<char, cellular::kIccidBytes> input{};
    std::array<char, cellular::kIccidBytes> masked{};
    net_status::maskIccid(input, masked);
    CHECK(masked.front() == '\0');
    sanitizeCopy("1234567890", input);
    net_status::maskIccid(input, masked);
    CHECK(std::string_view{masked.data()} == "1234567890");
    sanitizeCopy("12345678901234567890", input);
    net_status::maskIccid(input, masked);
    CHECK(std::string_view{masked.data()} == "123456**********7890");
    input.fill('1');
    net_status::maskIccid(input, masked);
    CHECK(masked.back() == '\0' && std::string_view{masked.data()} == "111111**********1111");
}

/** @brief 验证告警优先级、同批次逐项确认、关机和 OTA 抢占及确认键消费。 */
void testAlarmPage() noexcept
{
    using enum AlarmKind;
    auto vm = makeHealthyVm();
    UiController ctrl{};
    ctrl.reset(board::BootMode::kNormal, 0U);
    const std::array<uint32_t, 2U> skip{100U, 200U};
    CHECK(ctrl.step(200U, skip, vm).page == OledPage::kDialplate);
    AlarmSnapshot alarms{.sequence = 1U};
    vm.alarm = &alarms;
    alarms.pending = alarmBit(kNetwork) | alarmBit(kLowBattery) | alarmBit(kPrecisionLost) |
                     alarmBit(kFenceExit) | alarmBit(kFenceEnter);
    sanitizeCopy("TEST FENCE", alarms.fenceEnterName);
    CHECK(ctrl.step(1000U, {}, vm).page == OledPage::kAlarm);
    CHECK(ctrl.renderState(1000U).alarm == kFenceEnter);
    ScreenBuff fb{};
    renderUi(ctrl.renderState(1000U), vm, fb);
    CHECK(countLitBytes(fb) > 0U);

    vm.otaProgress.phase = ota::OtaPhase::kReceiving;
    const std::array<uint32_t, 1U> coveredPress{1005U};
    CHECK(ctrl.step(1010U, coveredPress, vm).page == OledPage::kOta);
    vm.shutdownStage = board::ShutdownStage::kAnnounce;
    CHECK(ctrl.step(1020U, {}, vm).page == OledPage::kShutdown);
    vm.shutdownStage = board::ShutdownStage::kIdle;
    CHECK(ctrl.step(1030U, {}, vm).page == OledPage::kOta);
    vm.otaProgress = {};
    CHECK(ctrl.step(1040U, {}, vm).page == OledPage::kAlarm);
    CHECK(ctrl.renderState(1040U).alarm == kFenceEnter);

    uint32_t now = 1100U;
    for (const auto next : {kFenceExit, kPrecisionLost, kLowBattery, kNetwork, kNone}) {
        const std::array press{now};
        const auto out = ctrl.step(now, press, vm);
        CHECK(out.page == (next == kNone ? OledPage::kDialplate : OledPage::kAlarm));
        CHECK(out.action.action == OledAction::kNone && !out.hasClickDeadline);
        CHECK(ctrl.renderState(now).alarm == next);
        now += 100U;
    }
    CHECK(ctrl.step(2000U, {}, vm).page == OledPage::kDialplate);
    CHECK(ctrl.dockSelected() == 0U);
    ++alarms.sequence;
    CHECK(ctrl.step(3000U, {}, vm).page == OledPage::kAlarm);
}

/** @brief 验证告警覆盖旧配对弹窗，也抑制告警期间抵达的配对请求。 */
void testAlarmSuppressesPairing() noexcept
{
    auto vm = makeHealthyVm();
    UiController ctrl{};
    ctrl.reset(board::BootMode::kNormal, 0U);
    const std::array<uint32_t, 2U> skip{100U, 200U};
    CHECK(ctrl.step(200U, skip, vm).page == OledPage::kDialplate);
    vm.pairing.id = 41U;
    vm.pairing.requestDeadlineMs = 5000U;
    static_cast<void>(ctrl.step(300U, {}, vm));
    CHECK(ctrl.renderState(300U).modal == UiModal::kPair);

    AlarmSnapshot alarm{.sequence = 1U, .pending = alarmBit(AlarmKind::kNetwork)};
    vm.alarm = &alarm;
    CHECK(ctrl.step(400U, {}, vm).page == OledPage::kAlarm);
    CHECK(ctrl.renderState(400U).modal == UiModal::kNone);
    const auto confirm = [&ctrl, &vm](uint32_t nowMs) {
        const std::array key{nowMs};
        const auto out = ctrl.step(nowMs, key, vm);
        CHECK(out.page == OledPage::kDialplate && out.action.action == OledAction::kNone);
        CHECK(ctrl.renderState(nowMs).modal == UiModal::kNone);
    };
    confirm(500U);

    ++alarm.sequence;
    CHECK(ctrl.step(600U, {}, vm).page == OledPage::kAlarm);
    vm.pairing.id = 42U;
    confirm(700U); // 在确认告警的同一轮读到新请求，确认键仍专属于告警。

    vm.pairing.id = 43U;
    static_cast<void>(ctrl.step(800U, {}, vm));
    CHECK(ctrl.renderState(800U).modal == UiModal::kPair);
}

/** @brief 对比实际帧中选用的告警位图、标题反色和外框相位。 */
void testAlarmIcons() noexcept
{
    using enum AlarmKind;
    struct IconCase {
        AlarmKind kind;
        const icons::AlarmBitmap &bitmap;
    };
    const std::array cases{
        IconCase{kFenceEnter, icons::kAlarmFenceEnter},
        IconCase{kFenceExit, icons::kAlarmFenceExit},
        IconCase{kPrecisionLost, icons::kAlarmPrecisionLost},
        IconCase{kLowBattery, icons::kAlarmLowBattery},
        IconCase{kNetwork, icons::kAlarmGeneric},
        IconCase{static_cast<AlarmKind>(255U), icons::kAlarmGeneric},
    };
    auto vm = makeHealthyVm();
    ScreenBuff lit{}, normal{}, steady{};
    for (const auto &entry : cases) {
        UiRenderState state{.page = OledPage::kAlarm,
                            .alarm = entry.kind,
                            .alarmFlashing = true,
                            .alarmHighlighted = true};
        renderUi(state, vm, lit);
        state.alarmHighlighted = false;
        renderUi(state, vm, normal);
        state.alarmFlashing = false;
        renderUi(state, vm, steady);
        CHECK((lit[0][1] & 0x02U) != 0U && (normal[0][1] & 0x02U) == 0U);
        CHECK((lit[7][0] & 0x80U) != 0U && (normal[7][0] & 0x80U) == 0U);
        CHECK((steady[7][0] & 0x80U) != 0U);
        constexpr size_t kIconSide = 24U;
        for (size_t y = 0U; y < kIconSide; ++y) {
            for (size_t x = 0U; x < kIconSide; ++x) {
                const bool expected =
                    (entry.bitmap[(y / 8U) * kIconSide + x] & (1U << (y % 8U))) != 0U;
                const size_t yOnScreen = y + 30U;
                const auto mask = 1U << (yOnScreen % 8U);
                for (const auto *frame : {&lit, &normal, &steady}) {
                    CHECK((((*frame)[yOnScreen / 8U][x + 7U] & mask) != 0U) == expected);
                }
            }
        }
    }
}

/** @brief 验证各告警全周期、30 s 停闪、同类型新批次重新闪烁和时钟回绕。 */
void testAlarmFlashTiming() noexcept
{
    using enum AlarmKind;
    constexpr uint32_t kStart = UINT32_MAX - 240U;
    constexpr uint32_t kBlinkEnd = 30000U;
    for (const auto kind : {kFenceEnter, kFenceExit, kPrecisionLost, kLowBattery, kNetwork}) {
        auto vm = makeHealthyVm();
        AlarmSnapshot alarm{.sequence = 1U, .pending = alarmBit(kind)};
        vm.alarm = &alarm;
        UiController ctrl{};
        ctrl.reset(board::BootMode::kNormal, kStart - 1U);
        const uint32_t period = (kind == kFenceEnter || kind == kFenceExit) ? 250U : 500U;
        const auto checkPhase = [&ctrl, &vm, kind](uint32_t elapsed, bool highlighted) {
            const auto tick = kStart + elapsed;
            const auto out = ctrl.step(tick, {}, vm);
            CHECK(out.page == OledPage::kAlarm && out.animating);
            CHECK(out.nextWaitMs == kAnimFramePeriodMs);
            const auto state = ctrl.renderState(tick);
            CHECK(state.alarm == kind && state.alarmFlashing);
            CHECK(state.alarmHighlighted == highlighted);
        };
        checkPhase(0U, true);
        checkPhase(period / 2U - 1U, true);
        checkPhase(period / 2U, false);
        checkPhase(period - 1U, false);
        checkPhase(period, true);
        checkPhase(kBlinkEnd - 1U, false);

        const auto end = ctrl.step(kStart + kBlinkEnd, {}, vm);
        CHECK(end.page == OledPage::kAlarm && !end.animating && end.nextWaitMs == kWaitForEvent);
        const auto steady = ctrl.renderState(kStart + kBlinkEnd);
        CHECK(!steady.alarmFlashing && !steady.alarmHighlighted);
        CHECK(!ctrl.step(kStart + 1U, {}, vm).animating); // 同一告警到期后回绕也不重启。
        ++alarm.sequence;
        checkPhase(2U, true);
    }
}

/** @brief 校验 DEV 四种告警输入，并验证停闪后确认回到原子页与选中项。 */
void testAlarmTestSourceAndReturn() noexcept
{
    using enum AlarmKind;
    auto alarm = makeTestAlarm("enter");
    CHECK(alarm.pending == alarmBit(kFenceEnter));
    CHECK(std::string_view{alarm.fenceEnterName.data()} == "TEST FENCE");
    alarm = makeTestAlarm("leave");
    CHECK(alarm.pending == alarmBit(kFenceExit));
    CHECK(std::string_view{alarm.fenceExitName.data()} == "TEST FENCE");
    CHECK(makeTestAlarm("precision").pending == alarmBit(kPrecisionLost));
    CHECK(makeTestAlarm("lowbat").pending == alarmBit(kLowBattery));
    CHECK(makeTestAlarm("invalid").pending == 0U);

    auto vm = makeHealthyVm();
    UiController ctrl{};
    ctrl.reset(board::BootMode::kNormal, 0U);
    const std::array<uint32_t, 2U> skip{100U, 200U};
    CHECK(ctrl.step(200U, skip, vm).page == OledPage::kDialplate);
    const std::array<uint32_t, 1U> select{500U};
    static_cast<void>(ctrl.step(800U, select, vm));
    const std::array<uint32_t, 2U> enterPos{1000U, 1100U};
    CHECK(ctrl.step(1100U, enterPos, vm).page == OledPage::kSubPos);
    const std::array<uint32_t, 1U> nextRow{1600U};
    static_cast<void>(ctrl.step(1900U, nextRow, vm));
    CHECK(ctrl.dockSelected() == 1U && ctrl.renderState(1900U).subSelected == 1U);

    alarm.sequence = 1U;
    vm.alarm = &alarm;
    CHECK(ctrl.step(2100U, {}, vm).page == OledPage::kAlarm);
    CHECK(!ctrl.step(32100U, {}, vm).animating);
    const std::array<uint32_t, 1U> ack{32200U};
    const auto out = ctrl.step(32200U, ack, vm);
    CHECK(out.page == OledPage::kSubPos && out.action.action == OledAction::kNone);
    CHECK(ctrl.dockSelected() == 1U && ctrl.renderState(32200U).subSelected == 1U);
}

/** @brief 验证跑马灯停留、位移、回绕和可见字段的唯一像素裁剪范围。 */
void testMarquee()
{
    CHECK(marqueeMotion(0, 1400U).waitMs == kWaitForEvent);
    CHECK(marqueeMotion(30, 999U).offset == 0 && marqueeMotion(30, 999U).waitMs == 1U);
    CHECK(marqueeMotion(30, 1000U).moving && marqueeMotion(30, 1500U).offset == 15);
    CHECK(marqueeMotion(30, 2000U).offset == 30 && !marqueeMotion(30, 2000U).moving);
    CHECK(marqueeMotion(30, 2999U).offset == 30 && marqueeMotion(30, 2999U).waitMs == 1U);
    CHECK(marqueeMotion(30, 3000U).offset == 0 && marqueeMotion(30, 3000U).waitMs == 1000U);
    CHECK(marqueeMotion(1, 1034U).offset == 1);

    Marquee marquee{};
    UiRenderState state{.page = OledPage::kSubComm, .marquee = &marquee};
    auto vm = makeHealthyVm();
    vm.radioMode = RadioMode::kWifiConfig;
    vm.wifiReady = true;
    vm.wifiIpv4 = {192U, 168U, 10U, 12U};
    sanitizeCopy("P2-1826M0056", vm.wifiSsid);
    ScreenBuff head{};
    ScreenBuff tail{};
    renderUi(state, vm, head);
    CHECK(marquee.waitMs() == 1000U && !marquee.moving());
    state.nowMs = 1200U; // 实际 SSID 72 px，图标右侧只有 69 px，现已到末端。
    renderUi(state, vm, tail);
    CHECK(marquee.waitMs() == 900U && !marquee.moving());
    CHECK(std::memcmp(head, tail, sizeof(ScreenBuff)) != 0);
    // 唯一变化应在 SSID 的值框内，排除左侧图标、选中框、IP 及滚动条。
    for (size_t page = 0U; page < kPages; ++page) {
        for (size_t col = 0U; col < kWidth; ++col) {
            if (page < 4U || page > 5U || col < 53U || col >= 122U) {
                CHECK(head[page][col] == tail[page][col]);
            }
        }
    }
    state.nowMs = 2100U;
    renderUi(state, vm, tail);
    CHECK(std::memcmp(head, tail, sizeof(ScreenBuff)) == 0);

    state.subScrollY = 40; // SSID 文本区域滚出列表后，无动画唤醒。
    renderUi(state, vm, tail);
    CHECK(marquee.waitMs() == kWaitForEvent && !marquee.moving());
    state.subScrollY = 0;
    state.nowMs = 4000U;
    renderUi(state, vm, tail);
    CHECK(marquee.waitMs() == 1000U && std::memcmp(head, tail, sizeof(ScreenBuff)) == 0);

    sanitizeCopy("P2-85EE", vm.wifiSsid);
    renderUi(state, vm, tail);
    CHECK(marquee.waitMs() == kWaitForEvent && !marquee.moving());
}

} // namespace

/**
 * @brief 执行 OL2 OLED UI 全部纯逻辑主机测试用例
 * @retval EXIT_SUCCESS 全部通过
 * @retval EXIT_FAILURE 存在失败用例
 */
int main()
{
    testClickDetectorAndLongPress();
    testFixedPointMathAndAnim();
    testSystemCheckStateMachine();
    testControllerNavigationAndShutdown();
    testFormattingAndSatSummary();
    testRenderAllPagesAndDirtySkip();
    testTransitionPose();
    testPacingAndPerf();
    testTransitionRetainsStatusBar();
    testCommAndPairing();
    testModalDirtyPages();
    testUiEventBus();
    testHoldOverlayAndPowerOffConfirm();
    testStandbyRingAndChargeAnim();
    testSubpageSpringSquishAndIdleTimeout();
    testNoChargerUsbIndicator();
    testRebootBannerAndWifiSelfTest();
    testHorizontalAccFormatting();
    testOtaPage();
    testMarquee();
    testSignalBars();
    testSignalBarsFluctuation();
    testCellularIcon();
    testStatusBarSlots();
    testStatusBarText();
    testNetNavigation();
    testIccidMask();
    testAlarmPage();
    testAlarmSuppressesPairing();
    testAlarmIcons();
    testAlarmFlashTiming();
    testAlarmTestSourceAndReturn();
    if (s_failures != 0) {
        std::printf("oled_ui_check: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    std::printf("oled_ui_check: OK\n");
    return EXIT_SUCCESS;
}
