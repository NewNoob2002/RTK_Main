// 板级共享 I2C0 主总线单一所有者实现（ADR-0020 §2.2）。
// ensureInit 仅启动期串行调用；BusLock/probe/scan 通过静态互斥锁串行化多任务访问，非 ISR。
// 堆档位：启动期一次性（本文件不分配堆；IDF i2c_new_master_bus/add_device 仅在启动期一次性内部分配）。
#include "board/I2cBus.hpp"

#include "driver/gpio.h"
#if defined(ESP_PLATFORM)
#include "esp_rom_sys.h"
#endif
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"

namespace app::board::i2c {
namespace {

constexpr uint8_t kI2cGlitchIgnoreCnt = 7U;    // I2C 时钟周期毛刺滤波
constexpr unsigned kRecoveryMaxClocks = 9U;    // I2C 规范释放卡死从机所需最多 9 个 SCL 脉冲
constexpr uint32_t kRecoveryHalfPeriodUs = 5U; // us，100 kHz 半周期（5 us）

/**
 * @brief 总线位操作半周期延时（目标板约 5 us，主机测试空操作）
 * @note 仅启动期总线恢复调用，非 ISR。
 */
void delayBusHalfPeriod() noexcept
{
#if defined(ESP_PLATFORM)
    esp_rom_delay_us(kRecoveryHalfPeriodUs);
#endif
}

struct BusState {
    i2c_master_bus_handle_t bus = nullptr;
    StaticSemaphore_t mutexBuf{};
    SemaphoreHandle_t mutex = nullptr;
    int port = 0;
    int sdaGpio = 0;
    int sclGpio = 0;
};

constinit BusState s_bus{};

} // namespace

BusLock::BusLock(TickType_t ticks) noexcept : handle_(s_bus.mutex)
{
    if (handle_ != nullptr) {
        locked_ = xSemaphoreTake(handle_, ticks) == pdTRUE;
    }
}

/** @brief 释放已持有的共享 I2C 总线互斥锁；非 ISR。 */
BusLock::~BusLock() noexcept
{
    if (locked_ && (handle_ != nullptr)) {
        static_cast<void>(xSemaphoreGive(handle_));
    }
}

bool recoverBusPins(int sdaGpio, int sclGpio) noexcept
{
    if ((sdaGpio < 0) || (sclGpio < 0)) {
        return false;
    }
    const auto sdaPin = static_cast<gpio_num_t>(sdaGpio);
    const auto sclPin = static_cast<gpio_num_t>(sclGpio);
    const gpio_config_t cfg{
        .pin_bit_mask =
            (1ULL << static_cast<unsigned>(sdaGpio)) | (1ULL << static_cast<unsigned>(sclGpio)),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return false;
    }
    static_cast<void>(gpio_set_level(sdaPin, 1U));
    static_cast<void>(gpio_set_level(sclPin, 1U));
    delayBusHalfPeriod();
    if (gpio_get_level(sdaPin) != 0) {
        return true;
    }

    FW_LOGW(fw::TraceCat::kSys, "i2c SDA GPIO%d stuck low before init, clocking %u SCL pulses",
            sdaGpio, kRecoveryMaxClocks);
    for (unsigned pulse = 0U; pulse < kRecoveryMaxClocks; ++pulse) {
        static_cast<void>(gpio_set_level(sclPin, 0U));
        delayBusHalfPeriod();
        static_cast<void>(gpio_set_level(sclPin, 1U));
        delayBusHalfPeriod();
        if (gpio_get_level(sdaPin) != 0) {
            break;
        }
    }
    // 发送标准 STOP 条件（SCL 高期间 SDA 由低拉高），通知从机结束残余事务。
    static_cast<void>(gpio_set_level(sclPin, 0U));
    delayBusHalfPeriod();
    static_cast<void>(gpio_set_level(sdaPin, 0U));
    delayBusHalfPeriod();
    static_cast<void>(gpio_set_level(sclPin, 1U));
    delayBusHalfPeriod();
    static_cast<void>(gpio_set_level(sdaPin, 1U));
    delayBusHalfPeriod();

    const bool released = gpio_get_level(sdaPin) != 0;
    if (!released) {
        FW_LOGW(fw::TraceCat::kSys, "i2c SDA GPIO%d still low after bus recovery", sdaGpio);
    }
    return released;
}

fw::Status ensureInit(int port, int sdaGpio, int sclGpio) noexcept
{
    if ((port < 0) || (sdaGpio < 0) || (sclGpio < 0)) {
        return fw::Status::kInvalidArg;
    }
    if (s_bus.bus != nullptr) {
        if ((s_bus.port == port) && (s_bus.sdaGpio == sdaGpio) && (s_bus.sclGpio == sclGpio)) {
            return fw::Status::kOk;
        }
        return fw::Status::kInvalidArg;
    }
    if (s_bus.mutex == nullptr) {
        s_bus.mutex = xSemaphoreCreateMutexStatic(&s_bus.mutexBuf);
        if (s_bus.mutex == nullptr) {
            return fw::Status::kNoSpace;
        }
    }

    // 软件重启若打断进行中的从机事务，SDA 可能保持低电平；创建主总线前先释放总线。
    static_cast<void>(recoverBusPins(sdaGpio, sclGpio));

    // 与旧 P2 一致，内部弱上拉与板上 R66/R68 (4.7 kΩ) 并联。
    // 引脚与 I2C 端口号经前置非负检查且来自板级静态表，枚举转换安全。
    i2c_master_bus_config_t busCfg{};
    busCfg.i2c_port = static_cast<i2c_port_num_t>(port);
    busCfg.sda_io_num = static_cast<gpio_num_t>(sdaGpio);
    busCfg.scl_io_num = static_cast<gpio_num_t>(sclGpio);
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = kI2cGlitchIgnoreCnt;
    busCfg.flags.enable_internal_pullup = 1U;

    const esp_err_t err = i2c_new_master_bus(&busCfg, &s_bus.bus);
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "i2c bus init failed: %s", esp_err_to_name(err));
        return rtos::fromEspErr(err);
    }

    s_bus.port = port;
    s_bus.sdaGpio = sdaGpio;
    s_bus.sclGpio = sclGpio;
    return fw::Status::kOk;
}

