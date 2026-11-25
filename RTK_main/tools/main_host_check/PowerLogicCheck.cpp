// 主机测试 B2 电源按键消抖、启动分支、关机状态机与充电待机长按开机流程（无 IDF 依赖）。
// 单线程主机测试，注入时间与 GPIO 桩，支持普通与 ASan/UBSan 构建。
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "board/PowerLogic.hpp"
#include "app/PowerRequests.hpp"

namespace {

using namespace app::board;

int s_failures = 0;

/**
 * @brief 累计并打印失败断言
 * @param ok: 断言结果
 * @param expr: 断言表达式文本
 * @note 单线程主机测试，非 ISR。
 */
void check(bool ok, const char *expr) noexcept
{
    if (!ok) {
        std::printf("FAIL %s\n", expr);
        ++s_failures;
    }
}

#define CHECK(cond) check((cond), #cond)

/** @brief 验证启动分支判定：GPIO25 高 / 低+USB / 低无 USB。 */
void testBootBranch() noexcept
{
    CHECK(decidePowerOn(true, false) == PowerOnAction::kHoldConfirm);
    CHECK(decidePowerOn(true, true) == PowerOnAction::kHoldConfirm);
    CHECK(decidePowerOn(false, true) == PowerOnAction::kChargeStandby);
    CHECK(decidePowerOn(false, false) == PowerOnAction::kHoldRecoverWarn);
}

/** @brief 验证按键消抖、短按、刚好 2500 ms 长按、不重复触发与时间回绕。 */
void testKeyStateMachine() noexcept
{
    PowerKeySm sm{2500U};
    CHECK(sm.poll(0U, false) == KeyEvent::kNone);
    CHECK(!sm.armed());
    CHECK(!sm.pressed());

    // 抖动（< 30 ms）不应触发按下或短按。
    CHECK(sm.poll(10U, true) == KeyEvent::kNone);
    CHECK(sm.poll(20U, true) == KeyEvent::kNone);
    CHECK(sm.poll(30U, false) == KeyEvent::kNone);
    CHECK(!sm.pressed());
    CHECK(sm.poll(60U, false) == KeyEvent::kNone);

    // 短按（< 600 ms）：t=100 按下，t=130 稳定按下，t=500 松开，t=530 稳定松开 -> kShort。
    CHECK(sm.poll(100U, true) == KeyEvent::kNone);
    CHECK(sm.poll(120U, true) == KeyEvent::kNone);
    CHECK(!sm.pressed() && !sm.holding());
    CHECK(sm.poll(130U, true) == KeyEvent::kNone);
    CHECK(sm.pressed() && sm.holding() && sm.pressStartMs() == 100U && sm.longPressMs() == 2500U);
    CHECK(sm.poll(500U, false) == KeyEvent::kNone);
    CHECK(sm.poll(520U, false) == KeyEvent::kNone);
    CHECK(sm.poll(530U, false) == KeyEvent::kShort);
    CHECK(!sm.pressed() && !sm.holding());
    CHECK(sm.lastHoldMs() == 400U);

    // 599 ms 仍为短按，600 ms 起进入长按浮层区：600..2499 ms 松开不发 kShort 也不发 kLongHold。
    CHECK(sm.poll(600U, true) == KeyEvent::kNone);
    CHECK(sm.poll(630U, true) == KeyEvent::kNone);
    CHECK(sm.holding() && sm.pressStartMs() == 600U);
    CHECK(sm.poll(1199U, false) == KeyEvent::kNone);
    CHECK(sm.poll(1229U, false) == KeyEvent::kShort);
    CHECK(sm.lastHoldMs() == 599U);

    CHECK(sm.poll(1300U, true) == KeyEvent::kNone);
    CHECK(sm.poll(1330U, true) == KeyEvent::kNone);
    CHECK(sm.holding() && sm.pressStartMs() == 1300U);
    CHECK(sm.poll(1900U, false) == KeyEvent::kNone);
    CHECK(sm.poll(1930U, false) == KeyEvent::kNone);
    CHECK(!sm.holding() && sm.lastHoldMs() == 600U);

    // 中等按压（600..2499 ms 松开）：不发 kShort 也不发 kLongHold。
    CHECK(sm.poll(2000U, true) == KeyEvent::kNone);
    CHECK(sm.poll(2030U, true) == KeyEvent::kNone);
    CHECK(sm.poll(2500U, false) == KeyEvent::kNone);
    CHECK(sm.poll(2530U, false) == KeyEvent::kShort);
    CHECK(sm.poll(2600U, true) == KeyEvent::kNone);
    CHECK(sm.poll(2630U, true) == KeyEvent::kNone);
    CHECK(sm.poll(2900U, false) == KeyEvent::kNone);
    CHECK(sm.poll(2930U, false) == KeyEvent::kShort);
    // 刚好 2500 ms 触发一次 kLongHold，holding() 随之复位，继续按住不重复触发，松开不发 kShort。
    CHECK(sm.poll(3000U, true) == KeyEvent::kNone);
    CHECK(sm.poll(3030U, true) == KeyEvent::kNone);
    CHECK(sm.holding() && sm.pressStartMs() == 3000U);
    CHECK(sm.poll(5499U, true) == KeyEvent::kNone);
    CHECK(sm.poll(5500U, true) == KeyEvent::kLongHold);
    CHECK(!sm.holding());
    CHECK(sm.poll(5510U, true) == KeyEvent::kNone);
    CHECK(sm.poll(8000U, true) == KeyEvent::kNone);
    CHECK(sm.poll(8100U, false) == KeyEvent::kNone);
    CHECK(sm.poll(8130U, false) == KeyEvent::kNone);
    // 启动时按键仍被按住：须先看到一次稳定松开，之前的按住不触发长按也不触发短按。
    PowerKeySm bootHeld{2500U};
    CHECK(bootHeld.poll(0U, true) == KeyEvent::kNone);
    CHECK(!bootHeld.armed());
    CHECK(!bootHeld.pressed());
    CHECK(bootHeld.poll(3000U, true) == KeyEvent::kNone);
    CHECK(bootHeld.pressed());
    CHECK(bootHeld.poll(5000U, true) == KeyEvent::kNone);
    // 20 ms 松开毛刺不算稳定松开。
    CHECK(bootHeld.poll(5010U, false) == KeyEvent::kNone);
    CHECK(bootHeld.poll(5030U, true) == KeyEvent::kNone);
    CHECK(!bootHeld.armed());
    // 30 ms 稳定松开后进入 armed，但不发 kShort。
    CHECK(bootHeld.poll(5100U, false) == KeyEvent::kNone);
    CHECK(bootHeld.poll(5130U, false) == KeyEvent::kNone);
    CHECK(bootHeld.armed());
    // 之后的按下正常触发长按。
    CHECK(bootHeld.poll(5200U, true) == KeyEvent::kNone);
    CHECK(bootHeld.poll(5230U, true) == KeyEvent::kNone);
    CHECK(bootHeld.poll(7700U, true) == KeyEvent::kLongHold);

    // uint32_t 时间回绕：跨越 0xFFFFFFFF -> 0。
    PowerKeySm wrap{2500U};
    const uint32_t t0 = 0xFFFFF800U; // 距离回绕 2048 ms
    CHECK(wrap.poll(t0, false) == KeyEvent::kNone);
    CHECK(wrap.poll(t0 + 30U, false) == KeyEvent::kNone);
    CHECK(wrap.poll(t0 + 40U, true) == KeyEvent::kNone);
    CHECK(wrap.poll(t0 + 70U, true) == KeyEvent::kNone);
    CHECK(wrap.pressed());
    CHECK(wrap.poll(t0 + 2539U, true) == KeyEvent::kNone);
    CHECK(wrap.poll(t0 + 2540U, true) == KeyEvent::kLongHold);
    CHECK(wrap.poll(t0 + 2600U, true) == KeyEvent::kNone);
}

/** @brief 验证无 USB 关机序列：阶段顺序、保存超时、2 s + 3.1 s 交替、30 s 超时回退与重复请求忽略。 */
void testShutdownNoUsb() noexcept
{
    ShutdownSm sm{};
    CHECK(!sm.active());
    CHECK(!sm.request(ShutdownReason::kNone));
    CHECK(sm.request(ShutdownReason::kKey));
    CHECK(sm.active());
    // 关机中再次请求忽略。
    CHECK(!sm.request(ShutdownReason::kConsole));
    CHECK(sm.reason() == ShutdownReason::kKey);

    // 1. kAnnounce
    auto out = sm.step(100U, false, false, true, false);
    CHECK(out.stage == ShutdownStage::kAnnounce && out.stageEntered && !out.driveKeyHigh);

    // 2. kSave
    constexpr uint32_t kSaveStart = 110U;
    constexpr uint32_t kSaveTimeoutAt = kSaveStart + kShutdownSaveTimeoutMs;
    out = sm.step(kSaveStart, false, false, true, false);
    CHECK(out.stage == ShutdownStage::kSave && out.stageEntered);
    out = sm.step(kSaveTimeoutAt - 1U, false, false, true, false);
    CHECK(out.stage == ShutdownStage::kSave && !out.stageEntered && !out.saveTimedOut);
    // 保存超时（kShutdownSaveTimeoutMs = 3000 ms）继续进 kQuiesce。
    out = sm.step(kSaveTimeoutAt, false, false, true, false);
    CHECK(out.stage == ShutdownStage::kQuiesce && out.stageEntered && out.saveTimedOut);

    // 3. kQuiesce 等待日志排空。
    out = sm.step(kSaveTimeoutAt + 40U, true, false, true, false);
    CHECK(out.stage == ShutdownStage::kQuiesce && !out.stageEntered);
    constexpr uint32_t kCutStart = kSaveTimeoutAt + 90U;
    out = sm.step(kCutStart, true, true, true, false);
    CHECK(out.stage == ShutdownStage::kCutPower && out.stageEntered && out.driveKeyHigh);

    // 4. kCutPower（无 USB）：2 s 高 -> 3.1 s 低 -> 3.1 s 高 -> 3.1 s 低。
    out = sm.step(kCutStart + 1999U, true, true, true, false);
    CHECK(out.stage == ShutdownStage::kCutPower && out.driveKeyHigh);
    out = sm.step(kCutStart + 2000U, true, true, true, false);
    CHECK(out.stage == ShutdownStage::kCutPower && !out.driveKeyHigh);
    out = sm.step(kCutStart + 5099U, true, true, true, false);
    CHECK(!out.driveKeyHigh);
    out = sm.step(kCutStart + 5100U, true, true, true, false);
    CHECK(out.driveKeyHigh);
    out = sm.step(kCutStart + 8200U, true, true, true, false);
    CHECK(!out.driveKeyHigh);

    // 30 s 超时回退：t = kCutStart + 30000。
    out = sm.step(kCutStart + kShutdownCutTimeoutMs - 1U, true, true, true, false);
    CHECK(out.stage == ShutdownStage::kCutPower && !out.cutTimedOut);
    out = sm.step(kCutStart + kShutdownCutTimeoutMs, true, true, true, false);
    CHECK(out.stage == ShutdownStage::kIdle && out.cutTimedOut && !out.driveKeyHigh &&
          !out.requestRestart);
    CHECK(!sm.active());
    CHECK(sm.lastReason() == ShutdownReason::kKey);

    // 回退后可再次受理新的关机请求。
    CHECK(sm.request(ShutdownReason::kConsole));
    CHECK(sm.reason() == ShutdownReason::kConsole);
}

/** @brief 验证有 USB 关机序列：正常保存完成后拉高 GPIO4 直到 GPIO25 读低并请求重启，及 30 s 超时。 */
void testShutdownWithUsb() noexcept
{
    ShutdownSm sm{};
    CHECK(sm.request(ShutdownReason::kConsole));

    auto out = sm.step(1000U, false, false, true, true);
    CHECK(out.stage == ShutdownStage::kAnnounce && out.stageEntered);
    out = sm.step(1010U, false, false, true, true);
    CHECK(out.stage == ShutdownStage::kSave && out.stageEntered);
    // 保存立即完成，无超时。
    out = sm.step(1020U, true, false, true, true);
    CHECK(out.stage == ShutdownStage::kQuiesce && out.stageEntered && !out.saveTimedOut);
    out = sm.step(1050U, true, true, true, true);
    CHECK(out.stage == ShutdownStage::kCutPower && out.stageEntered && out.driveKeyHigh);

    // GPIO25 仍高时持续拉高 GPIO4。
    out = sm.step(4000U, true, true, true, true);
    CHECK(out.stage == ShutdownStage::kCutPower && out.driveKeyHigh && !out.requestRestart);
    // GPIO25 变低：释放 GPIO4 并请求重启进入充电待机。
    out = sm.step(4100U, true, true, false, true);
    CHECK(!out.driveKeyHigh && out.requestRestart && !out.cutTimedOut);

    // 有 USB 且 GPIO25 始终不拉低：30 s 超时回退（含时间回绕测试）。
    ShutdownSm wrapSm{};
    CHECK(wrapSm.request(ShutdownReason::kKey));
    const uint32_t base = 0xFFFFF000U;
    static_cast<void>(wrapSm.step(base, false, false, true, true));
    static_cast<void>(wrapSm.step(base + 10U, false, false, true, true));
    static_cast<void>(wrapSm.step(base + 20U, true, false, true, true));
    const uint32_t cutStart = base + 30U;
    out = wrapSm.step(cutStart, true, true, true, true);
    CHECK(out.stage == ShutdownStage::kCutPower && out.driveKeyHigh);
    out = wrapSm.step(cutStart + 29999U, true, true, true, true);
    CHECK(out.stage == ShutdownStage::kCutPower && !out.cutTimedOut);
    out = wrapSm.step(cutStart + 30000U, true, true, true, true);
    CHECK(out.stage == ShutdownStage::kIdle && out.cutTimedOut && !out.driveKeyHigh);
}

/** @brief 验证释放毛刺和长按阈值前松开均不会误触发。 */
void testKeyReleaseBoundaries() noexcept
{
    PowerKeySm key{2500U};
    CHECK(key.poll(0U, false) == KeyEvent::kNone);
    CHECK(key.poll(10U, true) == KeyEvent::kNone);
    CHECK(key.poll(2510U, true) == KeyEvent::kNone);
    CHECK(!key.armed());
    CHECK(key.poll(3000U, false) == KeyEvent::kNone);
    CHECK(key.poll(3030U, false) == KeyEvent::kNone);
    CHECK(key.poll(3100U, true) == KeyEvent::kNone);
    CHECK(key.poll(3130U, true) == KeyEvent::kNone);
    CHECK(key.poll(5590U, false) == KeyEvent::kNone);
    CHECK(key.poll(5600U, false) == KeyEvent::kNone);
    CHECK(key.poll(5620U, false) == KeyEvent::kNone);
    CHECK(key.lastHoldMs() == 2490U);

    CHECK(key.poll(6000U, true) == KeyEvent::kNone);
    CHECK(key.poll(6030U, true) == KeyEvent::kNone);
    CHECK(key.poll(8490U, false) == KeyEvent::kNone);
    CHECK(key.poll(8500U, false) == KeyEvent::kNone);
    CHECK(key.poll(8510U, true) == KeyEvent::kLongHold);
}

/** @brief 验证待机数字保持确认、低脉冲复位计数、超时释放及可重试。 */
void testChargeStandbyLongPressPowerOn() noexcept
{
    StandbyWakeSm wake{};
    constexpr uint32_t kTimeout = 5000U;
    CHECK(!wake.active());
    CHECK(wake.confirmStandbyHold(0U, false, false, kTimeout) == StandbyWakeAction::kNone);
    CHECK(wake.confirmStandbyHold(2500U, true, true, kTimeout) == StandbyWakeAction::kPress);
    CHECK(wake.active() && wake.startedMs() == 2500U);
    CHECK(wake.confirmStandbyHold(2510U, false, true, kTimeout) == StandbyWakeAction::kNone);
    CHECK(wake.confirmStandbyHold(2520U, false, false, kTimeout) == StandbyWakeAction::kNone);
    CHECK(wake.confirmStandbyHold(2530U, false, true, kTimeout) == StandbyWakeAction::kNone);
    CHECK(wake.confirmStandbyHold(2540U, false, true, kTimeout) == StandbyWakeAction::kNone);
    CHECK(wake.confirmStandbyHold(2550U, false, true, kTimeout) == StandbyWakeAction::kHeld);
    CHECK(!wake.active());
    constexpr uint32_t kStart = 0xFFFFF000U;
    CHECK(wake.confirmStandbyHold(kStart, true, false, kTimeout) == StandbyWakeAction::kPress);
    CHECK(wake.confirmStandbyHold(kStart + kTimeout - 1U, false, false, kTimeout) ==
          StandbyWakeAction::kNone);
    CHECK(wake.confirmStandbyHold(kStart + kTimeout, false, false, kTimeout) ==
          StandbyWakeAction::kTimeout);
    CHECK(wake.confirmStandbyHold(kStart + kTimeout + 10U, false, true, kTimeout) ==
          StandbyWakeAction::kNone);
    CHECK(wake.confirmStandbyHold(kStart + kTimeout + 20U, true, false, kTimeout) ==
          StandbyWakeAction::kPress);
}

/** @brief 验证关机关闭设置准入、等待已获准入队、排除待机及普通重启。 */
void testPowerArbitration() noexcept
{
    app::power::PowerRequests requests{};
    CHECK(!requests.requestShutdown(BootMode::kChargeStandby, ShutdownReason::kConsole));
    CHECK(requests.canScheduleRestart());
    CHECK(requests.beginSetting());
    CHECK(requests.requestShutdown(BootMode::kNormal, ShutdownReason::kKey));
    CHECK(requests.shutdownReason() == ShutdownReason::kKey);
    CHECK(!requests.beginSetting());
    CHECK(!requests.settingsSubmitted());
    CHECK(!requests.canScheduleRestart());
    CHECK(!requests.claimRestart());
    CHECK(!requests.requestShutdown(BootMode::kNormal, ShutdownReason::kConsole));
    requests.endSetting();
    CHECK(requests.settingsSubmitted());
    requests.releaseAction();
    CHECK(requests.claimRestart());
    CHECK(!requests.requestShutdown(BootMode::kNormal, ShutdownReason::kKey));
    requests.releaseAction();
    CHECK(requests.beginSetting());
    requests.endSetting();
}

/** @brief 验证 U3 已关直接重启、保存期限回绕及切换 USB 协议后脉冲保持完整。 */
void testCutPowerBoundaries() noexcept
{
    ShutdownSm sm{};
    constexpr uint32_t kStart = 0xFFFFFF00U;
    CHECK(sm.request(ShutdownReason::kKey));
    static_cast<void>(sm.step(kStart, false, false, true, true));
    static_cast<void>(sm.step(kStart + 10U, false, false, true, true));
    CHECK(sm.step(kStart + 10U + kShutdownSaveTimeoutMs - 1U, false, false, true, true).stage ==
          ShutdownStage::kSave);
    CHECK(sm.step(kStart + 10U + kShutdownSaveTimeoutMs, false, false, true, true).saveTimedOut);
    const auto u3AlreadyOff = sm.step(1000U, true, true, false, true);
    CHECK(u3AlreadyOff.stageEntered && u3AlreadyOff.requestRestart && !u3AlreadyOff.driveKeyHigh);

    sm = {};
    CHECK(sm.request(ShutdownReason::kKey));
    static_cast<void>(sm.step(10U, false, false, true, false));
    static_cast<void>(sm.step(20U, false, false, true, false));
    static_cast<void>(sm.step(30U, true, false, true, false));
    CHECK(sm.step(50U, true, true, true, false).driveKeyHigh);
    CHECK(!sm.step(2150U, true, true, true, false).driveKeyHigh);
    CHECK(sm.step(2160U, true, true, true, true).driveKeyHigh);
    CHECK(sm.step(5260U, true, true, true, false).driveKeyHigh);
    CHECK(sm.step(7259U, true, true, true, false).driveKeyHigh);
    CHECK(!sm.step(7260U, true, true, true, false).driveKeyHigh);
    CHECK(!sm.step(10359U, true, true, true, false).driveKeyHigh);
    CHECK(sm.step(10360U, true, true, true, false).driveKeyHigh);
}

/** @brief 验证 kCutPower 从未知 USB 样本恢复为无 USB 时的首段 2 s 高电平与交替时序。 */
void testCutPowerNulloptRecovery() noexcept
{
    ShutdownSm sm{};
    CHECK(sm.request(ShutdownReason::kKey));
    static_cast<void>(sm.step(10U, false, false, true, false));
    static_cast<void>(sm.step(20U, false, false, true, false));
    static_cast<void>(sm.step(30U, true, false, true, false));
    // 首拍未知样本（A2）：GPIO4 保持低。
    const auto firstUnknown = sm.step(50U, true, true, false, std::nullopt);
    CHECK(firstUnknown.stage == ShutdownStage::kCutPower && firstUnknown.stageEntered &&
          !firstUnknown.driveKeyHigh && !firstUnknown.requestRestart);
    // 恢复为无 USB 后立即拉高，持续满 2000 ms 再进入 3.1 s 低/高交替。
    CHECK(sm.step(60U, true, true, true, false).driveKeyHigh);
    CHECK(sm.step(2059U, true, true, true, false).driveKeyHigh);
    CHECK(!sm.step(2060U, true, true, true, false).driveKeyHigh);
    CHECK(!sm.step(5159U, true, true, true, false).driveKeyHigh);
    CHECK(sm.step(5160U, true, true, true, false).driveKeyHigh);

    // true -> nullopt -> false 序列：恢复无 USB 后同样拉高满 2000 ms 再交替。
    sm = {};
    CHECK(sm.request(ShutdownReason::kConsole));
    static_cast<void>(sm.step(10U, false, false, true, true));
    static_cast<void>(sm.step(20U, false, false, true, true));
    static_cast<void>(sm.step(30U, true, false, true, true));
    CHECK(sm.step(50U, true, true, true, true).driveKeyHigh);
    CHECK(!sm.step(100U, true, true, false, std::nullopt).driveKeyHigh);
    CHECK(sm.step(110U, true, true, true, false).driveKeyHigh);
    CHECK(sm.step(2109U, true, true, true, false).driveKeyHigh);
    CHECK(!sm.step(2110U, true, true, true, false).driveKeyHigh);
    CHECK(!sm.step(5209U, true, true, true, false).driveKeyHigh);
    CHECK(sm.step(5210U, true, true, true, false).driveKeyHigh);
}

/** @brief 验证冷启动 GPIO27 或 REG08 迟到、有界无 USB 和 U3 已高快速退出。 */
void testBootUsbDelay() noexcept
{
    uint32_t now = 0U;
    const auto clock = [&] { return now; };
    const auto wait = [&](uint32_t ms) { now += ms; };
    const auto readLateUsb = [&] {
        return decideUsbPresent(std::nullopt, decodeSgmReg08(now >= 240U ? kSgmReg08PgBit : 0U),
                                false);
    };
    const auto first = readLateUsb();
    CHECK(!waitForBootUsb(true, first, clock, wait, readLateUsb).present);
    CHECK(now == 0U);
    CHECK(waitForBootUsb(false, first, clock, wait, readLateUsb).present);
    CHECK(now == 240U);
    now = 0U;
    CHECK(!waitForBootUsb(false, first, clock, wait, [&] { return first; }).present);
    CHECK(now == kBootUsbDetectionMs);

    now = 0U;
    const auto readLateGpio = [&] {
        return decideUsbPresent(now >= 60U, decodeSgmReg08(kSgmReg08PgBit), true);
    };
    const auto gpio = waitForBootUsb(false, readLateGpio(), clock, wait, readLateGpio);
    CHECK(gpio.present && gpio.source == UsbSource::kGpio && now == 60U);
}

/** @brief 验证 USB 在位来源优先级、REG08 解码与缓存。 */
void testUsbPresentAndChargerCache() noexcept
{
    // 1. REG08 解码与 I2C 在位判定。
    const ChargerInputStatus pgOnly = decodeSgmReg08(kSgmReg08PgBit);
    CHECK(pgOnly.ok && pgOnly.powerGood && pgOnly.vbusStat == 0U && pgOnly.chrgStat == 0U);
    const UsbDetection dPg = decideUsbPresent(std::nullopt, pgOnly, false);
    CHECK(dPg.present && dPg.source == UsbSource::kI2c);

    const ChargerInputStatus vbusOnly =
        decodeSgmReg08(static_cast<uint8_t>(1U << kSgmReg08VbusShift));
    CHECK(vbusOnly.ok && !vbusOnly.powerGood && vbusOnly.vbusStat == 1U);
    const UsbDetection dVbus = decideUsbPresent(std::nullopt, vbusOnly, false);
    CHECK(dVbus.present && dVbus.source == UsbSource::kI2c);

    const ChargerInputStatus noUsb = decodeSgmReg08(0x00U);
    CHECK(noUsb.ok && !noUsb.powerGood && noUsb.vbusStat == 0U);
    // I2C 成功且无输入电源时，不回退到 STAT。
    const UsbDetection dNoneI2c = decideUsbPresent(std::nullopt, noUsb, true);
    CHECK(!dNoneI2c.present && dNoneI2c.source == UsbSource::kI2c);

    // 2. I2C 失败退化到 STAT(GPIO2)。
    constexpr ChargerInputStatus kI2cFail{};
    const UsbDetection dStatLow = decideUsbPresent(std::nullopt, kI2cFail, true);
    CHECK(dStatLow.present && dStatLow.source == UsbSource::kStat);
    const UsbDetection dStatHigh = decideUsbPresent(std::nullopt, kI2cFail, false);
    CHECK(!dStatHigh.present && dStatHigh.source == UsbSource::kStat);
    const UsbDetection dNoHardware = decideUsbPresent(std::nullopt, kI2cFail, std::nullopt);
    CHECK(!dNoHardware.present && dNoHardware.source == UsbSource::kNone);

    // GPIO 高低均覆盖 REG08 和 STAT；缺失 GPIO 再按后两路优先级判定。
    for (const auto &i2c : {pgOnly, noUsb, kI2cFail}) {
        for (const bool stat : {false, true}) {
            const auto high = decideUsbPresent(true, i2c, stat);
            const auto low = decideUsbPresent(false, i2c, stat);
            CHECK(high.present && high.source == UsbSource::kGpio);
            CHECK(!low.present && low.source == UsbSource::kGpio);
            const auto missing = decideUsbPresent(std::nullopt, i2c, stat);
            CHECK(missing.source == (i2c.ok ? UsbSource::kI2c : UsbSource::kStat));
            CHECK(missing.present == (i2c.ok ? i2c.powerGood : stat));
        }
    }
    // 诊断占锁保留已有有效样本，通信故障则清除并走 STAT。
    auto cached = pgOnly;
    updateChargerSample(cached, fw::Status::kBusy, 0U);
    CHECK(decideUsbPresent(std::nullopt, cached, false).present);
    CHECK(cached.rawReg08 == kSgmReg08PgBit);
    updateChargerSample(cached, fw::Status::kTimeout, 0U);
    CHECK(!cached.ok && decideUsbPresent(std::nullopt, cached, true).source == UsbSource::kStat);
    updateChargerSample(cached, fw::Status::kOk, 0U);
    CHECK(cached.ok && !decideUsbPresent(std::nullopt, cached, true).present);
}

/** @brief 验证旧曲线节点、边界、插值与全量程单调性。 */
void testBatteryPercent() noexcept
{
    constexpr std::array<uint32_t, 11> kNodes{3340U, 3440U, 3540U, 3600U, 3630U, 3670U,
                                              3750U, 3860U, 3940U, 4050U, 4150U};
    for (size_t i = 0U; i < kNodes.size(); ++i) {
        CHECK(batteryPercent(kNodes[i]) == i * 10U);
    }
    CHECK(batteryPercent(0U) == 0U);
    CHECK(batteryPercent(3390U) == 5U);
    CHECK(batteryPercent(3439U) == 9U);
    CHECK(batteryPercent(4149U) == 99U);
    CHECK(batteryPercent(UINT32_MAX) == 100U);
    for (uint32_t mv = 3300U; mv < 4200U; ++mv) {
        CHECK(batteryPercent(mv) <= batteryPercent(mv + 1U));
    }
}

/** @brief 验证低电计时边界、USB 插入/样本失效复位及毫秒回绕。 */
void testLowBatteryTimer() noexcept
{
    LowBatteryTimer timer{};
    CHECK(!timer.isDue(0U, true, 9U));
    CHECK(!timer.isDue(kLowBatteryHoldMs - 1U, true, 9U));
    CHECK(timer.isDue(kLowBatteryHoldMs, true, 9U));
    CHECK(!timer.isDue(kLowBatteryHoldMs + 1U, false, 9U));
    CHECK(!timer.isDue(kLowBatteryHoldMs + 30000U, true, 9U));
    CHECK(!timer.isDue(kLowBatteryHoldMs + 60000U, true, 10U));
    CHECK(!timer.isDue(kLowBatteryHoldMs + 60001U, true, 9U));
    CHECK(!timer.isDue(kLowBatteryHoldMs + 90001U, true, kBatteryPctUnknown));
    constexpr uint32_t kStart = UINT32_MAX - 10000U;
    CHECK(!timer.isDue(kStart, true, 9U));
    CHECK(!timer.isDue(kStart + kLowBatteryHoldMs - 1U, true, 9U));
    CHECK(timer.isDue(kStart + kLowBatteryHoldMs, true, 9U));
    // 自定义测试保持时长与 kWeb 关机来源名。
    LowBatteryTimer custom{};
    CHECK(!custom.isDue(100U, true, 9U, 5000U));
    CHECK(!custom.isDue(5099U, true, 9U, 5000U));
    CHECK(custom.isDue(5100U, true, 9U, 5000U));
    CHECK(std::string_view{shutdownReasonName(ShutdownReason::kWeb)} == "web");
}

} // namespace

