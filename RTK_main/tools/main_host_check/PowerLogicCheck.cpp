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
    return s_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
