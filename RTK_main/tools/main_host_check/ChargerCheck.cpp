// 用 SDK 边界替身运行真实 SGM41513 驱动，检查探测、寄存器验证与总线复用。
// 每个进程执行一个单线程场景；无硬件访问。
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "board/Charger.hpp"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "fw/Log.hpp"
#include "rtos/EspStatus.hpp"

struct I2cBus {};
struct I2cDevice {};

namespace {

using namespace app::board;
using fw::Status;

constexpr ChargerPins kPins{
    .i2cPort = 0, .sdaGpio = 21, .sclGpio = 22, .addr = 0x1A, .reg02 = 0x34, .reg03 = 0xAA};

constinit I2cBus s_bus{};
constinit I2cDevice s_device{};
constinit StaticSemaphore_t *s_mutex = nullptr;
constinit bool s_absent = false;
constinit uint8_t s_mismatch_reg = 0U;
constinit uint8_t s_readback_xor = 0U;
constinit uint16_t s_timeout_addr = 0U;
constinit unsigned s_lock_releases = 0U;
constinit unsigned s_bus_creates = 0U;
constinit unsigned s_added = 0U;
constinit unsigned s_probes = 0U;
constinit unsigned s_reads = 0U;
constinit unsigned s_writes = 0U;
constinit std::array<uint8_t, kSgmRegCount> s_registers{};
constinit std::string_view s_expected_warning{};
constinit unsigned s_warnings = 0U;

/**
 * @brief 在 Debug 与 Release 中均验证驱动结果
 * @param ok: 断言结果
 * @param expression: 失败表达式
 * @param line: 源行号
 */
void check(bool ok, const char *expression, int line)
{
    if (!ok) {
        std::fprintf(stderr, "ChargerCheck:%d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

/**
 * @brief 验证日志级别、内容及行数
 * @param hdr: 日志记录头
 * @param payload: 日志文本
 */
void checkWarning(void *, const fw::LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    CHECK(hdr.level == fw::LogLevel::kWarn && hdr.cat == fw::TraceCat::kSys);
    // 日志文本以字节存储，char 可读取其对象表示。
    CHECK(std::string_view(reinterpret_cast<const char *>(payload.data()), payload.size()) ==
          s_expected_warning);
    ++s_warnings;
}

/** @brief 验证离线设备不访问寄存器，手动扫描仍复用 I2C0。 */
void checkAbsent()
{
    CHECK(charger::init(kPins) == Status::kNotFound);
    CHECK(charger::init(kPins) == Status::kNotFound);
    CHECK(!charger::isReady());
    CHECK(s_bus_creates == 1U && s_probes == 1U && s_added == 0U);
    auto sample = decodeSgmReg08(kSgmReg08PgBit);
    for (unsigned i = 0U; i < 5U; ++i) {
        CHECK(charger::applyChargeParams(kPins) == Status::kInvalidState);
        CHECK(charger::refreshInputStatus(sample) == Status::kInvalidState && !sample.ok);
    }
    std::array<uint8_t, kSgmRegCount> regs{};
    CHECK(charger::dumpRegs(regs) == Status::kInvalidState);
    CHECK(s_probes == 1U && s_reads == 0U && s_writes == 0U);

    std::array<uint8_t, charger::kScanAddressCount> addresses{};
    size_t count = 0U;
    CHECK(charger::scan(addresses, count) == Status::kOk);
    CHECK(count == 1U && addresses[0] == 0x3CU);
    CHECK(s_bus_creates == 1U && s_probes == 113U && !charger::isReady());
    CHECK(s_lock_releases == s_probes);
    CHECK(charger::refreshInputStatus(sample) == Status::kInvalidState);
    CHECK(s_reads == 0U && s_writes == 0U);
}

/** @brief 验证 REG02 保留高位、仅校验低位，REG03 校验全字节。 */
void checkOnline()
{
    s_registers[kSgmReg02] = 0xC0U;
    s_registers[kSgmReg03] = 0xF8U;
    CHECK(charger::init(kPins) == Status::kOk && charger::isReady());
    CHECK(s_bus_creates == 1U && s_probes == 1U && s_added == 1U);
    const auto expected = s_expected_warning.empty() ? Status::kOk : Status::kCorrupt;
    CHECK(charger::applyChargeParams(kPins) == expected);
    CHECK(s_registers[kSgmReg02] == 0xF4U && s_registers[kSgmReg03] == 0xAAU);
    CHECK(s_writes == 2U &&
          s_reads == (s_mismatch_reg == kSgmReg02 && expected == Status::kCorrupt ? 2U : 3U));
}

/** @brief 验证启动探测超时降级且停止充电寄存器访问。 */
void checkInitTimeout()
{
    s_timeout_addr = kPins.addr;
    CHECK(charger::init(kPins) == Status::kTimeout);
    CHECK(charger::init(kPins) == Status::kNotFound);
    CHECK(!charger::isReady() && s_probes == 1U && s_added == 0U);
    CHECK(charger::applyChargeParams(kPins) == Status::kInvalidState);
    CHECK(s_reads == 0U && s_writes == 0U);
}

/** @brief 验证扫描争用不探测，超时立即退出并保留已探测前缀。 */
void checkScanErrors()
{
    CHECK(charger::init(kPins) == Status::kOk);
    std::array<uint8_t, charger::kScanAddressCount> addresses{};
    size_t count = addresses.size();

    CHECK(xSemaphoreTake(s_mutex, 0U) == pdTRUE);
    CHECK(charger::scan(addresses, count) == Status::kBusy);
    CHECK(count == 0U && s_probes == 1U && s_mutex->held);
    CHECK(xSemaphoreGive(s_mutex) == pdTRUE);

    s_timeout_addr = 0x3DU;
    CHECK(charger::scan(addresses, count) == Status::kTimeout);
    CHECK(count == 2U && addresses[0] == kPins.addr && addresses[1] == 0x3CU);
    CHECK(s_probes == 55U && !s_mutex->held);
}

} // namespace

/** @brief 获取诊断所用静态互斥。 @param storage: 调用者存储。 @retval 互斥句柄。 */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    CHECK(s_mutex == nullptr);
    s_mutex = storage;
    return storage;
}

/** @brief 尝试持锁。 @param semaphore: 互斥。 @retval pdTRUE 已取得。 @retval pdFALSE 已占用。 */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t /*ticks*/)
{
    CHECK(semaphore == s_mutex);
    if (semaphore->held) {
        return pdFALSE;
    }
    semaphore->held = true;
    return pdTRUE;
}

/** @brief 释放互斥。 @param semaphore: 互斥。 @retval pdTRUE 已释放。 */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore == s_mutex && semaphore->held);
    semaphore->held = false;
    ++s_lock_releases;
    return pdTRUE;
}