/**
 * @brief 运行 B2 电源按键与关机状态机主机用例
 * @retval EXIT_SUCCESS 全部用例通过
 * @retval EXIT_FAILURE 存在失败用例
 * @note 单线程主机测试，非 ISR。
 */
/** @brief 构造一次电量格采样。 @param pct: 电量。 @param usb: USB 在位。 @param chg: 充电。 @param sess: simbat 会话。 @retval 采样。 */
BatteryBarsInput barsIn(uint8_t pct, bool usb = false, bool chg = false,
                        uint32_t sess = 0U) noexcept
{
    return BatteryBarsInput{.pct = pct, .usbPresent = usb, .charging = chg, .simSession = sess};
}

/** @brief OL7：首个有效样本直接定格，格边界 20/40/60/80，未知为 0 格。 */
void testBatteryBarsLatchAndBoundaries() noexcept
{
    struct Case {
        uint8_t pct;
        uint8_t bars;
    };
    constexpr std::array<Case, 12U> kCases{{{0U, 1U},
                                            {19U, 1U},
                                            {20U, 2U},
                                            {39U, 2U},
                                            {40U, 3U},
                                            {59U, 3U},
                                            {60U, 4U},
                                            {79U, 4U},
                                            {80U, 5U},
                                            {99U, 5U},
                                            {100U, 5U},
                                            {55U, 3U}}};
    for (const auto &c : kCases) {
        BatteryBars b{};
        b.update(1000U, barsIn(c.pct));
        CHECK(b.view(1000U).bars == c.bars);
    }
    BatteryBars unknown{};
    unknown.update(0U, barsIn(kBatteryPctUnknown));
    CHECK(unknown.view(0U).bars == 0U && !unknown.view(0U).blinking);
    CHECK(unknown.nextWakeMs(0U) == UINT32_MAX);
    // 未知后恢复有效样本：直接定格。
    unknown.update(10U, barsIn(85U));
    CHECK(unknown.view(10U).bars == 5U);
}

