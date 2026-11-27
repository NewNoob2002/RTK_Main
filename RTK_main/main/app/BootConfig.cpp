// 在启动任务分配和模块加载前读取 NVS，并立即应用诊断配置。
// 仅启动任务调用，NVS 访问可阻塞，非 ISR。
#include "app/BootConfig.hpp"

#include <cinttypes>
#include <limits>

#include "AppContext.hpp"
#include "NvsStore.h"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "modules/storage/StoreKeys.hpp"

extern "C" {
#include "nvs.h"
}

#include "modules/debug/DebugKeys.hpp"
#if RTK_GNSS_ENABLED
#include "modules/gnss/GnssKeys.hpp"
#include "modules/net/NetStore.hpp"
#endif

namespace app {
namespace {

constexpr uint8_t kFailureThreshold = 3U;
constexpr fw::StoreKey kFailsKey{.ns = "boot",
                                 .key = "fails",
                                 .type = fw::StoreType::kU8,
                                 .maxSize = sizeof(uint8_t),
                                 .max = std::numeric_limits<uint8_t>::max(),
                                 .help = "consecutive incomplete boots"};

/**
 * @brief 写入启动计数并报告存储故障。
 * @param value: 计数，新启动时加一，放行后为零。
 * @note 启动任务，可阻塞，非 ISR。
 */
void writeBootFailures(uint32_t value) noexcept
{
    const auto err = store::writeValue(kFailsKey, value);
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kStore, "Could not write boot/fails=%" PRIu32 ": %s", value,
                esp_err_to_name(err));
    }
}

/**
 * @brief 读取旧计数并递增，u8 饱和后保留在最大值。
 * @param config: 写入旧计数和安全缺省标志。
 * @note 启动任务，可阻塞，非 ISR。
 */
void recordBootAttempt(BootConfig &config) noexcept
{
    uint32_t previous = 0U;
    const auto err = store::readValue(kFailsKey, previous);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        FW_LOGW(fw::TraceCat::kStore, "Could not load boot/fails: %s", esp_err_to_name(err));
    }
    // 读取按无符号 8 位类型进行，转换无损。
    config.previousFailures = static_cast<uint8_t>(previous);
    config.safeDefaults = previous >= kFailureThreshold;
    if (previous != 0U) {
        FW_LOGI(fw::TraceCat::kStore, "Previous boot did not complete (%" PRIu32 ")", previous);
    }
    if (config.safeDefaults) {
        FW_LOGW(fw::TraceCat::kStore, "Boot failed %" PRIu32 " times, using default settings",
                previous);
    }
    writeBootFailures(previous < std::numeric_limits<uint8_t>::max() ? previous + 1U : previous);
}

/**
 * @brief 读取并校验一个早期标量，记录 C1 摘要使用的加载计数。
 * @param key: NVS 目录键，范围为非负数。
 * @param value: 缺省值，读取成功时覆盖。
 * @param counts: 该组的加载计数。
 * @param valid: 可空的附加离散值检查。
 * @note 启动任务，可阻塞，非 ISR。
 */
void readEarlyValue(const fw::StoreKey &key, uint32_t &value, StoreLoadCounts &counts,
                    bool (*valid)(uint32_t) noexcept = nullptr) noexcept
{
    uint32_t saved = 0U;
    const auto err = store::readValue(key, saved);
    // 此处只处理静态目录中范围非负的无符号键。
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ++counts.defaults;
    } else if (err != ESP_OK) {
        ++counts.failed;
        FW_LOGW(fw::TraceCat::kStore, "Could not load %.*s/%.*s: %s",
                static_cast<int>(key.ns.size()), key.ns.data(), static_cast<int>(key.key.size()),
                key.key.data(), esp_err_to_name(err));
    } else if (saved < static_cast<uint32_t>(key.min) || saved > static_cast<uint32_t>(key.max) ||
               (valid != nullptr && !valid(saved))) {
        ++counts.failed;
        FW_LOGW(fw::TraceCat::kStore, "Could not load %.*s/%.*s: invalid value %" PRIu32,
                static_cast<int>(key.ns.size()), key.ns.data(), static_cast<int>(key.key.size()),
                key.key.data(), saved);
    } else {
        value = saved;
        ++counts.saved;
    }
}

/**
 * @brief 只在允许加载用户配置时读取早期目录键。
 * @param config: 保存已校验的早期数值与摘要计数。
 * @note 启动任务，可阻塞，非 ISR。
 */
void readEarlyKeys(BootConfig &config) noexcept
{
    auto &counts = config.debugCounts;
    uint32_t seconds = 0U;
    readEarlyValue(debug::kKeys[debug::kIdxMask], config.traceMask, counts);
    readEarlyValue(debug::kKeys[debug::kIdxReport], seconds, counts);
    readEarlyValue(debug::kKeys[debug::kIdxHex], config.hexMask, counts);
    readEarlyValue(debug::kKeys[debug::kIdxLogLevel], config.logLevels, counts);
    // dbg/report 的上限为 3600 秒，转换无损。
    config.reportSeconds = static_cast<uint16_t>(seconds);
    config.netEnable = net_store::loadEnable();
#if RTK_GNSS_ENABLED
    readEarlyValue(gnss_store::kConfigKeys.back(), config.gnssBaud, config.baudCounts,
                   [](uint32_t baud) noexcept {
                       return gnss_store::validUserBaud(baud) ||
                              baud == CONFIG_RTK_GNSS_DEFAULT_BAUD;
                   });
#endif
}

} // namespace

void loadEarlyConfig(AppContext &app) noexcept
{
    auto &config = app.bootConfig;
    config = BootConfig{.logLevels = fw::kLogLevelsDefault};
    const auto err = nvs_store_init();
    config.userNvsReady = err == ESP_OK;
    if (config.userNvsReady) {
        recordBootAttempt(config);
        if (!config.safeDefaults) {
            readEarlyKeys(config);
        }
    } else {
        FW_LOGE(fw::TraceCat::kStore, "store init failed: %s, using defaults",
                esp_err_to_name(err));
    }

    fw::traceSetMask(config.traceMask);
    fw::logSetHexMask(config.hexMask);
    fw::logSetLevels(config.logLevels);
#if FW_TRACE_ENABLED
    app.debug.applyBootReport(config.reportSeconds);
#endif
}

void clearBootFailures(const BootConfig &config) noexcept
{
    if (config.userNvsReady) {
        writeBootFailures(0U);
    }
}

} // namespace app
