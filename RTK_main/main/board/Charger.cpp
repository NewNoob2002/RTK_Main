// P2 SGM41513 I2C 充电芯片驱动实现（ESP-IDF v6.1 driver/i2c_master.h）。
// 100 kHz、超时 10 ms；静态互斥锁串行化启动任务、kLink 与 console 访问，非 ISR。
// 寄存器位映射基于 BQ25601 兼容定义（待实板 `power charger` dump 核对）；
// 堆档位：启动期一次性；IDF 仅创建一条总线及至多一个设备，失败时跳过设备访问。
#include "board/Charger.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fw/Atomic.hpp"
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"

namespace app::board::charger {
namespace {

constexpr uint32_t kI2cFreqHz = 100000U;    // Hz，标准 100 kHz I2C
constexpr int kI2cXferTimeoutMs = 10;       // ms，单次传输超时上限
constexpr uint32_t kMutexWaitMs = 20U;      // ms，跨任务互斥等待上限
constexpr uint8_t kI2cGlitchIgnoreCnt = 7U; // I2C 时钟周期毛刺滤波
constexpr uint8_t kReg02IchgMask = 0x3FU;   // REG02[5:0] 充电电流

struct ChargerState {
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t dev = nullptr;
    StaticSemaphore_t mutexBuf{};
    SemaphoreHandle_t mutex = nullptr;
};

constinit ChargerState s_charger{};
constinit std::atomic<bool> s_ready{false};
static_assert(fw::kAtomicUsable<bool>, "充电芯片就绪标志原子须无锁");

/** 静态互斥锁 RAII 守卫，保证单次/复合 I2C 事务跨任务串行。 */
class BusGuard final {
public:
    /**
     * @brief 尝试在有界超时内获取 I2C 互斥锁
     * @param handle: 静态创建的互斥锁句柄
     * @param ticks: 最大等待 tick，0 为立即尝试
     * @note 任意任务可调用，最多阻塞 kMutexWaitMs，非 ISR。
     */
    explicit BusGuard(SemaphoreHandle_t handle,
                      TickType_t ticks = pdMS_TO_TICKS(kMutexWaitMs)) noexcept
        : handle_(handle)
    {
        if (handle_ != nullptr) {
            locked_ = xSemaphoreTake(handle_, ticks) == pdTRUE;
        }
    }

    /** @brief 释放已持有的互斥锁；非 ISR。 */
    ~BusGuard() noexcept
    {
        if (locked_ && (handle_ != nullptr)) {
            static_cast<void>(xSemaphoreGive(handle_));
        }
    }

    BusGuard(const BusGuard &) = delete;
    BusGuard &operator=(const BusGuard &) = delete;

    /**
     * @brief 查询是否成功取得互斥锁
     * @retval true 已持锁
     * @retval false 获取超时或句柄为空
     * @note 不阻塞，非 ISR。
     */
    [[nodiscard]] bool locked() const noexcept
    {
        return locked_;
    }

private:
    SemaphoreHandle_t handle_ = nullptr;
    bool locked_ = false;
};

/**
 * @brief 持锁下读取单个寄存器（不加锁，仅供内部已持锁调用）
 * @param reg: 寄存器地址
 * @param[out] value: 成功时写入读回值
 * @retval kOk 成功；其他为 IDF 错误映射
 * @note 调用方须已持有 BusGuard，非 ISR。
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
 * @note 调用方须已持有 BusGuard，非 ISR。
 */
[[nodiscard]] fw::Status writeRegLocked(uint8_t reg, uint8_t value) noexcept
{
    const std::array<uint8_t, 2U> tx{reg, value};
    const esp_err_t err =
        i2c_master_transmit(s_charger.dev, tx.data(), tx.size(), kI2cXferTimeoutMs);
    return rtos::fromEspErr(err);
}

/**
 * @brief 建立静态互斥与单条 I2C 总线，供设备及扫描共用
 * @param pins: 已校验的板级 I2C 引脚配置
 * @retval kOk 总线可用
 * @retval kNoSpace 互斥或 SDK 总线资源不足
 * @retval 其他 IDF 总线错误映射
 * @note 仅启动任务调用，非 ISR。
 */
[[nodiscard]] fw::Status createBus(const ChargerPins &pins) noexcept
{
    if (s_charger.mutex == nullptr) {
        s_charger.mutex = xSemaphoreCreateMutexStatic(&s_charger.mutexBuf);
        if (s_charger.mutex == nullptr) {
            return fw::Status::kNoSpace;
        }
    }

    // 与旧 P2 一致，内部弱上拉与板上 R66/R68 (4.7 kΩ) 并联。
    // 引脚与 I2C 端口号来自板级静态表，枚举转换安全。
    i2c_master_bus_config_t busCfg{};
    busCfg.i2c_port = static_cast<i2c_port_num_t>(pins.i2cPort);
    busCfg.sda_io_num = static_cast<gpio_num_t>(pins.sdaGpio);
    busCfg.scl_io_num = static_cast<gpio_num_t>(pins.sclGpio);
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = kI2cGlitchIgnoreCnt;
    busCfg.flags.enable_internal_pullup = 1U;

    const esp_err_t err = i2c_new_master_bus(&busCfg, &s_charger.bus);
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "charger i2c bus init failed: %s", esp_err_to_name(err));
        return rtos::fromEspErr(err);
    }

