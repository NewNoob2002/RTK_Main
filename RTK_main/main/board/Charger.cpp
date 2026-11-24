// P2 SGM41513 I2C 充电芯片驱动实现（共享板级 I2C0 总线，100 kHz，超时 10 ms）。
// 静态互斥锁经 board::i2c::BusLock 串行化启动任务、kLink、OLED 与 console 访问，非 ISR。
// 寄存器位映射基于 BQ25601 兼容定义（待实板 `power charger` dump 核对）；
// 堆档位：启动期一次性；复用板级共享总线，失败时跳过设备访问。
#include "board/Charger.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "board/I2cBus.hpp"
#include "fw/Atomic.hpp"
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"

namespace app::board::charger {
namespace {

constexpr uint32_t kI2cFreqHz = 100000U;  // Hz，标准 100 kHz I2C
constexpr int kI2cXferTimeoutMs = 10;     // ms，单次传输超时上限
constexpr uint8_t kReg02IchgMask = 0x3FU; // REG02[5:0] 充电电流

struct ChargerState {
    i2c_master_dev_handle_t dev = nullptr;
    bool probed = false;
};

constinit ChargerState s_charger{};
constinit std::atomic<bool> s_ready{false};
static_assert(fw::kAtomicUsable<bool>, "充电芯片就绪标志原子须无锁");

/**
 * @brief 持锁下读取单个寄存器（不加锁，仅供内部已持锁调用）
 * @param reg: 寄存器地址
 * @param[out] value: 成功时写入读回值
 * @retval kOk 成功；其他为 IDF 错误映射
 * @note 调用方须已持有 i2c::BusLock，非 ISR。
 */
[[nodiscard]] fw::Status readRegLocked(uint8_t reg, uint8_t &value) noexcept
{
    uint8_t rx = 0U;
    const esp_err_t err =
        i2c_master_transmit_receive(s_charger.dev, &reg, 1U, &rx, 1U, kI2cXferTimeoutMs);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    value = rx;
    return fw::Status::kOk;
}

/**
 * @brief 持锁下写入单个寄存器（不加锁，仅供内部已持锁调用）
 * @param reg: 寄存器地址
 * @param value: 写入字节
 * @retval kOk 成功；其他为 IDF 错误映射
 * @note 调用方须已持有 i2c::BusLock，非 ISR。
 */
[[nodiscard]] fw::Status writeRegLocked(uint8_t reg, uint8_t value) noexcept
{
    const std::array<uint8_t, 2U> tx{reg, value};
    const esp_err_t err =
        i2c_master_transmit(s_charger.dev, tx.data(), tx.size(), kI2cXferTimeoutMs);
    return rtos::fromEspErr(err);
}

/**
 * @brief 探测充电芯片并区分无应答与总线故障
 * @param addr: 板级充电芯片的 7 位 I2C 地址
 * @retval kOk 收到应答
 * @retval kNotFound 从设备无应答
 * @retval kBusy 未取得总线锁
 * @retval 其他 IDF 探测错误映射
 * @note 仅启动任务在总线创建后调用，非 ISR。
 */
[[nodiscard]] fw::Status probeCharger(uint8_t addr) noexcept
{
    esp_err_t rawErr = ESP_OK;
    const fw::Status status = i2c::probe(addr, kI2cXferTimeoutMs, rawErr);
    // 探测失败时总线保留给其他共享器件与 DEV 诊断，充电芯片保持离线直至下次启动。
    if (status == fw::Status::kNotFound) {
        FW_LOGW(fw::TraceCat::kSys, "charger: no ack at 0x%02X, i2c path disabled",
                static_cast<unsigned>(addr));
        return fw::Status::kNotFound;
    }
    if (!fw::isOk(status)) {
        if (status != fw::Status::kBusy) {
            FW_LOGW(fw::TraceCat::kSys, "charger: probe at 0x%02X failed: %s, i2c path disabled",
                    static_cast<unsigned>(addr), esp_err_to_name(rawErr));
        }
        return status;
    }
    return fw::Status::kOk;
}

} // namespace

fw::Status init(const ChargerPins &pins) noexcept
{
    if (s_charger.probed) {
        return isReady() ? fw::Status::kOk : fw::Status::kNotFound;
    }
    if (pins.i2cPort < 0) {
        return fw::Status::kNotFound;
    }
    if ((pins.sdaGpio < 0) || (pins.sclGpio < 0) || (pins.addr == 0U)) {
        return fw::Status::kInvalidArg;
    }
    const auto busStatus = i2c::ensureInit(pins.i2cPort, pins.sdaGpio, pins.sclGpio);
    if (!fw::isOk(busStatus)) {
        return busStatus;
    }
    s_charger.probed = true;
    const auto probeStatus = probeCharger(pins.addr);
    if (!fw::isOk(probeStatus)) {
        return probeStatus;
    }

    const auto addStatus = i2c::addDevice(pins.addr, kI2cFreqHz, s_charger.dev);
    if (!fw::isOk(addStatus)) {
        FW_LOGW(fw::TraceCat::kSys, "charger i2c add dev 0x%02X failed (%s)",
                static_cast<unsigned>(pins.addr), fw::toString(addStatus));
        return addStatus;
    }

    s_ready.store(true, std::memory_order_release);
    return fw::Status::kOk;
}

bool isReady() noexcept
{
    return s_ready.load(std::memory_order_acquire);
}

fw::Status readReg(uint8_t reg, uint8_t &value) noexcept
{
    if (!s_ready.load(std::memory_order_acquire)) {
        return fw::Status::kInvalidState;
    }
    const i2c::BusLock guard;
    if (!guard.locked()) {
        return fw::Status::kBusy;
    }
    return readRegLocked(reg, value);
}

fw::Status refreshInputStatus(ChargerInputStatus &last) noexcept
{
    if (!isReady()) {
        last = {};
        return fw::Status::kInvalidState;
    }
    const i2c::BusLock guard(0U);
    if (!guard.locked()) {
        return fw::Status::kBusy;
    }
    uint8_t raw = 0U;
    const auto status = readRegLocked(kSgmReg08, raw);
    updateChargerSample(last, status, raw);
    return status;
}

fw::Status applyChargeParams(const ChargerPins &pins) noexcept
{
    if (!s_ready.load(std::memory_order_acquire)) {
        return fw::Status::kInvalidState;
    }
    const i2c::BusLock guard;
    if (!guard.locked()) {
        return fw::Status::kBusy;
    }
    uint8_t currentReg02 = 0U;
    const auto readStatus = readRegLocked(kSgmReg02, currentReg02);
    if (!fw::isOk(readStatus)) {
        return readStatus;
    }
    // 与旧 P2 一致，保留 BOOST_LIM、Q1_FULLON；掩码合并结果仍为一个字节。
    const auto reg02 = static_cast<uint8_t>((currentReg02 & 0xC0U) | (pins.reg02 & kReg02IchgMask));
    const std::array regs{kSgmReg02, kSgmReg03};
    const std::array expected{reg02, pins.reg03};
    constexpr std::array<uint8_t, 2U> kReadbackMasks{kReg02IchgMask, 0xFFU};
    for (size_t i = 0U; i < regs.size(); ++i) {
        const auto status = writeRegLocked(regs[i], expected[i]);
        if (!fw::isOk(status)) {
            return status;
        }
    }
    for (size_t i = 0U; i < regs.size(); ++i) {
        uint8_t actual = 0U;
        const auto status = readRegLocked(regs[i], actual);
        if (!fw::isOk(status)) {
            return status;
        }
        if ((actual & kReadbackMasks[i]) != (expected[i] & kReadbackMasks[i])) {
            FW_LOGW(fw::TraceCat::kSys,
                    "charger: REG%02X readback mismatch expected=0x%02X actual=0x%02X",
                    static_cast<unsigned>(regs[i]), static_cast<unsigned>(expected[i]),
                    static_cast<unsigned>(actual));
            return fw::Status::kCorrupt;
        }
    }
    return fw::Status::kOk;
}

fw::Status dumpRegs(std::span<uint8_t, kSgmRegCount> out) noexcept
{
    for (uint8_t reg = 0U; reg < kSgmRegCount; ++reg) {
        const fw::Status st = readReg(reg, out[reg]);
        if (!fw::isOk(st)) {
            return st;
        }
    }
    return fw::Status::kOk;
}

fw::Status scan(std::span<uint8_t, kScanAddressCount> addresses, size_t &count) noexcept
{
    return i2c::scanBus(addresses, count, kI2cXferTimeoutMs);
}

} // namespace app::board::charger
