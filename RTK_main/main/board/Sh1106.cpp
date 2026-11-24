// P2 SH1106 128x64 OLED 驱动实现（ADR-0020，共享板级 I2C0 总线，400 kHz）。
// init 仅启动任务调用；flush/powerOff/wake 仅 OLED 低优先级任务调用，按页加锁，非 ISR。
// 堆档位：启动期一次性（本文件不分配堆；IDF i2c_master_bus_add_device 仅在启动期一次性内部分配）。
#include "board/Sh1106.hpp"

#include <cstring>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board/I2cBus.hpp"
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"

namespace app::board {
namespace {

// SH1106 控制字节与初始化寄存器命令常量（依据 SH1106 数据手册与旧 P2 xyz_oled.c:339-384）。
constexpr uint8_t kCtrlCmd = 0x00U;
constexpr uint8_t kCtrlData = 0x40U;
constexpr uint8_t kCmdDisplayOff = 0xAEU;
constexpr uint8_t kCmdDisplayOn = 0xAFU;
constexpr uint8_t kCmdDcDcCtrl = 0xADU;
constexpr uint8_t kCmdDcDcOff = 0x8AU;
constexpr uint8_t kCmdDcDcOn = 0x8BU;
constexpr uint8_t kCmdPageBase = 0xB0U;
constexpr uint8_t kPageIndexMask = 0x07U;
constexpr uint8_t kNibbleMask = 0x0FU;
constexpr uint8_t kColHighBase = 0x10U;
constexpr uint8_t kCmdColLow = Sh1106::kColumnOffset & kNibbleMask;
constexpr uint8_t kCmdColHigh = kColHighBase | ((Sh1106::kColumnOffset >> 4U) & kNibbleMask);
constexpr uint8_t kCmdStartLine0 = 0x40U;
constexpr uint8_t kCmdContrast = 0x81U;
constexpr uint8_t kValContrast = 0xCFU;
constexpr uint8_t kCmdSegRemapNormal = 0xA1U;
constexpr uint8_t kCmdSegRemapFlipped = 0xA0U;
constexpr uint8_t kCmdComScanNormal = 0xC8U;
constexpr uint8_t kCmdComScanFlipped = 0xC0U;
constexpr uint8_t kCmdNormalDisplay = 0xA6U;
constexpr uint8_t kCmdMultiplex = 0xA8U;
constexpr uint8_t kValMultiplex64 = 0x3FU;
constexpr uint8_t kCmdDisplayOffset = 0xD3U;
constexpr uint8_t kValDisplayOffset0 = 0x00U;
constexpr uint8_t kCmdClockDiv = 0xD5U;
constexpr uint8_t kValClockDiv = 0x80U;
constexpr uint8_t kCmdPrecharge = 0xD9U;
constexpr uint8_t kValPrecharge = 0xF1U;
constexpr uint8_t kCmdComPins = 0xDAU;
constexpr uint8_t kValComPins = 0x12U;
constexpr uint8_t kCmdVcomh = 0xDBU;
constexpr uint8_t kValVcomh = 0x40U;
constexpr uint8_t kCmdEntireDispRam = 0xA4U;

// writePage() 在单次 BusLock 内最多发起 2 次传输，持锁上限必须严格小于跨任务互斥等待上限（ROL1-02）。
static_assert((2U * static_cast<uint32_t>(Sh1106::kI2cXferTimeoutMs)) < i2c::kDefaultMutexWaitMs,
              "SH1106 single-page lock hold must remain below I2C mutex wait timeout");

/**
 * @brief 毫秒级让出等待（至少 1 tick）
 * @param ms: 等待毫秒数
 * @note 仅任务上下文调用，可阻塞，非 ISR。
 */
void sleepMs(uint32_t ms) noexcept
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks == 0U ? 1U : ticks);
}

} // namespace

void Sh1106::recoverBusOnTimeout(fw::Status status) noexcept
{
    if (status == fw::Status::kTimeout) {
        static_cast<void>(i2c::resetBus());
    }
}