/**
 * @brief 校验 I2C0 引脚和内部上拉配置
 * @param config: 总线配置
 * @param[out] out: 模拟总线句柄
 * @retval ESP_OK 配置有效
 */
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *out)
{
    CHECK(config->i2c_port == 0 && config->sda_io_num == 21 && config->scl_io_num == 22);
    CHECK(config->flags.enable_internal_pullup == 1U);
    ++s_bus_creates;
    *out = &s_bus;
    return ESP_OK;
}

/**
 * @brief 模拟首次充电探测及地址扫描，并核对超时和锁
 * @param bus: 模拟总线句柄
 * @param address: 7 bit 地址
 * @param timeoutMs: 超时上限（ms）
 * @retval ESP_OK 有应答
 * @retval ESP_ERR_NOT_FOUND 无应答
 * @retval ESP_ERR_TIMEOUT 注入总线超时
 */
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeoutMs)
{
    CHECK(bus == &s_bus && s_mutex->held && timeoutMs == 10);
    if (s_probes == 0U) {
        CHECK(address == 0x1AU);
    } else {
        CHECK(address == 0x08U + (s_probes - 1U) && address <= 0x77U);
    }
    ++s_probes;
    if (address == s_timeout_addr) {
        return ESP_ERR_TIMEOUT;
    }
    return (address == 0x1AU && !s_absent) || address == 0x3CU ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/**
 * @brief 验证芯片应答后才装入设备，并验证总线频率
 * @param bus: 模拟总线句柄
 * @param config: 设备配置
 * @param[out] out: 模拟设备句柄
 * @retval ESP_OK 已装入
 */
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out)
{
    CHECK(bus == &s_bus && s_probes == 1U && !s_absent);
    CHECK(config->device_address == 0x1AU && config->scl_speed_hz == 100000U);
    ++s_added;
    *out = &s_device;
    return ESP_OK;
}

