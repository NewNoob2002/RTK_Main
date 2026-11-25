// 用 SDK 边界替身验证共享板级 I2C0 总线（Charger + SH1106 共存）、400 kHz 初始化、按页加解锁、
// 空闲跳过与关机断电（OL1）。每个进程运行一个单线程场景，无硬件访问。
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "board/Charger.hpp"
#include "board/I2cBus.hpp"
#include "board/Sh1106.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw/Log.hpp"
#include "modules/oled/OledUi.hpp"
#include "rtos/EspStatus.hpp"

struct I2cBus {};
struct I2cDevice {
    uint16_t addr = 0U;
    uint32_t speedHz = 0U;
};

namespace {

using namespace app::board;
using fw::Status;

constexpr ChargerPins kChargerPins{
    .i2cPort = 0, .sdaGpio = 21, .sclGpio = 22, .addr = 0x1AU, .reg02 = 0x34U, .reg03 = 0xAAU};
constexpr OledPins kOledPins{
    .i2cPort = 0, .sdaGpio = 21, .sclGpio = 22, .powerGpio = 12, .addr = 0x3CU, .rotated180 = true};

constinit I2cBus s_bus{};
constinit I2cDevice s_charger_dev{};
constinit I2cDevice s_oled_dev{};
constinit StaticSemaphore_t *s_mutex = nullptr;
constinit bool s_charger_present = true;
constinit bool s_oled_present = true;
constinit int s_fail_page_once = -1;
constinit bool s_fail_init_cmd_once = false;
constinit bool s_fail_all_data = false;
constinit esp_err_t s_data_error = ESP_ERR_TIMEOUT;
constinit unsigned s_bus_creates = 0U;
constinit unsigned s_bus_resets = 0U;
constinit unsigned s_devices_added = 0U;
constinit unsigned s_devices_removed = 0U;
constinit unsigned s_oled_tx_count = 0U;
constinit unsigned s_oled_data_pages = 0U;
constinit unsigned s_lock_acquires = 0U;
constinit unsigned s_lock_releases = 0U;
constinit unsigned s_delay_calls = 0U;
constinit unsigned s_warnings = 0U;
constinit int s_gpio12_level = -1;
constinit bool s_gpio12_configured = false;
constinit bool s_saw_init_rotated = false;
constinit bool s_saw_power_off_cmds = false;
constinit uint8_t s_last_page_cmd = 0xFFU;
constinit std::array<uint8_t, app::oled::kPages> s_panel_first_byte{};
constinit std::array<uint8_t, kSgmRegCount> s_charger_regs{};
constinit int64_t s_time_us = 1000000LL;
constinit int64_t s_cmd_delay_us = 200LL;
constinit int64_t s_data_delay_us = 3100LL;
constinit int64_t s_lock_acquire_us = 0LL;
constinit int64_t s_max_lock_hold_us = 0LL;
constinit int s_last_cmd_timeout_ms = 0;
constinit int s_last_data_timeout_ms = 0;
constinit bool s_force_lock_busy = false;
constinit bool s_fail_data_consumes_timeout = false;

/**
 * @brief 验证断言并在失败时中止
 * @param ok: 断言结果
 * @param expr: 失败表达式
 * @param line: 源行号
 */
void check(bool ok, const char *expr, int line) noexcept
{
    if (!ok) {
        std::fprintf(stderr, "OledDriverCheck:%d: FAIL %s\n", line, expr);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

/**
 * @brief 统计 W 级日志条数
 * @param hdr: 日志头
 * @param payload: 日志正文
 */
void logSink(void *, const fw::LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    static_cast<void>(payload);
    if (hdr.level == fw::LogLevel::kWarn) {
        ++s_warnings;
    }
}

/** @brief 共享总线双器件均在线：总线只建一次，按页刷屏，空闲跳过，关机断电与恢复。 */
void runSharedBothOnline() noexcept
{
    s_charger_regs[kSgmReg02] = 0xC0U;
    s_charger_regs[kSgmReg03] = 0xF8U;
    CHECK(charger::init(kChargerPins) == Status::kOk);
    CHECK(charger::applyChargeParams(kChargerPins) == Status::kOk);

    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    CHECK(panel.present() && !panel.poweredOff());
    CHECK(s_bus_creates == 1U && s_devices_added == 2U);
    CHECK(s_charger_dev.addr == 0x1AU && s_charger_dev.speedHz == 100000U);
    CHECK(s_oled_dev.addr == 0x3CU && s_oled_dev.speedHz == 400000U);
    CHECK(s_gpio12_configured && s_gpio12_level == 1 && s_delay_calls == 1U);
    CHECK(s_saw_init_rotated && s_warnings == 0U);
    CHECK(s_lock_acquires == s_lock_releases);

    ScreenBuff fb{};
    app::oled::ViewModel vm{};
    app::oled::sanitizeCopy("P2", vm.product);
    app::oled::sanitizeCopy("V3.1.0", vm.fwVer);
    app::oled::renderPage(app::OledPage::kSystemCheck, vm, fb);

    const unsigned beforeFirst = s_oled_data_pages;
    const unsigned locksBeforeFirst = s_lock_acquires;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(panel.lastDirtyPages() == 8U && panel.totalDirtyPages() == 8U);
    CHECK(s_oled_data_pages - beforeFirst == 8U);
    // 每页单独加解锁，至少 8 次独立锁周期。
    CHECK(s_lock_acquires - locksBeforeFirst == 8U);
    CHECK(s_lock_acquires == s_lock_releases);

    // 内容未变 -> 零 I2C 写出。
    const unsigned txBeforeIdle = s_oled_tx_count;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kSkipped);
    CHECK(panel.lastDirtyPages() == 0U && panel.totalDirtyPages() == 8U);
    CHECK(s_oled_tx_count == txBeforeIdle);

    // 仅改第 5 页 1 字节 -> 只写 1 页。
    fb[5][10] ^= 0xFFU;
    const unsigned pagesBeforeDelta = s_oled_data_pages;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(panel.lastDirtyPages() == 1U && panel.totalDirtyPages() == 9U);
    CHECK(s_oled_data_pages - pagesBeforeDelta == 1U);

    // 关机关闭面板并拉低 GPIO12。
    panel.powerOff();
    CHECK(panel.poweredOff() && s_saw_power_off_cmds && s_gpio12_level == 0);
    const unsigned txAfterOff = s_oled_tx_count;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kDisabled);
    CHECK(s_oled_tx_count == txAfterOff);

    // 关机超时回退恢复点亮。
    CHECK(panel.wakeFromPowerOff() == Status::kOk);
    CHECK(!panel.poweredOff() && s_gpio12_level == 1);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
}

/** @brief OLED 无应答：只报一行 W、拉低 GPIO12 并禁用显示，不影响充电芯片。 */
void runOledAbsentChargerOk() noexcept
{
    s_oled_present = false;
    s_charger_regs[kSgmReg02] = 0xC0U;
    CHECK(charger::init(kChargerPins) == Status::kOk);

    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kNotFound);
    CHECK(!panel.present() && s_gpio12_level == 0);
    CHECK(s_bus_creates == 1U && s_devices_added == 1U && s_warnings == 1U);
    CHECK(charger::isReady() && charger::applyChargeParams(kChargerPins) == Status::kOk);