fw::Status Sh1106::drivePowerPin(bool high) noexcept
{
    if (!hasPowerGpio_) {
        return fw::Status::kOk;
    }
    // 引脚来自板级静态表且已判非负，枚举与位掩码转换安全。
    const auto pin = static_cast<gpio_num_t>(powerGpio_);
    esp_err_t err = gpio_set_level(pin, high ? 1U : 0U);
    if (err != ESP_OK) {
        return rtos::fromEspErr(err);
    }
    if (!powerPinConfigured_) {
        const gpio_config_t config{.pin_bit_mask = 1ULL << static_cast<unsigned>(powerGpio_),
                                   .mode = GPIO_MODE_OUTPUT,
                                   .pull_up_en = GPIO_PULLUP_DISABLE,
                                   .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                   .intr_type = GPIO_INTR_DISABLE};
        err = gpio_config(&config);
        if (err != ESP_OK) {
            return rtos::fromEspErr(err);
        }
        powerPinConfigured_ = true;
    }
    return fw::Status::kOk;
}

fw::Status Sh1106::writePage(uint8_t page, std::span<const uint8_t, oled::kWidth> data) noexcept
{
    // page < 8，与 kCmdPageBase (0xB0) 按位或结果仍在 1 字节内。
    const std::array<uint8_t, 4U> pageCmd{
        kCtrlCmd, static_cast<uint8_t>(kCmdPageBase | (page & kPageIndexMask)), kCmdColLow,
        kCmdColHigh};
    pageTxBuf_[0] = kCtrlData;
    const auto payload = std::span{pageTxBuf_}.subspan(1U);
    std::memcpy(payload.data(), data.data(), data.size());

    fw::Status status = fw::Status::kOk;
    {
        const i2c::BusLock guard;
        if (!guard.locked()) {
            return fw::Status::kBusy;
        }
        esp_err_t err =
            i2c_master_transmit(dev_, pageCmd.data(), pageCmd.size(), kI2cXferTimeoutMs);
        if (err == ESP_OK) {
            err =
                i2c_master_transmit(dev_, pageTxBuf_.data(), pageTxBuf_.size(), kI2cXferTimeoutMs);
        }
        status = rtos::fromEspErr(err);
    }
    recoverBusOnTimeout(status);
    return status;
}

fw::Status Sh1106::configurePanel() noexcept
{
    const uint8_t segRemap = rotated180_ ? kCmdSegRemapFlipped : kCmdSegRemapNormal;
    const uint8_t comScan = rotated180_ ? kCmdComScanFlipped : kCmdComScanNormal;
    const std::array initCmds{
        kCtrlCmd,          kCmdDisplayOff,     kCmdColLow,    kCmdColHigh,
        kCmdStartLine0,    kCmdContrast,       kValContrast,  segRemap,
        comScan,           kCmdNormalDisplay,  kCmdMultiplex, kValMultiplex64,
        kCmdDisplayOffset, kValDisplayOffset0, kCmdClockDiv,  kValClockDiv,
        kCmdPrecharge,     kValPrecharge,      kCmdComPins,   kValComPins,
        kCmdVcomh,         kValVcomh,          kCmdDcDcCtrl,  kCmdDcDcOn,
        kCmdEntireDispRam, kCmdNormalDisplay,
    };
    {
        fw::Status initStatus = fw::Status::kOk;
        {
            const i2c::BusLock guard;
            if (!guard.locked()) {
                return fw::Status::kBusy;
            }
            const esp_err_t err =
                i2c_master_transmit(dev_, initCmds.data(), initCmds.size(), kI2cXferTimeoutMs);
            initStatus = rtos::fromEspErr(err);
        }
        if (!fw::isOk(initStatus)) {
            recoverBusOnTimeout(initStatus);
            return initStatus;
        }
    }

    // 开显示前清空 8 页显存，避免上电花屏。
    constexpr std::array<uint8_t, oled::kWidth> kZeroPage{};
    for (uint8_t page = 0U; page < oled::kPages; ++page) {
        const fw::Status st = writePage(page, kZeroPage);
        if (!fw::isOk(st)) {
            return st;
        }
    }

    const std::array<uint8_t, 2U> onCmd{kCtrlCmd, kCmdDisplayOn};
    fw::Status onStatus = fw::Status::kOk;
    {
        const i2c::BusLock guard;
        if (!guard.locked()) {
            return fw::Status::kBusy;
        }
        const esp_err_t err =
            i2c_master_transmit(dev_, onCmd.data(), onCmd.size(), kI2cXferTimeoutMs);
        onStatus = rtos::fromEspErr(err);
    }
    recoverBusOnTimeout(onStatus);
    return onStatus;
}

