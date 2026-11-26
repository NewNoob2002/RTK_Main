// ADR-0022 启动迁移、逐键读回验证与完成标记。
// 仅启动任务，可阻塞，非 ISR。EEPROM 缓冲从 C6 启动分配器借用。
#include "modules/storage/LegacyMigration.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string_view>

#include "NvsStoreBoot.h"
#include "fw/Log.hpp"
#include "settings/Registry.hpp"

namespace app::store {
namespace {

constexpr const char *kDone = "done";
constexpr const char *kSrcLen = "src_len";
constexpr const char *kStats = "stats";
constexpr size_t kV27EepromBytes = 2280U;
constexpr size_t kV31EepromBytes = NVS_STORE_EEPROM_MAX;
constexpr uint8_t kErasedByte = 0xFFU;
constexpr uint8_t kAsciiDelete = 0x7FU;

struct IdentityField {
    size_t offset;
    size_t width;
    bool sn;
};

// ADR-0022 §2.5 修订：V2.7 和 V3.1 共享 SN/PN 地址。
constexpr std::array<IdentityField, 2U> kIdentityFields{{
    {200U, NVS_STORE_SN_MAX, true},
    {216U, NVS_STORE_PN_BYTES, false},
}};
static_assert(NVS_STORE_PN_BYTES <= NVS_STORE_SN_MAX + 1U);

struct Counts {
    unsigned migrated = 0U;
    unsigned skipped = 0U;
    unsigned invalid = 0U;
};

/** @brief 检查源长度。 @param len: blob 字节数。 @retval true 支持的旧固件长度。 */
bool legacyEepromLength(size_t len) noexcept
{
    return len == kV27EepromBytes || len == kV31EepromBytes;
}

/**
 * @brief 查找任意类型的迁移完成标记。
 * @retval ESP_OK 已存在；其他为缺失或后端错误。
 */
esp_err_t findDoneMarker() noexcept
{
    nvs_handle_t handle = 0U;
    auto err = nvs_store_open_cfg(NVS_STORE_MIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_find_key(handle, kDone, nullptr);
        nvs_close(handle);
    }
    return err;
}

/**
 * @brief 提取到首个 NUL 为止；空串、全擦除或含控制字符按未设置。
 * @param bytes: 源身份字段。
 * @retval 指向源缓冲的文本视图，未设置时为空。
 */
std::string_view readText(std::span<const uint8_t> bytes) noexcept
{
    const auto text = std::span{bytes.begin(), std::ranges::find(bytes, 0U)};
    if (std::ranges::all_of(text, [](uint8_t byte) { return byte == kErasedByte; }) ||
        std::ranges::any_of(text,
                            [](uint8_t byte) { return byte < ' ' || byte == kAsciiDelete; })) {
        return {};
    }
    // char 可以读取字节缓冲的对象表示，长度已限制在字段内。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

/**
 * @brief SN 使用 factory.sn 校验，PN 检查 C6 接口要求的可打印 ASCII。
 * @param sn: true 为 SN。 @param text: 非空的旧身份文本。
 * @retval true 可迁移。
 */
bool validIdentity(bool sn, std::string_view text) noexcept
{
    if (!sn) {
        return std::ranges::all_of(text, [](char ch) { return ch >= ' ' && ch <= '~'; });
    }
    const auto &spec = *settings::keyAt(settings::KeyId::kFactorySn);
    settings::Domain domain{};
    spec.domain({}, false, domain);
    settings::CanonicalValue value{};
    return settings::validateValue(spec, domain, text, value) == settings::Error::kOk;
}

/**
 * @brief 迁移两个身份字段，首个 NVS 错误立即返回。
 * @param blob: 长度已确认的 EEPROM。 @param[out] counts: 本轮字段统计。
 * @retval ESP_OK 两字段均已处理；其他为 NVS 错误。
 */
esp_err_t migrateFields(std::span<const uint8_t> blob, Counts &counts) noexcept
{
    for (const auto &field : kIdentityFields) {
        const auto text = readText(blob.subspan(field.offset, field.width));
        if (text.empty()) {
            ++counts.skipped;
            continue;
        }
        if (!validIdentity(field.sn, text)) {
            ++counts.invalid;
            continue;
        }

        // 地址表限制了 text 长度。SN 补终止符，PN 补零到定长。
        std::array<char, NVS_STORE_SN_MAX + 1U> value{};
        std::memcpy(value.data(), text.data(), text.size());
        const size_t size = field.sn ? text.size() + 1U : field.width;
        bool written = false;
        const auto err = nvs_store_migrate_identity(field.sn, value.data(), size, &written);
        if (err != ESP_OK) {
            return err;
        }
        if (written) {
            ++counts.migrated;
        } else {
            ++counts.skipped;
        }
    }
    return ESP_OK;
}

/**
 * @brief 提交迁移元数据并读回；计数以三个字节按迁移、跳过、非法的顺序存储。
 * @param handle: mig 命名空间的读写句柄。 @param len: 已确认的源长度。
 * @param counts: 源字段迁移统计。 @retval ESP_OK 元数据已验证；其他为失败。
 */
esp_err_t saveMigrationStats(nvs_handle_t handle, uint16_t len, const Counts &counts) noexcept
{
    // 两种长度均小于 UINT16_MAX；单项计数最多为 2。
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
    Counts counts{};
    auto migration = migrateFields({buffer, len}, counts);
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
    const auto done = findDoneMarker();
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