/** @brief OL7：放电降格需低于当前格下界 3 个百分点并持续 30 s（±1 ms 边界）。 */
void testBatteryBarsDischargeHysteresis() noexcept
{
    BatteryBars b{};
    b.update(0U, barsIn(65U)); // 4 格（60..79）
    CHECK(b.view(0U).bars == 4U);
    // 58 仍在 3 点范围内（下界 60，需 <=57）。
    b.update(1000U, barsIn(58U));
    b.update(40000U, barsIn(58U));
    CHECK(b.view(40000U).bars == 4U);
    // 57 开始计时，29999 ms 不降，30000 ms 降一格。
    b.update(50000U, barsIn(57U));
    b.update(79999U, barsIn(57U));
    CHECK(b.view(79999U).bars == 4U);
    b.update(80000U, barsIn(57U));
    CHECK(b.view(80000U).bars == 3U);
    // 中途回到范围内清除计时：再次低于需重新累计 30 s。
    BatteryBars c{};
    c.update(0U, barsIn(65U));
    c.update(1000U, barsIn(50U));
    c.update(20000U, barsIn(61U)); // 清除计时
    c.update(21000U, barsIn(50U));
    c.update(50999U, barsIn(50U));
    CHECK(c.view(50999U).bars == 4U);
    c.update(51000U, barsIn(50U));
    CHECK(c.view(51000U).bars == 3U);
    // 骤降多格时每 30 s 只降一格。
    BatteryBars d{};
    d.update(0U, barsIn(95U)); // 5 格
    d.update(1U, barsIn(30U));
    d.update(30000U, barsIn(30U));
    CHECK(d.view(30000U).bars == 5U);
    d.update(30001U, barsIn(30U));
    CHECK(d.view(30001U).bars == 4U);
    d.update(60000U, barsIn(30U)); // 降格后下一次采样重新开始计时
    CHECK(d.view(60000U).bars == 4U);
    d.update(89999U, barsIn(30U));
    CHECK(d.view(89999U).bars == 4U);
    d.update(90000U, barsIn(30U));
    CHECK(d.view(90000U).bars == 3U);
    // 最低一格不再降。
    BatteryBars e{};
    e.update(0U, barsIn(30U));
    e.update(1U, barsIn(0U));
    e.update(40000U, barsIn(0U));
    e.update(80000U, barsIn(0U));
    CHECK(e.view(80000U).bars == 1U);
    // uint32_t 时钟回绕：计时跨越 0 仍按 30 s 判定。
    BatteryBars w{};
    const uint32_t t0 = UINT32_MAX - 9999U;
    w.update(t0 - 1U, barsIn(65U));
    w.update(t0, barsIn(50U));
    w.update(t0 + 29999U, barsIn(50U));
    CHECK(w.view(t0 + 29999U).bars == 4U);
    w.update(t0 + 30000U, barsIn(50U));
    CHECK(w.view(t0 + 30000U).bars == 3U);
}

