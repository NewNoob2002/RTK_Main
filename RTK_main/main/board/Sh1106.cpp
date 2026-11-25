// P2 SH1106 128x64 OLED 驱动实现（ADR-0020，共享板级 I2C0 总线，400 kHz）。
// init 仅启动任务调用；flush/powerOff/wake 仅 OLED 低优先级任务调用，按页加锁，非 ISR。
// 堆档位：启动期一次性（本文件不分配堆；IDF i2c_master_bus_add_device 仅在启动期一次性内部分配）。
#include "board/Sh1106.hpp"

#include <cstring>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
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

// 同步驱动可对多个等待阶段重复使用入参，传参预算只约束应用层分配给单页的时限。
static_assert(Sh1106::kPageTimeoutBudgetMs < i2c::kDefaultMutexWaitMs,
              "SH1106 page timeout budget must remain below I2C mutex wait timeout");
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

/**
 * @brief 探测指定 7-bit I2C 地址，NACK 时额外执行总线复位重试
 * @param addr: 7-bit 设备地址
 * @param timeoutMs: 单次探测超时（ms）
 * @param[out] rawErr: 末次底层 IDF 错误码
 * @retval kOk 探测成功；其他为失败错误码
 * @note 仅 Sh1106::probeAndAttach 调用，非 ISR。
 */
fw::Status probeAddrWithRecovery(uint8_t addr, int timeoutMs, esp_err_t &rawErr) noexcept
{
    fw::Status status = fw::Status::kNotFound;
    // i2c::probe 对超时/总线故障已内置复位重试；此处额外对 NACK 执行 >=2 次复位重试。
    for (unsigned attempt = 0U; attempt <= i2c::kProbeRecoveryRetries; ++attempt) {
        status = i2c::probe(addr, timeoutMs, rawErr);
        if (fw::isOk(status)) {
            return status;
        }
        if ((status != fw::Status::kNotFound) || (attempt >= i2c::kProbeRecoveryRetries)) {
            break;
        }
        static_cast<void>(i2c::resetBus());
    }
    return status;
}

/**
 * @brief 输出 OLED 初始化阶段失败告警（后台静默重试时跳过）
 * @param stage: 失败阶段
 * @param status: 错误码
 * @param addr: 7-bit I2C 地址
 * @param powerGpio: 电源使能脚号
 * @param rawErr: 探测失败时的底层 IDF 错误码
 * @param quiet: true 时抑制告警
 * @note 仅初始化失败路径调用，非 ISR。
 */
void logInitFailure(OledInitStage stage, fw::Status status, uint8_t addr, int powerGpio,
                    esp_err_t rawErr, bool quiet) noexcept
{
    if (quiet) {
        return;
    }
    const char *detail =
        (stage == OledInitStage::kProbe) ? esp_err_to_name(rawErr) : fw::toString(status);
    FW_LOGW(fw::TraceCat::kSys,
            "<oled> init failed at %s (0x%02X, pwr GPIO%d): %s, display disabled",
            oledInitStageName(stage), static_cast<unsigned>(addr), powerGpio, detail);
}

} // namespace