    ScreenBuff fb{};
    CHECK(panel.flush(fb) == Sh1106FlushResult::kDisabled);
}

/** @brief 充电芯片离线，OLED 仍共用同一 I2C0 总线正常工作。 */
void runChargerAbsentOledOk() noexcept
{
    s_charger_present = false;
    CHECK(charger::init(kChargerPins) == Status::kNotFound);
    CHECK(!charger::isReady() && s_warnings == 1U);

    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    CHECK(panel.present() && s_bus_creates == 1U && s_devices_added == 1U && s_warnings == 1U);

    ScreenBuff fb{};
    fb[0][0] = 0xAAU;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
}

/** @brief 单页超时立即中止剩余页、复位总线，并在下次刷新仅重传未提交页（ROL1-02）。 */
void runPageErrorRetry() noexcept
{
    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    ScreenBuff fb{};
    fb[2][0] = 0x55U;

    const unsigned pagesBeforeFail = s_oled_data_pages;
    s_fail_page_once = 2;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(panel.i2cErrors() == 1U);
    // 第 0、1 页写出后第 2 页超时：立即中止，不得继续轰炸第 3..7 页，且必须触发总线复位。
    CHECK(s_oled_data_pages - pagesBeforeFail == 2U);
    CHECK(s_bus_resets == 1U);
    CHECK(s_lock_acquires == s_lock_releases);

    const unsigned pagesBeforeRetry = s_oled_data_pages;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    // 第 0、1 页已提交摘要，本次只补写剩余第 2..7 共 6 页。
    CHECK(s_oled_data_pages - pagesBeforeRetry == 6U);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kSkipped);
}