bool isReady() noexcept
{
    return (s_bus.bus != nullptr) && (s_bus.mutex != nullptr);
}

fw::Status probe(uint8_t addr, int timeoutMs, esp_err_t &rawErr, uint32_t lockWaitMs) noexcept
{
    rawErr = ESP_ERR_INVALID_STATE;
    if (s_bus.bus == nullptr) {
        return fw::Status::kInvalidState;
    }
    const BusLock guard(pdMS_TO_TICKS(lockWaitMs));
    if (!guard.locked()) {
        rawErr = ESP_ERR_TIMEOUT;
        return fw::Status::kBusy;
    }
    for (unsigned attempt = 0U; attempt <= kProbeRecoveryRetries; ++attempt) {
        rawErr = i2c_master_probe(s_bus.bus, addr, timeoutMs);
        if (rawErr == ESP_OK) {
            return fw::Status::kOk;
        }
        if (rawErr == ESP_ERR_NOT_FOUND) {
            return fw::Status::kNotFound;
        }
        if (attempt < kProbeRecoveryRetries) {
            static_cast<void>(i2c_master_bus_reset(s_bus.bus));
        }
    }
    return rtos::fromEspErr(rawErr);
}

fw::Status addDevice(uint8_t addr, uint32_t sclSpeedHz, i2c_master_dev_handle_t &out) noexcept
{
    if (s_bus.bus == nullptr) {
        return fw::Status::kInvalidState;
    }
    if ((addr == 0U) || (sclSpeedHz == 0U)) {
        return fw::Status::kInvalidArg;
    }
    i2c_device_config_t devCfg{};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address = addr;
    devCfg.scl_speed_hz = sclSpeedHz;

    i2c_master_dev_handle_t dev = nullptr;
    const esp_err_t err = i2c_master_bus_add_device(s_bus.bus, &devCfg, &dev);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    out = dev;
    return fw::Status::kOk;
}

fw::Status removeDevice(i2c_master_dev_handle_t &dev) noexcept
{
    if (dev == nullptr) {
        return fw::Status::kOk;
    }
    if (s_bus.bus == nullptr) {
        dev = nullptr;
        return fw::Status::kInvalidState;
    }
    const BusLock guard;
    if (!guard.locked()) {
        return fw::Status::kBusy;
    }
    const esp_err_t err = i2c_master_bus_rm_device(dev);
    dev = nullptr;
    return rtos::fromEspErr(err);
}

fw::Status resetBus(uint32_t lockWaitMs) noexcept
{
    if (s_bus.bus == nullptr) {
        return fw::Status::kInvalidState;
    }
    const BusLock guard(pdMS_TO_TICKS(lockWaitMs));
    if (!guard.locked()) {
        return fw::Status::kBusy;
    }
    const esp_err_t err = i2c_master_bus_reset(s_bus.bus);
    return rtos::fromEspErr(err);
}

fw::Status scanBus(std::span<uint8_t, kScanAddressCount> addresses, size_t &count,
                   int timeoutMs) noexcept
{
    count = 0U;
    if (s_bus.bus == nullptr) {
        return fw::Status::kInvalidState;
    }
    for (uint8_t addr = kScanFirstAddress; addr <= kScanLastAddress; ++addr) {
        const BusLock guard;
        if (!guard.locked()) {
            return fw::Status::kBusy;
        }
        const esp_err_t err = i2c_master_probe(s_bus.bus, addr, timeoutMs);
        if (err == ESP_OK) {
            addresses[count++] = addr;
        } else if (err != ESP_ERR_NOT_FOUND) {
            return rtos::fromEspErr(err);
        }
    }
    return fw::Status::kOk;
}

} // namespace app::board::i2c