fw::Status Sh1106::finishTransfer(esp_err_t err) noexcept
{
    // IDF v6.1 同步传输的内部超时和 NACK 都可能以 INVALID_RESPONSE 返回。
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE) {
        static_cast<void>(i2c::resetBus());
    }
    return rtos::fromEspErr(err);
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

    esp_err_t err = ESP_OK;
    {
        const i2c::BusLock guard;
        if (!guard.locked()) {
            return fw::Status::kBusy;
        }
        constexpr int64_t kUsPerMs = 1000LL;
        constexpr int64_t kBudgetUs = static_cast<int64_t>(kPageTimeoutBudgetMs) * kUsPerMs;
        const int64_t lockStartUs = esp_timer_get_time();
        err = i2c_master_transmit(dev_, pageCmd.data(), pageCmd.size(),
                                  static_cast<int>(kPageTimeoutBudgetMs));
        if (err == ESP_OK) {
            const int64_t afterCmdUs = esp_timer_get_time();
            const int64_t elapsedUs = (afterCmdUs > lockStartUs) ? (afterCmdUs - lockStartUs) : 0LL;
            const int64_t remUs = kBudgetUs - elapsedUs;
            if (remUs < kUsPerMs) {
                // 剩余预算不足 1 ms，避免向 IDF 传入零超时。
                err = ESP_ERR_TIMEOUT;
            } else {
                const int dataTimeoutMs = static_cast<int>(remUs / kUsPerMs);
                err =
                    i2c_master_transmit(dev_, pageTxBuf_.data(), pageTxBuf_.size(), dataTimeoutMs);
            }
        }
    }
    return finishTransfer(err);
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
        esp_err_t err = ESP_OK;
        {
            const i2c::BusLock guard;
            if (!guard.locked()) {
                return fw::Status::kBusy;
            }
            err = i2c_master_transmit(dev_, initCmds.data(), initCmds.size(), kI2cXferTimeoutMs);
        }
        const fw::Status initStatus = finishTransfer(err);
        if (!fw::isOk(initStatus)) {
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
    esp_err_t err = ESP_OK;
    {
        const i2c::BusLock guard;
        if (!guard.locked()) {
            return fw::Status::kBusy;
        }
        err = i2c_master_transmit(dev_, onCmd.data(), onCmd.size(), kI2cXferTimeoutMs);
    }
    return finishTransfer(err);
}

void Sh1106::resetForInit(const OledPins &pins) noexcept
{
    if (dev_ != nullptr) {
        static_cast<void>(i2c::removeDevice(dev_));
    }
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
    lastDirtyPages_ = 0U;
    totalDirtyPages_ = 0U;
    i2cErrors_ = 0U;
    lastInitStage_ = OledInitStage::kNone;
    lastInitStatus_ = fw::Status::kOk;
    tracker_.invalidate();
}

fw::Status Sh1106::probeAndAttach(bool quiet) noexcept
{
    const fw::Status busStatus = i2c::ensureInit(i2cPort_, sdaGpio_, sclGpio_);
    if (!fw::isOk(busStatus)) {
        lastInitStage_ = OledInitStage::kI2cBus;
        lastInitStatus_ = busStatus;
        logInitFailure(lastInitStage_, busStatus, addr_, powerGpio_, ESP_OK, quiet);
        return busStatus;
    }

    esp_err_t rawErr = ESP_OK;
    const fw::Status probeStatus = probeAddrWithRecovery(addr_, kI2cXferTimeoutMs, rawErr);
    if (!fw::isOk(probeStatus)) {
        lastInitStage_ = OledInitStage::kProbe;
        lastInitStatus_ = probeStatus;
        logInitFailure(lastInitStage_, probeStatus, addr_, powerGpio_, rawErr, quiet);
        return probeStatus;
    }

    const fw::Status addStatus = i2c::addDevice(addr_, kI2cFreqHz, dev_);
    if (!fw::isOk(addStatus)) {
        lastInitStage_ = OledInitStage::kAddDevice;
        lastInitStatus_ = addStatus;
        logInitFailure(lastInitStage_, addStatus, addr_, powerGpio_, ESP_OK, quiet);
    }
    return addStatus;
}

/**
 * @brief 使能 OLED 电源脚、探测 0x3C（失败自动复位重试）并初始化 SH1106
 * @param pins: 板级 OLED 引脚、地址与旋转配置
 * @param quiet: true 时抑制常规失败告警日志（用于后台周期重试，避免刷屏）
 * @retval kOk 面板初始化完成并已亮屏；其他见 Sh1106.hpp 声明
 */