/** @brief 连续超时触发早停、总线复位与退避（零总线风暴），恢复后可重新刷屏（ROL1-02）。 */
void runTimeoutAbortResetBackoff() noexcept
{
    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    ScreenBuff fb{};
    fb[0][0] = 0xAAU;

    s_fail_all_data = true;
    const unsigned txBeforeFirst = s_oled_tx_count;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    // 第 0 页失败立即退出：仅 1 次页定位命令 + 1 次数据发送 = 2 次 TX，并复位总线 1 次。
    CHECK(s_oled_tx_count - txBeforeFirst == 2U);
    CHECK(panel.i2cErrors() == 1U && s_bus_resets == 1U);

    // 首次错误后下轮允许立即重试；第 2 次连续失败进入 2 轮退避。
    const unsigned txBeforeSecond = s_oled_tx_count;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(s_oled_tx_count - txBeforeSecond == 2U);
    CHECK(panel.i2cErrors() == 2U && s_bus_resets == 2U);

    // 退避 2 轮期间必须返回独立 kBackoff（非 kSkipped），且零 I2C 事务、零额外总线复位（R2OL2-05）。
    const unsigned txBeforeBackoff = s_oled_tx_count;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kBackoff);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kBackoff);
    CHECK(s_oled_tx_count == txBeforeBackoff && s_bus_resets == 2U);

    // 紧急路径（USB 关机终帧）：第 3 次失败进入 4 轮退避，先验证退避生效（返回 kBackoff），
    // 调用 clearBackoff() 后必须立即解除剩余退避并在当轮补刷成功（ROL2-03/R2OL2-05）。
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kBackoff);
    s_fail_all_data = false;
    panel.clearBackoff();
    const unsigned pagesBeforeUrgent = s_oled_data_pages;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(s_oled_data_pages - pagesBeforeUrgent == 8U);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kSkipped);
    // 重新制造退避并验证普通路径自动恢复流程保持不变。
    fb[0][0] ^= 0xFFU;
    s_fail_all_data = true;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kBackoff);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kBackoff);
    // 恢复后第 3 轮自动重新刷出脏页，随后恢复空闲跳过。
    s_fail_all_data = false;
    const unsigned pagesBeforeRecover = s_oled_data_pages;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(s_oled_data_pages - pagesBeforeRecover == 1U);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kSkipped);
    CHECK(s_lock_acquires == s_lock_releases);
}