/** @brief OL7：升格需高于当前格上界 5 个百分点并持续 60 s；短暂回弹不抬格。 */
void testBatteryBarsRecoveryHysteresis() noexcept
{
    BatteryBars b{};
    b.update(0U, barsIn(45U)); // 3 格（40..59）
    // 上界 60：64 点仍在 5 点范围内，不升。
    b.update(1000U, barsIn(64U));
    b.update(200000U, barsIn(64U));
    CHECK(b.view(200000U).bars == 3U);
    // 65 开始计时：59999 ms 不升，60000 ms 升一格。
    b.update(300000U, barsIn(65U));
    b.update(359999U, barsIn(65U));
    CHECK(b.view(359999U).bars == 3U);
    b.update(360000U, barsIn(65U));
    CHECK(b.view(360000U).bars == 4U);
    // 短暂回弹（< 60 s）后回落：不升格；重新回弹需重新累计 60 s。
    BatteryBars c{};
    c.update(0U, barsIn(45U));
    c.update(1000U, barsIn(70U));
    c.update(30000U, barsIn(70U));
    c.update(30001U, barsIn(50U)); // 清除计时
    c.update(100000U, barsIn(70U));
    c.update(159999U, barsIn(70U));
    CHECK(c.view(159999U).bars == 3U);
    c.update(160000U, barsIn(70U));
    CHECK(c.view(160000U).bars == 4U);
    // 最高一格不再升。
    BatteryBars top{};
    top.update(0U, barsIn(85U));
    top.update(1U, barsIn(99U));
    top.update(70000U, barsIn(99U));
    CHECK(top.view(70000U).bars == 5U);
}