fw::Status Sh1106::init(const OledPins &pins, bool quiet) noexcept
{
    resetForInit(pins);
    if (pins.i2cPort < 0) {
        lastInitStage_ = OledInitStage::kDisabled;
        lastInitStatus_ = fw::Status::kNotFound;
        return fw::Status::kNotFound;
    }
    if ((pins.sdaGpio < 0) || (pins.sclGpio < 0) || (pins.addr == 0U)) {
        lastInitStage_ = OledInitStage::kDisabled;
        lastInitStatus_ = fw::Status::kInvalidArg;
        return fw::Status::kInvalidArg;
    }
    if (const fw::Status pwr = drivePowerPin(true); !fw::isOk(pwr)) {
        lastInitStage_ = OledInitStage::kPowerPin;
        lastInitStatus_ = pwr;
        logInitFailure(lastInitStage_, pwr, addr_, pins.powerGpio, ESP_OK, quiet);
        return pwr;
    }
    if (hasPowerGpio_) {
        sleepMs(kPowerSettleMs);
    }
    if (const fw::Status attachStatus = probeAndAttach(quiet); !fw::isOk(attachStatus)) {
        static_cast<void>(drivePowerPin(false));
        return attachStatus;
    }
    if (const fw::Status cfgStatus = configurePanel(); !fw::isOk(cfgStatus)) {
        static_cast<void>(i2c::removeDevice(dev_));
        static_cast<void>(drivePowerPin(false));
        lastInitStage_ = OledInitStage::kConfigure;
        lastInitStatus_ = cfgStatus;
        logInitFailure(lastInitStage_, cfgStatus, addr_, pins.powerGpio, ESP_OK, quiet);
        return cfgStatus;
    }
    present_ = true;
    lastInitStage_ = OledInitStage::kOk;
    lastInitStatus_ = fw::Status::kOk;
    FW_LOGI(fw::TraceCat::kSys, "<oled> SH1106 ready at 0x%02X (400 kHz, pwr GPIO%d)",
            static_cast<unsigned>(pins.addr), pins.powerGpio);
    return fw::Status::kOk;
}

/**
 * @brief 按页对比摘要，仅将发生变化的页经 I2C 写出到 SH1106（每页独立加解锁）
 * @param fb: 8×128 单色帧缓冲（WouoUI ScreenBuff）
 * @param forceFull: true 时跳过摘要对比，强制写出全部 8 页
 * @param deadlineMs: 可选的单调毫秒截止，到期时在发下一脏页前中止
 * @retval kWritten 至少写出一页且无错误
 * @retval kSkipped 内容完全未变，未发起任何 I2C 写操作
 * @retval kInterrupted 截止已到且还有脏页未发送
 * @retval kError 至少一页写失败
 * @retval kDisabled 面板未就绪或已断电
 * @note 仅启动任务或 OLED 任务调用；驱动等待和恢复可能超过传参预算，非 ISR。
 */
Sh1106FlushResult Sh1106::flush(const ScreenBuff &fb, bool forceFull,
                                std::optional<uint32_t> deadlineMs) noexcept
{
    lastDirtyPages_ = 0U;
    if (!present_ || poweredOff_ || (dev_ == nullptr)) {
        return Sh1106FlushResult::kDisabled;
    }
    if (forceFull) {
        backoffSkips_ = 0U;
    } else if (backoffSkips_ > 0U) {
        --backoffSkips_;
        return Sh1106FlushResult::kBackoff;
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
        if (deadlineMs.has_value() && static_cast<int32_t>(rtos::nowMs() - *deadlineMs) >= 0) {
            return Sh1106FlushResult::kInterrupted;
        }
        const fw::Status st = writePage(page, std::span<const uint8_t, oled::kWidth>{fb[page]});
        if (fw::isOk(st)) {
            tracker_.commitPage(page, hashes[page]);
            ++lastDirtyPages_;
            ++totalDirtyPages_;
            continue;
        }
        if (st != fw::Status::kBusy) {
            tracker_.invalidatePage(page);
        }
        ++i2cErrors_;
        if (consecutiveErrors_ < kMaxErrorBackoffTicks) {
            ++consecutiveErrors_;
        }
        // 首次失败允许下轮重试，连续失败时退避至最多 kMaxErrorBackoffTicks 轮。
        if (consecutiveErrors_ >= 2U) {
            const uint8_t shift = static_cast<uint8_t>(consecutiveErrors_ - 1U);
            const uint8_t rawSkips = static_cast<uint8_t>(1U << shift);
            backoffSkips_ = (rawSkips < kMaxErrorBackoffTicks) ? rawSkips : kMaxErrorBackoffTicks;
        }
        return Sh1106FlushResult::kError;
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
        esp_err_t err = ESP_OK;
        bool locked = false;
        {
            const i2c::BusLock guard;
            locked = guard.locked();
            if (locked) {
                err = i2c_master_transmit(dev_, offCmd.data(), offCmd.size(), kI2cXferTimeoutMs);
            }
        }
        if (!locked || !fw::isOk(finishTransfer(err))) {
            ++i2cErrors_;
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