/** @brief 验证单页取锁到释放统一 8 ms 预算：命令迟到缩减数据超时、命令耗尽跳过数据并复位、数据超时与锁争用（R2OL2-04）。 */
void runPageLockBudget() noexcept
{
    s_charger_regs[kSgmReg02] = 0xC0U;
    s_charger_regs[kSgmReg03] = 0xF8U;
    CHECK(charger::init(kChargerPins) == Status::kOk);

    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    ScreenBuff fb{};
    fb[0][0] = 0x11U;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);

    // 1. 命令用去 5 ms：数据段只拿到剩余 3 ms 超时。
    s_max_lock_hold_us = 0LL;
    s_cmd_delay_us = 5000LL;
    s_data_delay_us = 2000LL;
    fb[0][0] = 0x22U;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(s_last_cmd_timeout_ms == static_cast<int>(Sh1106::kPageTimeoutBudgetMs));
    CHECK(s_last_data_timeout_ms == 3);
    CHECK(s_max_lock_hold_us == 7000LL);

    // 2. 命令耗尽全部 8 ms 预算（剩余 < 1 ms）：绝不得再发数据段，释锁后触发总线复位。
    const unsigned pagesBeforeExhaust = s_oled_data_pages;
    const unsigned resetsBeforeExhaust = s_bus_resets;
    s_cmd_delay_us = 8000LL;
    s_data_delay_us = 1000LL;
    fb[1][0] = 0x33U;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(s_oled_data_pages == pagesBeforeExhaust);
    CHECK(s_bus_resets == resetsBeforeExhaust + 1U);
    CHECK(s_lock_acquires == s_lock_releases);

    // 3. 命令用去 2 ms，数据替身用尽传入的 6 ms 后报超时，触发总线复位。
    panel.clearBackoff();
    s_max_lock_hold_us = 0LL;
    s_cmd_delay_us = 2000LL;
    s_fail_all_data = true;
    s_fail_data_consumes_timeout = true;
    const unsigned resetsBeforeDataTimeout = s_bus_resets;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(s_last_data_timeout_ms == 6);
    CHECK(s_max_lock_hold_us == 8000LL);
    CHECK(s_bus_resets == resetsBeforeDataTimeout + 1U);
    s_fail_all_data = false;
    s_fail_data_consumes_timeout = false;

    // 4. 锁争用：共享总线锁被占用时 SH1106 返回 kError（不误触发总线复位），Charger 0-tick 返回 kBusy。
    panel.clearBackoff();
    s_cmd_delay_us = 200LL;
    s_data_delay_us = 3100LL;
    s_force_lock_busy = true;
    const unsigned resetsBeforeBusy = s_bus_resets;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(s_bus_resets == resetsBeforeBusy);
    ChargerInputStatus st{};
    CHECK(charger::refreshInputStatus(st) == Status::kBusy);
    s_force_lock_busy = false;

    // 恢复后可补刷脏页。
    panel.clearBackoff();
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(s_lock_acquires == s_lock_releases);
}

/** @brief 模拟 IDF 内部分段等待后以 INVALID_RESPONSE 返回，验证释放页锁并复位总线。 */
void runInternalTimeoutRecovery() noexcept
{
    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    ScreenBuff fb{};
    s_cmd_delay_us = 2000LL;
    s_fail_all_data = true;
    s_fail_data_consumes_timeout = true;
    s_data_error = ESP_ERR_INVALID_RESPONSE;
    const unsigned resetsBefore = s_bus_resets;
    const unsigned writtenBefore = s_oled_data_pages;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(s_last_data_timeout_ms == 6);
    CHECK(s_oled_data_pages == writtenBefore);
    CHECK(s_bus_resets == resetsBefore + 1U);
    CHECK(s_lock_acquires == s_lock_releases);

    s_fail_all_data = false;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
}

/** @brief 模拟部分字节到达面板后报错，目标恢复旧内容时仅重写失败的页。 */
void runPartialPageRevert() noexcept
{
    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    constexpr uint8_t kPage = 2U;
    ScreenBuff fb{};
    fb[kPage][0] = 0xAAU;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(s_panel_first_byte[kPage] == 0xAAU);

    s_fail_page_once = kPage;
    fb[kPage][0] = 0x55U;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kError);
    CHECK(s_panel_first_byte[kPage] == 0x55U);

    const unsigned writtenBefore = s_oled_data_pages;
    fb[kPage][0] = 0xAAU;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
    CHECK(s_oled_data_pages == writtenBefore + 1U);
    CHECK(s_panel_first_byte[kPage] == 0xAAU);
    CHECK(panel.flush(fb) == Sh1106FlushResult::kSkipped);
}