/** @brief OL7：充电循环填充、充电期间冻结格数、充满 5 格常亮。 */
void testBatteryBarsCharging() noexcept
{
    BatteryBars b{};
    b.update(0U, barsIn(45U, true, true)); // 3 格充电
    // 3,4,5 循环，每 500 ms 一步。
    CHECK(b.view(0U).bars == 3U);
    CHECK(b.view(499U).bars == 3U);
    CHECK(b.view(500U).bars == 4U);
    CHECK(b.view(1000U).bars == 5U);
    CHECK(b.view(1500U).bars == 3U);
    CHECK(b.nextWakeMs(1100U) == 400U);
    CHECK(!b.view(500U).blinking);
    // 充电抬升读数（90）不改变起点格数，也不计升格：断充后保持期内仍为起点格。
    b.update(2000U, barsIn(90U, true, true));
    b.update(400000U, barsIn(90U, true, true));
    CHECK(b.view(400000U).bars == 3U + (400000U / 500U) % 3U);
    // 充满：5 格常亮，不再要求定时唤醒。
    b.update(400500U, barsIn(100U, true, false));
    CHECK(b.view(400500U).bars == 5U && b.view(401000U).bars == 5U);
    CHECK(b.nextWakeMs(401000U) == UINT32_MAX);
    // 满格时充电起点本身为 5：动画无需唤醒。
    BatteryBars full{};
    full.update(0U, barsIn(95U, true, true));
    CHECK(full.view(0U).bars == 5U && full.view(700U).bars == 5U);
    CHECK(full.nextWakeMs(0U) == UINT32_MAX);
}