    return fw::Status::kOk;
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
    esp_err_t err = ESP_OK;
    {
        const BusGuard guard(s_charger.mutex);
        if (!guard.locked()) {
            return fw::Status::kBusy;
        }
        err = i2c_master_probe(s_charger.bus, addr, kI2cXferTimeoutMs);
    }
    // 探测失败时总线保留给 DEV 诊断，充电芯片保持离线直至下次启动。
    if (err == ESP_ERR_NOT_FOUND) {
        FW_LOGW(fw::TraceCat::kSys, "charger: no ack at 0x%02X, i2c path disabled",
                static_cast<unsigned>(addr));
        return fw::Status::kNotFound;
    }
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "charger: probe at 0x%02X failed: %s, i2c path disabled",
                static_cast<unsigned>(addr), esp_err_to_name(err));
        return rtos::fromEspErr(err);
    }
    return fw::Status::kOk;
}

} // namespace

fw::Status init(const ChargerPins &pins) noexcept
{
    if (s_charger.bus != nullptr) {
        return isReady() ? fw::Status::kOk : fw::Status::kNotFound;
    }
    if (pins.i2cPort < 0) {
        return fw::Status::kNotFound;
    }
    if ((pins.sdaGpio < 0) || (pins.sclGpio < 0) || (pins.addr == 0U)) {
        return fw::Status::kInvalidArg;
    }
    const auto busStatus = createBus(pins);
    if (!fw::isOk(busStatus)) {
        return busStatus;
    }
    const auto probeStatus = probeCharger(pins.addr);
    if (!fw::isOk(probeStatus)) {
        return probeStatus;
    }

    i2c_device_config_t devCfg{};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address = pins.addr;
    devCfg.scl_speed_hz = kI2cFreqHz;

    const esp_err_t err = i2c_master_bus_add_device(s_charger.bus, &devCfg, &s_charger.dev);
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "charger i2c add dev 0x%02X failed: %s",
                static_cast<unsigned>(pins.addr), esp_err_to_name(err));
        return rtos::fromEspErr(err);
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
    const BusGuard guard(s_charger.mutex);
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
    const BusGuard guard(s_charger.mutex, 0U);
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
    const BusGuard guard(s_charger.mutex);
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
    count = 0U;
    if (s_charger.bus == nullptr) {
        return fw::Status::kInvalidState;
    }
    for (uint8_t addr = kScanFirstAddress; addr <= kScanLastAddress; ++addr) {
        const BusGuard guard(s_charger.mutex);
        if (!guard.locked()) {
            return fw::Status::kBusy;
        }
        const auto err = i2c_master_probe(s_charger.bus, addr, kI2cXferTimeoutMs);
        if (err == ESP_OK) {
            addresses[count++] = addr;
        } else if (err != ESP_ERR_NOT_FOUND) {
            return rtos::fromEspErr(err);
        }
    }
    return fw::Status::kOk;
}

} // namespace app::board::charger