fw::Status Sh1106::probeAndAttach() noexcept
{
    const fw::Status busStatus = i2c::ensureInit(i2cPort_, sdaGpio_, sclGpio_);
    if (!fw::isOk(busStatus)) {
        FW_LOGW(fw::TraceCat::kSys, "<oled> i2c bus unavailable (%s), display disabled",
                fw::toString(busStatus));
        return busStatus;
    }

    esp_err_t rawErr = ESP_OK;
    const fw::Status probeStatus = i2c::probe(addr_, kI2cXferTimeoutMs, rawErr);
    if (!fw::isOk(probeStatus)) {
        FW_LOGW(fw::TraceCat::kSys,
                probeStatus == fw::Status::kNotFound
                    ? "<oled> no ack at 0x%02X, display disabled"
                    : "<oled> probe at 0x%02X failed (%s), display disabled",
                static_cast<unsigned>(addr_), esp_err_to_name(rawErr));
        return probeStatus;
    }

    const fw::Status addStatus = i2c::addDevice(addr_, kI2cFreqHz, dev_);
    if (!fw::isOk(addStatus)) {
        FW_LOGW(fw::TraceCat::kSys, "<oled> add dev 0x%02X failed (%s), display disabled",
                static_cast<unsigned>(addr_), fw::toString(addStatus));
    }
    return addStatus;
}

fw::Status Sh1106::init(const OledPins &pins) noexcept
{
    hasPowerGpio_ = pins.powerGpio >= 0;
    powerGpio_ = hasPowerGpio_ ? pins.powerGpio : 0;
    i2cPort_ = (pins.i2cPort >= 0) ? pins.i2cPort : 0;
    sdaGpio_ = (pins.sdaGpio >= 0) ? pins.sdaGpio : 0;
    sclGpio_ = (pins.sclGpio >= 0) ? pins.sclGpio : 0;
    addr_ = pins.addr;
    rotated180_ = pins.rotated180;
    present_ = false;
    poweredOff_ = false;
    consecutiveErrors_ = 0U;
    backoffSkips_ = 0U;
    i2cErrors_ = 0U;
    tracker_.invalidate();

    if (pins.i2cPort < 0) {
        return fw::Status::kNotFound;
    }
    if ((pins.sdaGpio < 0) || (pins.sclGpio < 0) || (pins.addr == 0U)) {
        return fw::Status::kInvalidArg;
    }
    if (const fw::Status pwr = drivePowerPin(true); !fw::isOk(pwr)) {
        FW_LOGW(fw::TraceCat::kSys, "<oled> power GPIO%d failed (%s), display disabled",
                pins.powerGpio, fw::toString(pwr));
        return pwr;
    }
    if (hasPowerGpio_) {
        sleepMs(kPowerSettleMs);
    }

    const fw::Status attachStatus = probeAndAttach();
    if (!fw::isOk(attachStatus)) {
        static_cast<void>(drivePowerPin(false));
        return attachStatus;
    }

    const fw::Status cfgStatus = configurePanel();
    if (!fw::isOk(cfgStatus)) {
        static_cast<void>(i2c::removeDevice(dev_));
        static_cast<void>(drivePowerPin(false));
        FW_LOGW(fw::TraceCat::kSys, "<oled> panel init failed (%s), display disabled",
                fw::toString(cfgStatus));
        return cfgStatus;
    }

    present_ = true;
    FW_LOGI(fw::TraceCat::kSys, "<oled> SH1106 ready at 0x%02X (400 kHz, pwr GPIO%d)",
            static_cast<unsigned>(pins.addr), pins.powerGpio);
    return fw::Status::kOk;
}