/** @brief OL7：拔 USB 后保持原格数 60 s 再按新样本重新定格（跳过滞回）。 */
void testBatteryBarsUnplugHold() noexcept
{
    BatteryBars b{};
    b.update(0U, barsIn(95U, true, true)); // 充电中，5 格
    b.update(1000U, barsIn(70U, true, true));
    b.update(2000U, barsIn(70U, false, false)); // 拔出，充电抬升读数仍高
    CHECK(b.view(2000U).bars == 5U);
    b.update(61999U, barsIn(55U));
    CHECK(b.view(61999U).bars == 5U);
    CHECK(b.nextWakeMs(61999U) == 1U);
    b.update(62000U, barsIn(55U)); // 保持期满：直接定格为 3 格，不经 30 s 迟滞
    CHECK(b.view(62000U).bars == 3U);
    // 保持期内重新插入：回到充电逻辑，不再按旧样本定格。
    BatteryBars c{};
    c.update(0U, barsIn(65U, true, true));
    c.update(1000U, barsIn(65U));
    c.update(2000U, barsIn(65U, true, true));
    c.update(70000U, barsIn(65U, true, true));
    CHECK(c.view(70000U).bars >= 4U);
    // 保持期内低电 <10% 不闪烁（沿用旧格数展示）；期满后闪烁。
    BatteryBars low{};
    low.update(0U, barsIn(60U, true, true));
    low.update(1000U, barsIn(5U));
    CHECK(low.view(1000U).bars == 4U);
}