/** @brief wakeFromPowerOff() 失败时必须拉低 GPIO12 保持断电态，后续重试可恢复点亮（ROL1-01）。 */
void runWakeFailRecovers() noexcept
{
    Sh1106 panel{};
    CHECK(panel.init(kOledPins) == Status::kOk);
    panel.powerOff();
    CHECK(panel.poweredOff() && s_gpio12_level == 0);

    s_fail_init_cmd_once = true;
    CHECK(panel.wakeFromPowerOff() == Status::kTimeout);
    // 关键回归点：GPIO12 不得悬停 HIGH，poweredOff_ 仍为 true，且触发了总线复位。
    CHECK(panel.poweredOff() && s_gpio12_level == 0);
    CHECK(panel.i2cErrors() == 1U && s_bus_resets == 1U);
    CHECK(s_lock_acquires == s_lock_releases);

    // 瞬态故障消失后再次唤醒成功并恢复正常刷屏。
    CHECK(panel.wakeFromPowerOff() == Status::kOk);
    CHECK(!panel.poweredOff() && s_gpio12_level == 1);
    ScreenBuff fb{};
    fb[1][1] = 0x3CU;
    CHECK(panel.flush(fb) == Sh1106FlushResult::kWritten);
}

/** @brief init() 阶段 configurePanel() 失败必须卸载已挂载设备句柄并拉低 GPIO12（ROL1-06）。 */
void runInitCfgFailCleanup() noexcept
{
    Sh1106 panel{};
    s_fail_init_cmd_once = true;
    CHECK(panel.init(kOledPins) == Status::kTimeout);
    CHECK(!panel.present() && s_gpio12_level == 0);
    CHECK(s_devices_added == 1U && s_devices_removed == 1U);
    CHECK(s_bus_resets == 1U && s_warnings == 1U);
    CHECK(panel.wakeFromPowerOff() == Status::kInvalidState);
    CHECK(s_lock_acquires == s_lock_releases);
}

} // namespace

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    CHECK(s_mutex == nullptr);
    s_mutex = storage;
    return storage;
}

