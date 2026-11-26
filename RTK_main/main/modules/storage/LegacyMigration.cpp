// ADR-0022 启动迁移、逐键读回验证与完成标记。
// 仅启动任务，可阻塞，非 ISR。EEPROM 缓冲从 C6 启动分配器借用。
#include "modules/storage/LegacyMigration.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "NvsStoreBoot.h"
#include "fw/Log.hpp"
#include "modules/gnss/GnssModulePrivate.hpp"
#include "modules/storage/LegacyEeprom.hpp"
#include "modules/storage/StoreKeys.hpp"

namespace app::store {
namespace {

constexpr const char *kDone = "done";
constexpr const char *kSrcLen = "src_len";
constexpr const char *kStats = "stats";
constexpr fw::StoreKey kRateKey{.ns = "gnss",
                                .key = "rate",
                                .type = fw::StoreType::kU8,
                                .maxSize = 1U,
                                .min = 1,
                                .max = 20,
                                .help = "GNSS position output rate (Hz)"};
constexpr fw::StoreKey kOutputKey{.ns = "gnss",
                                  .key = "out",
                                  .type = fw::StoreType::kBlob,
                                  .maxSize = data::GnssOutConfig::kBlobBytes,
                                  .help = "GNSS receiver and app output configuration"};

struct Counts {
    unsigned migrated = 0U;
    unsigned skipped = 0U;
    unsigned invalid = 0U;
};

/**
 * @brief 查找任意类型的键，避免把类型不匹配当成缺失。
 * @param partition: NVS 实例标签。 @param ns: 命名空间。 @param key: 键名。
 * @retval ESP_OK 已存在；其他为缺失或后端错误。
 */
esp_err_t findStoredKey(const char *partition, const char *ns, const char *key) noexcept
{
    nvs_handle_t handle = 0U;
    auto err = nvs_open_from_partition(partition, ns, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_find_key(handle, key, nullptr);
        nvs_close(handle);
    }
    return err;
}

/**
 * @brief 直接从指定实例读回身份字段并比较原始数据。
 * @param partition: NVS 实例标签。 @param ns: 命名空间。 @param sn: true 读取字符串 SN。
 * @param expected: 完整写入字节，SN 包含末尾 NUL。
 * @retval ESP_OK 验证通过；其他为读取失败或数据不匹配。
 */
esp_err_t verifyIdentity(const char *partition, const char *ns, bool sn,
                         std::span<const uint8_t> expected) noexcept
{
    nvs_handle_t handle = 0U;
    auto err = nvs_open_from_partition(partition, ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    std::array<char, NVS_STORE_SN_MAX + 1U> raw{};
    size_t len = raw.size();
    err = sn ? nvs_get_str(handle, "sn", raw.data(), &len)
             : nvs_get_blob(handle, "pn", raw.data(), &len);
    nvs_close(handle);
    if (err == ESP_OK &&
        (len != expected.size() || std::memcmp(raw.data(), expected.data(), len) != 0)) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

/**
 * @brief 仅在身份两副本均不存在时调用 C6 双写，再逐份读回。
 * @param sn: true 迁移 SN，false 迁移 PN。 @param fields: 已校验的规范字段。
 * @param[in,out] counts: 源字段迁移统计。 @retval ESP_OK 成功或跳过；其他为失败。
 */
esp_err_t migrateIdentity(bool sn, const LegacyFields &fields, Counts &counts) noexcept
{
    if ((sn && fields.sn.front() == '\0') || (!sn && fields.pn.front() == 0U)) {
        return ESP_OK;
    }
    const char *key = sn ? "sn" : "pn";
    const auto cfg = findStoredKey(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE, key);
    const auto backup = findStoredKey(NVS_STORE_SYSTEM_PARTITION, NVS_STORE_BACKUP_NAMESPACE, key);
    if (cfg != ESP_OK && cfg != ESP_ERR_NVS_NOT_FOUND) {
        return cfg;
    }
    if (backup != ESP_OK && backup != ESP_ERR_NVS_NOT_FOUND) {
        return backup;
    }
    if (cfg == ESP_OK || backup == ESP_OK) {
        // C6 在本钩子前完成副本修复；已存在的身份优先于旧 EEPROM。
        ++counts.skipped;
        return ESP_OK;
    }
    auto err = sn ? nvs_store_set_sn(fields.sn.data()) : nvs_store_set_pn(fields.pn.data());
    if (err != ESP_OK) {
        return err;
    }
    // char 可用无符号字节查看，SN 经 Registry 验证并带有 NUL。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *snBytes = reinterpret_cast<const uint8_t *>(fields.sn.data());
    const auto expected = sn ? std::span{snBytes, std::strlen(fields.sn.data()) + 1U}
                             : std::span<const uint8_t>{fields.pn};
    err = verifyIdentity(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE, sn, expected);
    if (err == ESP_OK) {
        err = verifyIdentity(NVS_STORE_SYSTEM_PARTITION, NVS_STORE_BACKUP_NAMESPACE, sn, expected);
    }
    if (err == ESP_OK) {
        ++counts.migrated;
    }
    return err;
}

/**
 * @brief 目标键缺失时提交已校验的帧，然后独立读回并按字节比较。
 * @param key: 固件已有的定长 blob 定义。 @param value: 已编码的帧。
 * @param count: 该帧包含的有效源字段数。 @param[in,out] counts: 迁移统计。
 * @retval ESP_OK 成功或跳过；其他为写入或校验失败。
 */
esp_err_t migrateBlob(const fw::StoreKey &key, std::span<const uint8_t> value, uint8_t count,
                      Counts &counts) noexcept
{
    if (count == 0U) {
        return ESP_OK;
    }
    // 本文件调用的四个 StoreKey 的名称均为 NUL 结尾的字面量。
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    auto err = findStoredKey(NVS_STORE_CFG_PARTITION, key.ns.data(), key.key.data());
    if (err == ESP_OK) {
        counts.skipped += count;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    err = writeBlob(key, value);
    if (err != ESP_OK) {
        return err;
    }
    std::array<uint8_t, kMaxBlobKeyBytes> readback{};
    size_t len = 0U;
    err = readBlob(key, readback, len);
    if (err == ESP_OK && !std::ranges::equal(value, std::span{readback}.first(len))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        counts.migrated += count;
    }
    return err;
}

/**
 * @brief 逐个迁移身份和帧；第一个失败立即终止。
 * @param fields: 已校验的规范字段。 @param[in,out] counts: 迁移统计。
 * @retval ESP_OK 所有候选键已处理；其他为首个错误。
 */
esp_err_t migrateFields(const LegacyFields &fields, Counts &counts) noexcept
{
    std::array<uint8_t, data::GnssOutConfig::kBlobBytes> output{};
    size_t len = 0U;
    if (!fw::isOk(fields.output.encode(output, len))) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto sat = core::encodeSatTrackConfig(fields.satellite);
    auto err = migrateIdentity(true, fields, counts);
    if (err == ESP_OK) {
        err = migrateIdentity(false, fields, counts);
    }
    if (err == ESP_OK) {
        err = migrateBlob(kPhoneSatCfgKey, sat, fields.satelliteCount, counts);
    }
    if (err == ESP_OK) {
        err = migrateBlob(kOutputKey, std::span{output}.first(len), fields.outputCount, counts);
    }
    if (err == ESP_OK) {
        err = migrateBlob(kPhoneJsonSrvKey, fields.json, fields.jsonCount, counts);
    }
    if (err == ESP_OK) {
        err = migrateBlob(kPhoneTbKey, fields.thingsboard, fields.thingsboardCount, counts);
    }
    return err;
}

/**
 * @brief 提交迁移元数据并读回；计数以三个字节按迁移、跳过、非法的顺序存储。
 * @param handle: mig 命名空间的读写句柄。 @param len: 已确认的源长度。
 * @param counts: 源字段迁移统计。 @retval ESP_OK 元数据已验证；其他为失败。
 */
esp_err_t saveMigrationStats(nvs_handle_t handle, uint16_t len, const Counts &counts) noexcept
{
    // 两种长度均小于 UINT16_MAX；单项计数最多为迁移表的 21 个源字段。
    const std::array stats{static_cast<uint8_t>(counts.migrated),
                           static_cast<uint8_t>(counts.skipped),
                           static_cast<uint8_t>(counts.invalid)};
    auto err = nvs_set_u16(handle, kSrcLen, len);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, kStats, stats.data(), stats.size());
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    uint16_t savedLen = 0U;
    if (err == ESP_OK) {
        err = nvs_get_u16(handle, kSrcLen, &savedLen);
    }
    std::array<uint8_t, stats.size()> savedStats{};
    size_t actual = savedStats.size();
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, kStats, savedStats.data(), &actual);
    }
    if (err == ESP_OK && (savedLen != len || actual != stats.size() || savedStats != stats)) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

/**
 * @brief 提交完成标记；提交失败时尝试删除标记以便重试。
 * @param handle: mig 命名空间的读写句柄。 @param marker: 完成值。
 * @retval ESP_OK 提交成功；其他为写入或提交失败。
 */
esp_err_t saveDoneMarker(nvs_handle_t handle, const char *marker) noexcept
{
    const auto set = nvs_set_str(handle, kDone, marker);
    if (set != ESP_OK) {
        return set;
    }
    const auto commit = nvs_commit(handle);
    if (commit == ESP_OK) {
        return ESP_OK;
    }
    auto removal = nvs_erase_key(handle, kDone);
    if (removal == ESP_OK) {
        removal = nvs_commit(handle);
    }
    if (removal != ESP_OK && removal != ESP_ERR_NVS_NOT_FOUND) {
        FW_LOGE(fw::TraceCat::kStore, "Legacy migration marker rollback failed: %s",
                esp_err_to_name(removal));
    }
    return commit;
}

/**
 * @brief 先验证并保存元数据，最后写入完成标记。
 * @param marker: 完成值 none/bad_len/v1。 @param len: 成功迁移的源长度。
 * @param counts: v1 的统计；其他状态传 nullptr。 @retval ESP_OK 提交成功；其他为失败。
 */
esp_err_t finishMigration(const char *marker, uint16_t len, const Counts *counts) noexcept
{
    nvs_handle_t handle = 0U;
    auto err = nvs_store_open_cfg(NVS_STORE_MIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (counts != nullptr) {
        err = saveMigrationStats(handle, len, *counts);
    }
    if (err == ESP_OK) {
        err = saveDoneMarker(handle, marker);
    }
    nvs_close(handle);
    return err;
}

/**
 * @brief 使用 C6 的临时缓冲读取源、计算规范值并写入目标键。
 * @param buffer: 启动分配器借出的缓冲。 @param capacity: 缓冲容量。
 * @retval ESP_OK 已完成；其他为读取、分配、验证或写入错误。
 */
esp_err_t migrateWithScratch(uint8_t *buffer, size_t capacity) noexcept
{
    if (buffer == nullptr || capacity < NVS_STORE_EEPROM_MAX) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = capacity;
    const auto err = nvs_store_read_legacy_eeprom(buffer, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (!legacyEepromLength(len)) {
        return ESP_ERR_INVALID_SIZE;
    }
#if defined(CONFIG_RTK_GNSS_RATE_HZ)
    constexpr uint8_t kDefaultRate = CONFIG_RTK_GNSS_RATE_HZ;
#else
    constexpr uint8_t kDefaultRate = 1U;
#endif
    uint32_t savedRate = 0U;
    const auto rateResult = readValue(kRateKey, savedRate);
    if (rateResult != ESP_OK && rateResult != ESP_ERR_NVS_NOT_FOUND &&
        rateResult != ESP_ERR_NVS_TYPE_MISMATCH) {
        return rateResult;
    }
    // kRateKey 是 u8，readValue 成功时缩窄无损。
    const auto rate = static_cast<uint8_t>(savedRate);
    const auto fields =
        decodeLegacyEeprom({buffer, len}, detail::isValidRateHz(rate) ? rate : kDefaultRate);
    if (!fields.has_value()) {
        return ESP_ERR_INVALID_ARG;
    }
    Counts counts{.skipped = fields->skipped, .invalid = fields->invalid};
    auto migration = migrateFields(*fields, counts);
    if (migration == ESP_OK) {
        // 已确认 len 为 2280 或 2560，转换无损。
        migration = finishMigration("v1", static_cast<uint16_t>(len), &counts);
    }
    if (migration == ESP_OK) {
        FW_LOGI(fw::TraceCat::kStore, "Legacy config migrated: %u fields, %u skipped, %u invalid",
                counts.migrated, counts.skipped, counts.invalid);
    }
    return migration;
}

/**
 * @brief 先读完成标记与源长度；仅需迁移时借用临时 EEPROM 缓冲。
 * @param withScratch: 可空的 C6 缓冲提供者。 @retval ESP_OK 成功或跳过；其他为失败。
 */
esp_err_t runMigration(nvs_store_boot_scratch_fn withScratch) noexcept
{
    const auto done = findStoredKey(NVS_STORE_CFG_PARTITION, NVS_STORE_MIG_NAMESPACE, kDone);
    if (done != ESP_ERR_NVS_NOT_FOUND) {
        return done;
    }
    size_t len = 0U;
    const auto source = nvs_store_read_legacy_eeprom(nullptr, &len);
    if (source == ESP_ERR_NVS_NOT_FOUND) {
        return finishMigration("none", 0U, nullptr);
    }
    if (source != ESP_OK) {
        return source;
    }
    if (!legacyEepromLength(len)) {
        FW_LOGW(fw::TraceCat::kStore, "Legacy EEPROM blob has unsupported length: %zu", len);
        return finishMigration("bad_len", 0U, nullptr);
    }
    return withScratch != nullptr ? withScratch(&migrateWithScratch) : ESP_ERR_NO_MEM;
}

} // namespace

void migrateLegacyEeprom(nvs_store_boot_scratch_fn withScratch) noexcept
{
    const auto error = runMigration(withScratch);
    if (error != ESP_OK) {
        FW_LOGW(fw::TraceCat::kStore, "Legacy config migration failed, will retry: %s",
                esp_err_to_name(error));
    }
}

} // namespace app::store