/** @brief OL7：<10% 最后一格 1 Hz 闪烁；未知不闪烁；10% 不闪烁。 */
void testBatteryBarsLowBlink() noexcept
{
    BatteryBars b{};
    b.update(0U, barsIn(9U));
    CHECK(b.view(0U).bars == 1U && b.view(0U).blinking && !b.view(0U).blinkOff);
    CHECK(b.view(500U).blinkOff);
    CHECK(!b.view(1000U).blinkOff);
    CHECK(b.nextWakeMs(100U) == 400U);
    BatteryBars ten{};
    ten.update(0U, barsIn(10U));
    CHECK(!ten.view(0U).blinking && ten.nextWakeMs(0U) == UINT32_MAX);
    // 低电时充电：显示循环填充动画而非闪烁。
    BatteryBars chg{};
    chg.update(0U, barsIn(5U, true, true));
    CHECK(!chg.view(500U).blinking && chg.view(500U).bars == 2U);
}

/** @brief OL7：power simbat 会话变化立即重新定格，跳过滞回与保持。 */
void testBatteryBarsSimSession() noexcept
{
    BatteryBars b{};
    b.update(0U, barsIn(85U, false, false, 1U));
    CHECK(b.view(0U).bars == 5U);
    b.update(100U, barsIn(15U, false, false, 1U)); // 同会话：需 30 s
    CHECK(b.view(100U).bars == 5U);
    b.update(200U, barsIn(15U, false, false, 2U)); // 新会话：立即定格
    CHECK(b.view(200U).bars == 1U);
    // 拔出保持期内也被会话变化打断。
    BatteryBars c{};
    c.update(0U, barsIn(90U, true, true, 1U));
    c.update(1000U, barsIn(90U, false, false, 1U));
    c.update(2000U, barsIn(30U, false, false, 2U));
    CHECK(c.view(2000U).bars == 2U);
    // reset 后下一样本直接定格。
    c.reset();
    c.update(3000U, barsIn(65U));
    CHECK(c.view(3000U).bars == 4U);
}

int main()
{
    testBootBranch();
    testKeyStateMachine();
    testKeyReleaseBoundaries();
    testBootUsbDelay();
    testPowerArbitration();
    testCutPowerBoundaries();
    testCutPowerNulloptRecovery();
    testShutdownNoUsb();
    testShutdownWithUsb();
    testChargeStandbyLongPressPowerOn();
    testUsbPresentAndChargerCache();
    testBatteryPercent();
    testLowBatteryTimer();
    testBatteryBarsLatchAndBoundaries();
    testBatteryBarsDischargeHysteresis();
    testBatteryBarsRecoveryHysteresis();
    testBatteryBarsCharging();
    testBatteryBarsUnplugHold();
    testBatteryBarsLowBlink();
    testBatteryBarsSimSession();
    return s_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
