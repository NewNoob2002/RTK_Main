// 主机单元测试（OL2）：双击/短按消歧、开机自检状态机、关机进度与 USB 终帧、
// 定点缓动/弹簧/滚动数字、33 ms/静止 0 写帧调度、全页面/过渡渲染与脏页跳过。
// 单线程主机测试，无硬件访问，非 ISR。
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "board/PowerLogic.hpp"
#include "modules/oled/OledAnim.hpp"
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

    // 300 ms 过渡结束后静止：animating=false，nextWaitMs=200 ms。
    now += kPageTransitionMs + 33U;
    dec = ctrl.step(now, kNoKeys, vm);
    CHECK(!dec.animating && dec.nextWaitMs == kIdlePollPeriodMs);

    // 短按一次：等待双击窗口时记录绝对截止并缩短 nextWaitMs（250 ms 后剩余 50 ms < 200 ms，ROL2-02/R2OL2-03），
    // 满 300 ms 确认单击后选中框切换至入口 1（弹簧激活 33 ms）。
    const uint32_t pressAt = now + 10U;
    const auto k1 = oneKey(pressAt);
    dec = ctrl.step(pressAt, k1, vm);
    CHECK(ctrl.dockSelected() == 0U && !dec.animating && dec.hasClickDeadline &&
          dec.clickDeadlineMs == (pressAt + kDoubleClickWindowMs) &&
          dec.nextWaitMs == kIdlePollPeriodMs);
    dec = ctrl.step(pressAt + 250U, kNoKeys, vm);
    CHECK(ctrl.dockSelected() == 0U && !dec.animating && dec.hasClickDeadline &&
          dec.nextWaitMs == 50U);
    dec = ctrl.step(pressAt + kDoubleClickWindowMs, kNoKeys, vm);
    CHECK(ctrl.dockSelected() == 1U && dec.animating && !dec.hasClickDeadline &&
          dec.nextWaitMs == kAnimFramePeriodMs);
    // 弹簧收敛后回到静止 200 ms。
    now += 10U + kDoubleClickWindowMs;
    for (unsigned i = 0U; i < 20U; ++i) {
        now += kAnimFramePeriodMs;
        dec = ctrl.step(now, kNoKeys, vm);
    }
    CHECK(!dec.animating && dec.nextWaitMs == kIdlePollPeriodMs);

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
        .nextWaitMs = kIdlePollPeriodMs,
    };
    const PostFlushDecision onErr = settleFrameWait(finalDec, FlushOutcome::kError, 8000U, 1000U);
    CHECK(!onErr.markFinalFlushed && onErr.nextWaitMs == kShutdownFinalRetryMs);
    const PostFlushDecision onBackoff =
        settleFrameWait(finalDec, FlushOutcome::kBackoff, 1000U, 1000U);
    CHECK(!onBackoff.markFinalFlushed && onBackoff.nextWaitMs == kShutdownFinalRetryMs);
    const PostFlushDecision onDisabled =
        settleFrameWait(finalDec, FlushOutcome::kDisabled, 1000U, 1000U);
    CHECK(!onDisabled.markFinalFlushed && onDisabled.nextWaitMs == kIdlePollPeriodMs);
    const PostFlushDecision onWritten =
        settleFrameWait(finalDec, FlushOutcome::kWritten, 18000U, 1000U);
    CHECK(onWritten.markFinalFlushed && onWritten.nextWaitMs == kIdlePollPeriodMs);
    const PostFlushDecision onSkipped =
        settleFrameWait(finalDec, FlushOutcome::kSkipped, 1000U, 1000U);
    CHECK(onSkipped.markFinalFlushed && onSkipped.nextWaitMs == kIdlePollPeriodMs);
    UiDecision animDec{
        .page = OledPage::kDialplate,
        .markShutdownFinalFrame = false,
        .animating = true,
        .nextWaitMs = kAnimFramePeriodMs,
    };
    CHECK(settleFrameWait(animDec, FlushOutcome::kWritten, 12000U, 1000U).nextWaitMs == 21U);
    CHECK(settleFrameWait(animDec, FlushOutcome::kWritten, 50000U, 1000U).nextWaitMs == 1U);

    // R2OL2-03：空闲帧休眠前按双击绝对截止重算剩余时间（t=1250 决定睡 50 ms，刷屏耗 20 ms 到 t=1270 -> 仅睡 30 ms；超期到 1305 -> 睡 0 ms）。
    UiDecision idleClickDec{
        .page = OledPage::kDialplate,
        .markShutdownFinalFrame = false,
        .animating = false,
        .hasClickDeadline = true,
        .clickDeadlineMs = 1300U,
        .nextWaitMs = 50U,
    };
    CHECK(settleFrameWait(idleClickDec, FlushOutcome::kWritten, 20000U, 1270U).nextWaitMs == 30U);
    CHECK(settleFrameWait(idleClickDec, FlushOutcome::kWritten, 55000U, 1305U).nextWaitMs == 0U);
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
    std::array<uint64_t, kPages> hashes{};

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
    uint8_t dirty = tracker.computeDirtyMask(fb, hashes, false);
    CHECK(dirty == 0xFFU);
    for (uint8_t p = 0U; p < kPages; ++p) {
        tracker.commitPage(p, hashes[p]);
    }
    renderUi(st, vm, fb);
    CHECK(tracker.computeDirtyMask(fb, hashes, false) == 0x00U);

    // 仅改变顶栏卫星数 -> 只脏顶部受影响页，底部页保持干净。
    vm.satsUsed = 22U;
    renderUi(st, vm, fb);
    dirty = tracker.computeDirtyMask(fb, hashes, false);
    CHECK(dirty != 0x00U && dirty != 0xFFU);

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
    constexpr size_t kBatColBegin = 80U;
    constexpr size_t kBatColEnd = 108U;
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

    // 6b. 仅电量百分比滚动（85->72 递减 vs 60->72 递增）：电量区域中点 != 终帧、递减 != 递增、终帧 == 静态无滚帧，且卫星区域不受影响。
    NumberRoll batDown{};
    batDown.update(85, 0U);
    batDown.update(72, 100U);
    NumberRoll batUp{};
    batUp.update(60, 0U);
    batUp.update(72, 100U);
    UiRenderState batSt{};
    batSt.page = OledPage::kDialplate;
    batSt.batRoll = &batDown;
    batSt.nowMs = 160U;
    renderUi(batSt, rollVm, fb);
    batSt.nowMs = 350U;
    renderUi(batSt, rollVm, fb2);
    CHECK(!regionEqual(fb, fb2, 0U, kBatColBegin, kBatColEnd));
    CHECK(regionEqual(fb, fb2, 0U, kSatColBegin, kSatColEnd));
    batSt.batRoll = &batUp;
    batSt.nowMs = 160U;
    renderUi(batSt, rollVm, fbDir);
    CHECK(!regionEqual(fb, fbDir, 0U, kBatColBegin, kBatColEnd));
    batSt.batRoll = nullptr;
    renderUi(batSt, rollVm, fbDir);
    CHECK(regionEqual(fb2, fbDir, 0U, kBatColBegin, kBatColEnd));

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
    CHECK(!regionEqual(fb, fb2, 3U, 72U, 128U));
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
    CHECK(open.pose(40U, screen).contentOffsetY == 5);
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
    CHECK(once(2600U).action.action == OledAction::kNone);
    CHECK(ui.renderState(2900U).modal == UiModal::kNone);
    CHECK(twice(3100U).action.action == OledAction::kNone);
    CHECK(twice(3400U).action.action == OledAction::kWifi);

    static_cast<void>(once(3500U));
    CHECK(twice(4000U).action.action == OledAction::kTogglePair);
    vm.pairing.id = 41U;
    vm.pairing.requestDeadlineMs = 34000U;
    CHECK(twice(4200U).action.action == OledAction::kNone);
    CHECK(ui.renderState(4200U).modal == UiModal::kPair);
    const auto accept = twice(4400U).action;
    CHECK(accept.action == OledAction::kPairAccept && accept.pairId == 41U);
    CHECK(twice(4600U).action.action == OledAction::kNone);
    vm.pairing.id = 42U;
    CHECK(ui.step(4700U, {}, vm).action.action == OledAction::kNone);
    const auto reject = once(4800U).action;
    CHECK(reject.action == OledAction::kPairReject && reject.pairId == 42U);
    CHECK(ui.step(34000U, {}, vm).page == OledPage::kSubComm);
    CHECK(ui.renderState(34000U).modal == UiModal::kNone);

    static_cast<void>(once(34200U));
    CHECK(twice(34700U).page == OledPage::kDialplate);
    vm.radioMode = RadioMode::kWifiConfig;
    CHECK(twice(35300U).page == OledPage::kSubComm);
    CHECK(twice(35900U).action.action == OledAction::kBluetooth);
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
    testCommAndPairing();
    testUiEventBus();
    if (s_failures != 0) {
        std::printf("oled_ui_check: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    std::printf("oled_ui_check: OK\n");
    return EXIT_SUCCESS;
}
