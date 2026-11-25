// 电源模块控制台命令组（DEV 档）：电源状态、关机、充电寄存器与 I2C 诊断。
// 仅在 console REPL 任务调用，非 ISR；不分配堆。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/power/PowerModule.hpp"

#include <array>
#include <cinttypes>
#include <span>
#include <string_view>

#include "AppContext.hpp"
#include "board/BoardProfile.hpp"
#include "board/Charger.hpp"
#include "cli/Out.hpp"

namespace app {
namespace {

/**
 * @brief 显示电源引脚、电池、开机原因、按键计数与关机状态
 * @param argc: 参数个数（含动词）
 * @retval 0 已输出状态
 * @retval 非零 参数错误
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
int cmdPowerStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: power status");
    }
    const auto &self = appContext();
    const PowerSnapshot snap = self.data.powerStatus.read();
    const auto *profile = self.boardProfile;
    cli::printSection("power");
    cli::printKv("board", "%s (active=%s)", profile != nullptr ? profile->name : "unknown",
                 snap.active ? "yes" : "no");
    cli::printKv("boot_mode", "%s", board::bootModeName(self.bootMode));
    cli::printKv("power_on", "%s", board::powerOnReasonName(self.powerOnReason));
    cli::printKv("pins", "key=%u u3_on=%u usb=%u chg=%u usb_src=%s", snap.keyPressed ? 1U : 0U,
                 snap.u3On ? 1U : 0U, snap.usbPresent ? 1U : 0U, snap.charging ? 1U : 0U,
                 board::usbSourceName(snap.usbSource));
    if (snap.batteryPct == board::kBatteryPctUnknown) {
        cli::printKv("battery", "bat_mv=n/a bat_pct=n/a");
    } else {
        cli::printKv("battery", "bat_mv=%" PRIu32 " bat_pct=%u", snap.batteryMv,
                     static_cast<unsigned>(snap.batteryPct));
    }
    if (snap.chargerOk) {
        cli::printKv("charger", "reg08=0x%02X", static_cast<unsigned>(snap.chargerReg08));
    } else {
        cli::printKv("charger", "reg08=n/a");
    }
    cli::printKv("key_count", "short=%" PRIu32 " long=%" PRIu32, snap.shortPresses,
                 snap.longPresses);
    cli::printKv("shutdown", "stage=%s last=%s", board::shutdownStageName(snap.shutdownStage),
                 board::shutdownReasonName(snap.lastShutdown));
    return 0;
}

/**
 * @brief 只读转储 SGM41513 寄存器 REG00..REG0B（便于实板核对位定义）
 * @param argc: 参数个数（含动词）
 * @retval 0 已输出寄存器表
 * @retval 非零 参数错误、板型无充电芯片或 I2C 读取失败
 * @note 仅 console REPL 任务，I2C 读取有界超时，非 ISR。
 */
int cmdPowerCharger(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: power charger");
    }
    const auto &chargerPins = board::current().charger;
    if (chargerPins.i2cPort < 0) {
        return cli::fail("charger unsupported on this board");
    }
    std::array<uint8_t, board::kSgmRegCount> regs{};
    const fw::Status st = board::charger::dumpRegs(regs);
    if (!fw::isOk(st)) {
        return cli::fail("charger dump failed: %s", fw::toString(st));
    }
    cli::printSection("charger");
    cli::printKv("i2c", "port=%d sda=%d scl=%d addr=0x%02X", chargerPins.i2cPort,
                 chargerPins.sdaGpio, chargerPins.sclGpio, static_cast<unsigned>(chargerPins.addr));
    for (size_t i = 0U; i < regs.size(); ++i) {
        cli::outPrintf("  REG%02X = 0x%02X\n", static_cast<unsigned>(i),
                       static_cast<unsigned>(regs[i]));
    }
    const board::ChargerInputStatus decoded = board::decodeSgmReg08(regs[board::kSgmReg08]);
    const bool vbusGd = (regs[board::kSgmReg0A] & board::kSgmReg0AVbusGdBit) != 0U;
    cli::printKv("reg08_dec", "vbus_stat=%u chrg_stat=%u pg_stat=%u vbus_gd=%u",
                 static_cast<unsigned>(decoded.vbusStat), static_cast<unsigned>(decoded.chrgStat),
                 decoded.powerGood ? 1U : 0U, vbusGd ? 1U : 0U);
    return 0;
}

/**
 * @brief 扫描 I2C0 的应答地址
 * @param argc: 参数个数（含 scan 动词）
 * @retval 0 已列出全部应答地址
 * @retval 非零 参数错误或无法完成探测
 * @note 仅 DEV console REPL 任务；探测每地址最多 10 ms，非 ISR。
 */
int cmdPowerI2cScan(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: power i2c scan");
    }
    std::array<uint8_t, board::charger::kScanAddressCount> addresses{};
    size_t count = 0U;
    const auto status = board::charger::scan(addresses, count);
    cli::printSection("I2C0");
    for (size_t i = 0U; i < count; ++i) {
        cli::outPrintf("  0x%02X\n", static_cast<unsigned>(addresses[i]));
    }
    if (!fw::isOk(status)) {
        return cli::fail("I2C scan failed: %s", fw::toString(status));
    }
    return 0;
}