Sh1106FlushResult Sh1106::flush(const ScreenBuff &fb, bool forceFull) noexcept
{
    if (!present_ || poweredOff_ || (dev_ == nullptr)) {
        return Sh1106FlushResult::kDisabled;
    }
    if (forceFull) {
        backoffSkips_ = 0U;
    } else if (backoffSkips_ > 0U) {
        --backoffSkips_;
        return Sh1106FlushResult::kSkipped;
    }

    std::array<uint64_t, oled::kPages> hashes{};
    const uint8_t dirtyMask = tracker_.computeDirtyMask(fb, hashes, forceFull);
    if (dirtyMask == 0U) {
        return Sh1106FlushResult::kSkipped;
    }

    for (uint8_t page = 0U; page < oled::kPages; ++page) {
        if ((dirtyMask & static_cast<uint8_t>(1U << page)) == 0U) {
            continue;
        }
        const fw::Status st = writePage(page, std::span<const uint8_t, oled::kWidth>{fb[page]});
        if (fw::isOk(st)) {
            tracker_.commitPage(page, hashes[page]);
        } else {
            ++i2cErrors_;
            if (consecutiveErrors_ < kMaxErrorBackoffTicks) {
                ++consecutiveErrors_;
            }
            // 首次失败立即允许下轮重试，连续失败时指数退避至最多 kMaxErrorBackoffTicks 轮，避免拖垮总线。
            if (consecutiveErrors_ >= 2U) {
                const uint8_t shift = static_cast<uint8_t>(consecutiveErrors_ - 1U);
                const uint8_t rawSkips = static_cast<uint8_t>(1U << shift);
                backoffSkips_ =
                    (rawSkips < kMaxErrorBackoffTicks) ? rawSkips : kMaxErrorBackoffTicks;
            }
            return Sh1106FlushResult::kError;
        }
    }
    consecutiveErrors_ = 0U;
    backoffSkips_ = 0U;
    return Sh1106FlushResult::kWritten;
}

void Sh1106::powerOff() noexcept
{
    if (!present_ || poweredOff_) {
        return;
    }
    if (dev_ != nullptr) {
        const std::array<uint8_t, 4U> offCmd{kCtrlCmd, kCmdDisplayOff, kCmdDcDcCtrl, kCmdDcDcOff};
        fw::Status offStatus = fw::Status::kOk;
        {
            const i2c::BusLock guard;
            if (guard.locked()) {
                const esp_err_t err =
                    i2c_master_transmit(dev_, offCmd.data(), offCmd.size(), kI2cXferTimeoutMs);
                offStatus = rtos::fromEspErr(err);
            } else {
                offStatus = fw::Status::kBusy;
            }
        }
        if (!fw::isOk(offStatus)) {
            ++i2cErrors_;
            recoverBusOnTimeout(offStatus);
        }
    }
    if (!fw::isOk(drivePowerPin(false))) {
        FW_LOGW(fw::TraceCat::kSys, "<oled> power-off GPIO%d failed", powerGpio_);
    }
    poweredOff_ = true;
    consecutiveErrors_ = 0U;
    backoffSkips_ = 0U;
    tracker_.invalidate();
}

fw::Status Sh1106::wakeFromPowerOff() noexcept
{
    if (!present_ || (dev_ == nullptr)) {
        return fw::Status::kInvalidState;
    }
    if (!poweredOff_) {
        return fw::Status::kOk;
    }
    const fw::Status pwrStatus = drivePowerPin(true);
    if (!fw::isOk(pwrStatus)) {
        return pwrStatus;
    }
    if (hasPowerGpio_) {
        sleepMs(kPowerSettleMs);
    }
    const fw::Status cfgStatus = configurePanel();
    if (!fw::isOk(cfgStatus)) {
        static_cast<void>(drivePowerPin(false));
        ++i2cErrors_;
        return cfgStatus;
    }
    poweredOff_ = false;
    consecutiveErrors_ = 0U;
    backoffSkips_ = 0U;
    tracker_.invalidate();
    return fw::Status::kOk;
}

} // namespace app::board