int64_t esp_timer_get_time(void)
{
    return s_time_us;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t /*ticks*/)
{
    CHECK(semaphore == s_mutex);
    if (semaphore->held || s_force_lock_busy) {
        return pdFALSE;
    }
    semaphore->held = true;
    s_lock_acquire_us = s_time_us;
    ++s_lock_acquires;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore == s_mutex && semaphore->held);
    const int64_t heldUs = s_time_us - s_lock_acquire_us;
    if (heldUs > s_max_lock_hold_us) {
        s_max_lock_hold_us = heldUs;
    }
    semaphore->held = false;
    ++s_lock_releases;
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks)
{
    CHECK(ticks > 0U);
    ++s_delay_calls;
}

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    CHECK(gpio == 12);
    s_gpio12_level = static_cast<int>(level);
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    CHECK(config->pin_bit_mask == (1ULL << 12U) && config->mode == GPIO_MODE_OUTPUT);
    s_gpio12_configured = true;
    return ESP_OK;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *out)
{
    CHECK(config->i2c_port == 0 && config->sda_io_num == 21 && config->scl_io_num == 22);
    CHECK(config->flags.enable_internal_pullup == 1U);
    ++s_bus_creates;
    *out = &s_bus;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int /*timeoutMs*/)
{
    CHECK(bus == &s_bus && s_mutex->held);
    if (address == 0x1AU) {
        return s_charger_present ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    if (address == 0x3CU) {
        return s_oled_present ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out)
{
    CHECK(bus == &s_bus);
    ++s_devices_added;
    if (config->device_address == 0x1AU) {
        s_charger_dev = {config->device_address, config->scl_speed_hz};
        *out = &s_charger_dev;
        return ESP_OK;
    }
    if (config->device_address == 0x3CU) {
        s_oled_dev = {config->device_address, config->scl_speed_hz};
        *out = &s_oled_dev;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    CHECK((handle == &s_oled_dev || handle == &s_charger_dev) && s_mutex->held);
    ++s_devices_removed;
    return ESP_OK;
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus_handle)
{
    CHECK(bus_handle == &s_bus && s_mutex->held);
    ++s_bus_resets;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t count,
                              int timeoutMs)
{
    CHECK(s_mutex->held && count > 0U);
    if (dev == &s_charger_dev) {
        CHECK(count == 2U);
        s_charger_regs.at(tx[0]) = tx[1];
        return ESP_OK;
    }
    CHECK(dev == &s_oled_dev);
    // 只检查 IDF 超时传参值，替身耗时由各场景独立控制。
    CHECK(timeoutMs > 0 && static_cast<uint32_t>(timeoutMs) <= Sh1106::kPageTimeoutBudgetMs);
    ++s_oled_tx_count;
    if (tx[0] == 0x00U) {
        if ((count == 4U) && ((tx[1] & 0xF8U) == 0xB0U)) {
            CHECK(tx[2] == 0x02U && tx[3] == 0x10U); // SH1106 列偏移 2
            s_last_page_cmd = static_cast<uint8_t>(tx[1] & 0x07U);
            s_last_cmd_timeout_ms = timeoutMs;
            s_time_us += s_cmd_delay_us;
        }
        if (count >= 20U) {
            if (s_fail_init_cmd_once) {
                s_fail_init_cmd_once = false;
                return ESP_ERR_TIMEOUT;
            }
            bool sawA0 = false;
            bool sawC0 = false;
            for (size_t i = 1U; i < count; ++i) {
                sawA0 = sawA0 || (tx[i] == 0xA0U);
                sawC0 = sawC0 || (tx[i] == 0xC0U);
            }
            s_saw_init_rotated = sawA0 && sawC0;
        }
        if ((count == 4U) && (tx[1] == 0xAEU) && (tx[2] == 0xADU) && (tx[3] == 0x8AU)) {
            s_saw_power_off_cmds = true;
        }
        return ESP_OK;
    }
    if (tx[0] == 0x40U) {
        CHECK(count == 129U);
        s_last_data_timeout_ms = timeoutMs;
        if (s_fail_all_data) {
            if (s_fail_data_consumes_timeout) {
                s_time_us += static_cast<int64_t>(timeoutMs) * 1000LL;
            }
            return s_data_error;
        }
        s_panel_first_byte.at(s_last_page_cmd) = tx[1];
        if ((s_fail_page_once >= 0) && (static_cast<int>(s_last_page_cmd) == s_fail_page_once)) {
            s_fail_page_once = -1;
            return ESP_ERR_TIMEOUT;
        }
        s_time_us += s_data_delay_us;
        ++s_oled_data_pages;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx,
                                      size_t txCount, uint8_t *rx, size_t rxCount,
                                      int /*timeoutMs*/)
{
    CHECK(dev == &s_charger_dev && s_mutex->held && txCount == 1U && rxCount == 1U);
    *rx = s_charger_regs.at(tx[0]);
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT) {
        return "ESP_ERR_TIMEOUT";
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return "ESP_ERR_NOT_FOUND";
    }
    return "ESP_OK";
}

namespace rtos {

uint32_t nowMs() noexcept
{
    return static_cast<uint32_t>(s_time_us / 1000LL);
}

fw::Status fromEspErr(esp_err_t err) noexcept
{
    if (err == ESP_OK) {
        return fw::Status::kOk;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return fw::Status::kTimeout;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return fw::Status::kNotFound;
    }
    return fw::Status::kIoError;
}

} // namespace rtos

/**
 * @brief 执行指定的共享 I2C / SH1106 驱动主机测试场景
 * @param argc: 参数个数
 * @param argv: 场景名
 * @retval 0 场景通过
 */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::string_view scenario{argv[1]};
    CHECK(fw::isOk(fw::logInstallDirect(&logSink, nullptr, []() noexcept { return 0U; })));
    if (scenario == "shared_both_online") {
        runSharedBothOnline();
    } else if (scenario == "oled_absent_charger_ok") {
        runOledAbsentChargerOk();
    } else if (scenario == "charger_absent_oled_ok") {
        runChargerAbsentOledOk();
    } else if (scenario == "page_error_retry") {
        runPageErrorRetry();
    } else if (scenario == "timeout_abort_reset_backoff") {
        runTimeoutAbortResetBackoff();
    } else if (scenario == "page_lock_budget") {
        runPageLockBudget();
    } else if (scenario == "internal_timeout_recovery") {
        runInternalTimeoutRecovery();
    } else if (scenario == "partial_page_revert") {
        runPartialPageRevert();
    } else if (scenario == "wake_fail_recovers") {
        runWakeFailRecovers();
    } else if (scenario == "init_cfg_fail_cleanup") {
        runInitCfgFailCleanup();
    } else {
        std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