constexpr std::array<cli::Verb, 1U> kI2cVerbs{{
    {"scan", nullptr, "scan for responding addresses on I2C0", &cmdPowerI2cScan},
}};

/**
 * @brief 分派 I2C 诊断子命令
 * @param ctx: 控制台上下文
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 子命令成功
 * @retval 非零 参数或执行错误
 * @note 仅 DEV console REPL 任务，非 ISR。
 */
int cmdPowerI2c(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kI2cVerbs, ctx, argc, argv);
}

/**
 * @brief 通过电源管理发起控制台软关机请求
 * @param argc: 参数个数（含动词）
 * @retval 0 已受理关机请求
 * @retval 非零 参数错误、板型不支持或正在关机中
 * @note 仅 console REPL 任务，非 ISR；不在控制台任务上下文直接断电。
 */
int cmdPowerOff(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: power off");
    }
    if (board::current().powerHold.kind != board::HoldKind::kKeyEmulate) {
        return cli::fail("power off unsupported on this board");
    }
    if (appContext().bootMode == board::BootMode::kChargeStandby) {
        return cli::fail("already in charge standby");
    }
    if (!power::requestShutdown(board::ShutdownReason::kConsole)) {
        return cli::fail("power action already in progress");
    }
    cli::outPrintf("power off requested\n");
    return 0;
}

/**
 * @brief 注入或取消台架低电模拟百分比（仅 DEV 档 HIL 验证，不改变产品默认值）
 * @param argc: 参数个数（含动词）
 * @param argv: 参数数组（<0-100|off>）
 * @retval 0 已更新模拟电量状态
 * @retval 非零 参数错误
 * @note 仅 DEV console REPL 任务，不阻塞，非 ISR。
 */
int cmdPowerSimBat(void * /*ctx*/, int argc, char **argv) noexcept
{
    constexpr uint8_t kMaxBatPct = 100U;
    if ((argc != 2) || (argv == nullptr)) {
        return cli::fail("usage: power simbat <0-100|off>");
    }
    const std::span<char *const> args{argv, 2U};
    if (args.back() == nullptr) {
        return cli::fail("usage: power simbat <0-100|off>");
    }
    if (std::string_view{args.back()} == "off") {
        power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
        cli::outPrintf("simulated battery cleared\n");
        return 0;
    }
    uint8_t pct = 0U;
    if (!cli::parseU8(args.back(), pct) || (pct > kMaxBatPct)) {
        return cli::fail("usage: power simbat <0-100|off>");
    }
    power::setSimulatedBatteryPct(pct);
    cli::outPrintf("simulated battery=%u%%\n", static_cast<unsigned>(pct));
    return 0;
}

constexpr std::array<cli::Verb, 5U> kPowerVerbs{{
    {"status", nullptr, "show power pins, boot reason, key counts and shutdown state",
     &cmdPowerStatus},
    {"off", nullptr, "request clean power shutdown (P2 only)", &cmdPowerOff},
    {"charger", nullptr, "dump SGM41513 registers REG00-REG0B (read-only)", &cmdPowerCharger},
    {"i2c", "scan", "scan for responding addresses on I2C0", &cmdPowerI2c},
    {"simbat", "<0-100|off>", "inject simulated battery percent for HIL low-battery test",
     &cmdPowerSimBat},
}};

/**
 * @brief 分派 power 子命令，缺省显示 power status
 * @param ctx: 控制台上下文
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 子命令成功
 * @retval 非零 参数或执行错误
 * @note 仅 console REPL 任务，非 ISR。
 */
int cmdPower(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kPowerVerbs, ctx, argc, argv, "status");
}

constexpr std::array<cli::Cmd, 1U> kPowerCmds{{
    {"power",
     "power [status|off|charger|i2c scan|simbat <0-100|off>]: inspect power/charger state or "
     "request shutdown",
     "[status|off|charger|i2c scan|simbat <0-100|off>]", &cmdPower, nullptr},
}};

} // namespace

std::span<const cli::Cmd> powerCmds() noexcept
{
    return kPowerCmds;
}

std::span<const cli::Verb> powerVerbs() noexcept
{
    return kPowerVerbs;
}

} // namespace app