/**
 * @brief 模拟卸载 I2C 从设备句柄（供共享 I2cBus.cpp 链接）
 * @param handle: 待卸载设备句柄
 * @retval ESP_OK 已卸载
 */
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    CHECK(handle == &s_device && s_mutex->held);
    return ESP_OK;
}

/**
 * @brief 模拟复位共享 I2C 总线（供共享 I2cBus.cpp 链接）
 * @param bus_handle: 模拟总线句柄
 * @retval ESP_OK 已复位
 */
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus_handle)
{
    CHECK(bus_handle == &s_bus && s_mutex->held);
    return ESP_OK;
}

/**
 * @brief 记录充电芯片寄存器写入
 * @param dev: 模拟设备句柄
 * @param tx: 寄存器及写入值
 * @param count: 输入字节数
 * @param timeoutMs: 超时上限（ms）
 * @retval ESP_OK 已写入
 */
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t count,
                              int timeoutMs)
{
    CHECK(dev == &s_device && s_mutex->held && count == 2U && timeoutMs == 10);
    s_registers.at(tx[0]) = tx[1];
    ++s_writes;
    return ESP_OK;
}

/**
 * @brief 回读模拟寄存器，写入后可翻转指定寄存器中的位
 * @param dev: 模拟设备句柄
 * @param tx: 寄存器地址
 * @param txCount: 地址字节数
 * @param[out] rx: 寄存器读回值
 * @param rxCount: 输出容量
 * @param timeoutMs: 超时上限（ms）
 * @retval ESP_OK 已读出
 */
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx,
                                      size_t txCount, uint8_t *rx, size_t rxCount, int timeoutMs)
{
    CHECK(dev == &s_device && s_mutex->held && txCount == 1U && rxCount == 1U && timeoutMs == 10);
    const uint8_t value = s_registers.at(tx[0]);
    // 两个操作数均为 8 位，异或结果可安全窄化。
    *rx = s_writes != 0U && tx[0] == s_mismatch_reg ? static_cast<uint8_t>(value ^ s_readback_xor)
                                                    : value;
    ++s_reads;
    return ESP_OK;
}

/** @brief 校验日志取得超时错误名。 @param err: SDK 状态。 @retval 超时错误名。 */
const char *esp_err_to_name(esp_err_t err)
{
    CHECK(err == ESP_ERR_TIMEOUT);
    return "ESP_ERR_TIMEOUT";
}

namespace rtos {

/** @brief 验证驱动传递预期状态。 @param err: SDK 状态。 @retval kOk 模拟成功。 @retval kTimeout 模拟总线超时。 */
fw::Status fromEspErr(esp_err_t err) noexcept
{
    if (err == ESP_ERR_TIMEOUT) {
        return fw::Status::kTimeout;
    }
    CHECK(err == ESP_OK);
    return fw::Status::kOk;
}

} // namespace rtos

/**
 * @brief 执行离线或在线寄存器场景
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 场景通过
 */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::string_view mode{argv[1]};
    CHECK(fw::isOk(fw::logInstallDirect(&checkWarning, nullptr, []() noexcept { return 0U; })));
    if (mode == "absent") {
        s_absent = true;
        s_expected_warning = "charger: no ack at 0x1A, i2c path disabled";
        checkAbsent();
    } else if (mode == "init_timeout") {
        s_expected_warning = "charger: probe at 0x1A failed: ESP_ERR_TIMEOUT, i2c path disabled";
        checkInitTimeout();
    } else if (mode == "scan_errors") {
        checkScanErrors();
    } else {
        if (mode == "mismatch02") {
            s_mismatch_reg = kSgmReg02;
            s_readback_xor = 0x01U;
            s_expected_warning = "charger: REG02 readback mismatch expected=0xF4 actual=0xF5";
        } else if (mode == "mismatch03") {
            s_mismatch_reg = kSgmReg03;
            s_readback_xor = 0x80U;
            s_expected_warning = "charger: REG03 readback mismatch expected=0xAA actual=0x2A";
        } else if (mode == "readback02_high_bits") {
            s_mismatch_reg = kSgmReg02;
            s_readback_xor = 0xC0U;
        } else {
            CHECK(mode == "online");
        }
        checkOnline();
    }
    CHECK(s_warnings == (s_expected_warning.empty() ? 0U : 1U));
    return 0;
}
