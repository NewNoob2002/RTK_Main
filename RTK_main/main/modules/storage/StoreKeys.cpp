// 通用持久化键的 NVS 读写（ADR-0010 §2.10）与 StorageModule::load_keys/save_keys。
// 值以小端字节序列交给 fw::StoreBinding，与固件内其它持久化编码一致。

#include "modules/storage/StoreKeys.hpp"

#include <array>
#include <cstring>
#include <string_view>

#include "def/StorageDef.hpp"
#include "modules/storage/StorageModule.hpp"

/* NvsStore.h（冻结）没有整数键 API，这里直接使用 nvs.h。
 * 主机替身头不带 C++ 链接守卫，因此显式声明 C 链接；IDF 的 nvs.h 自带 extern "C"，嵌套无害。 */
extern "C" {
#include "nvs.h"
}

namespace app::store {

namespace {

constexpr size_t kMaxKeyWidth = 4U;   // 支持的最高键宽（u32）
constexpr uint32_t kByteMask = 0xFFU; // 按字节拆整数时使用的掩码
constexpr uint32_t kU16Mask = 0xFFFFU;
constexpr uint32_t kBitsPerByte = 8U;

using Name = std::array<char, fw::kStoreNameMax + 1U>;

/* StoreKey 的 ns/key 只是视图，NVS 需要 0 结尾的名字；超长直接拒绝。 */
bool copy_name(std::string_view in, Name &out) noexcept
{
    if (in.empty() || (in.size() > fw::kStoreNameMax)) {
        return false;
    }
    std::memcpy(out.data(), in.data(), in.size());
    out[in.size()] = '\0';
    return true;
}

bool names_of(const fw::StoreKey &key, Name &ns, Name &name) noexcept
{
    return fw::valid(key) && (value_width(key.type) != 0U) && copy_name(key.ns, ns) &&
           copy_name(key.key, name);
}

bool blob_names_of(const fw::StoreKey &key, Name &ns, Name &name) noexcept
{
    return fw::valid(key) && (key.type == fw::StoreType::kBlob) && (key.max_size > 0U) &&
           (key.max_size <= kMaxBlobKeyBytes) && copy_name(key.ns, ns) && copy_name(key.key, name);
}

} // namespace

size_t value_width(fw::StoreType type) noexcept
{
    switch (type) {
    case fw::StoreType::kU8:
        return 1U;
    case fw::StoreType::kU16:
        return 2U;
    case fw::StoreType::kU32:
        return 4U;
    default:
        return 0U;
    }
}

esp_err_t read_value(const fw::StoreKey &key, uint32_t &value) noexcept
{
    Name ns{};
    Name name{};
    if (!names_of(key, ns, name)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(ns.data(), NVS_READONLY, &handle);
    if (error != ESP_OK) {
        // 命名空间还不存在：等同于“从未保存过”。
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (key.type == fw::StoreType::kU32) {
        error = nvs_get_u32(handle, name.data(), &value);
    } else if (key.type == fw::StoreType::kU16) {
        uint16_t narrow = 0U;
        error = nvs_get_u16(handle, name.data(), &narrow);
        value = narrow;
    } else {
        uint8_t narrow = 0U;
        error = nvs_get_u8(handle, name.data(), &narrow);
        value = narrow;
    }
    nvs_close(handle);
    return error;
}

esp_err_t write_value(const fw::StoreKey &key, uint32_t value) noexcept
{
    Name ns{};
    Name name{};
    if (!names_of(key, ns, name)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(ns.data(), NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    if (key.type == fw::StoreType::kU32) {
        error = nvs_set_u32(handle, name.data(), value);
    } else if (key.type == fw::StoreType::kU16) {
        // 只保留低 16 位：键宽即 2 字节。
        error = nvs_set_u16(handle, name.data(), static_cast<uint16_t>(value & kU16Mask));
    } else {
        // 只保留低 8 位：键宽即 1 字节。
        error = nvs_set_u8(handle, name.data(), static_cast<uint8_t>(value & kByteMask));
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t read_blob(const fw::StoreKey &key, std::span<uint8_t> out, size_t &len) noexcept
{
    len = 0U;
    Name ns{};
    Name name{};
    if (!blob_names_of(key, ns, name) || (out.size() < key.max_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(ns.data(), NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t req = out.size();
    error = nvs_get_blob(handle, name.data(), out.data(), &req);
    nvs_close(handle);
    if (error == ESP_OK) {
        len = req;
    }
    return error;
}

esp_err_t write_blob(const fw::StoreKey &key, std::span<const uint8_t> data) noexcept
{
    Name ns{};
    Name name{};
    if (!blob_names_of(key, ns, name) || data.empty() || (data.size() > key.max_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(ns.data(), NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, name.data(), data.data(), data.size());
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

} // namespace app::store

namespace app {

fw::Result StorageModule::load_keys(std::span<const fw::StoreKey> keys) noexcept
{
    fw::Result overall = fw::Result::kOk;
    uint32_t loaded = 0U;
    for (const fw::StoreKey &key : keys) {
        if (key.binding.load == nullptr) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        if (key.type == fw::StoreType::kBlob) {
            if ((key.max_size == 0U) || (key.max_size > store::kMaxBlobKeyBytes)) {
                overall = fw::Result::kInvalidArg;
                continue;
            }
            std::array<uint8_t, store::kMaxBlobKeyBytes> blob{};
            size_t len = 0U;
            const esp_err_t error =
                store::read_blob(key, std::span<uint8_t>{blob}.first(key.max_size), len);
            if (error == ESP_ERR_NVS_NOT_FOUND) {
                overall = fw::Result::kNotFound;
                continue;
            }
            if (error != ESP_OK) {
                overall =
                    (error == ESP_ERR_INVALID_ARG) ? fw::Result::kInvalidArg : fw::Result::kIoError;
                continue;
            }
            const fw::Result applied =
                key.binding.load(key.binding.obj, std::span<const uint8_t>{blob}.first(len));
            if (!fw::is_ok(applied)) {
                overall = applied;
                continue;
            }
            ++loaded;
            continue;
        }
        const size_t width = store::value_width(key.type);
        uint32_t value = 0U;
        const esp_err_t error = store::read_value(key, value);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            // 从未保存过：保持模块缺省值。
            overall = fw::Result::kNotFound;
            continue;
        }
        if (error != ESP_OK) {
            overall =
                (error == ESP_ERR_INVALID_ARG) ? fw::Result::kInvalidArg : fw::Result::kIoError;
            continue;
        }
        std::array<uint8_t, store::kMaxKeyWidth> bytes{};
        for (size_t index = 0U; index < width; ++index) {
            // value 为 32 位、width ≤ 4，移位与掩码都不会越界。
            bytes[index] =
                static_cast<uint8_t>((value >> (store::kBitsPerByte * index)) & store::kByteMask);
        }
        const fw::Result applied =
            key.binding.load(key.binding.obj, std::span<const uint8_t>{bytes}.first(width));
        if (!fw::is_ok(applied)) {
            overall = applied;
            continue;
        }
        ++loaded;
    }
    trace_store(StoreTrace::kLoad, static_cast<uint32_t>(overall), loaded);
    return overall;
}

fw::Result StorageModule::save_keys(std::span<const fw::StoreKey> keys) noexcept
{
    fw::Result overall = fw::Result::kOk;
    uint32_t saved = 0U;
    for (const fw::StoreKey &key : keys) {
        if (key.binding.save == nullptr) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        if (key.type == fw::StoreType::kBlob) {
            if ((key.max_size == 0U) || (key.max_size > store::kMaxBlobKeyBytes)) {
                overall = fw::Result::kInvalidArg;
                continue;
            }
            std::array<uint8_t, store::kMaxBlobKeyBytes> blob{};
            size_t len = 0U;
            const fw::Result encoded = key.binding.save(
                key.binding.obj, std::span<uint8_t>{blob}.first(key.max_size), len);
            if (!fw::is_ok(encoded) || (len == 0U) || (len > key.max_size)) {
                overall = fw::is_ok(encoded) ? fw::Result::kInvalidArg : encoded;
                continue;
            }
            const esp_err_t error =
                store::write_blob(key, std::span<const uint8_t>{blob}.first(len));
            if (error != ESP_OK) {
                overall =
                    (error == ESP_ERR_INVALID_ARG) ? fw::Result::kInvalidArg : fw::Result::kIoError;
                continue;
            }
            ++saved;
            continue;
        }
        const size_t width = store::value_width(key.type);
        if (width == 0U) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        std::array<uint8_t, store::kMaxKeyWidth> bytes{};
        size_t len = 0U;
        const fw::Result encoded =
            key.binding.save(key.binding.obj, std::span<uint8_t>{bytes}, len);
        if (!fw::is_ok(encoded) || (len != width)) {
            overall = fw::is_ok(encoded) ? fw::Result::kInvalidArg : encoded;
            continue;
        }
        uint32_t value = 0U;
        for (size_t index = 0U; index < len; ++index) {
            // len ≤ 4，移位结果落在 32 位内。
            value |= static_cast<uint32_t>(bytes[index]) << (store::kBitsPerByte * index);
        }
        const esp_err_t error = store::write_value(key, value);
        if (error != ESP_OK) {
            overall =
                (error == ESP_ERR_INVALID_ARG) ? fw::Result::kInvalidArg : fw::Result::kIoError;
            continue;
        }
        ++saved;
    }
    trace_store(StoreTrace::kSave, static_cast<uint32_t>(overall), saved);
    return overall;
}

} // namespace app
