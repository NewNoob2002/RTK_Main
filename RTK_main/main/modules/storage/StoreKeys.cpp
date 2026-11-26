// 通用持久化键的 NVS 读写（ADR-0010 §2.10）与 StorageModule::loadKeys/saveKeys。
// 值以小端字节序列交给 fw::StoreBinding，与固件内其它持久化编码一致。

#include "modules/storage/StoreKeys.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "def/StorageDef.hpp"
#include "fw/Log.hpp"
#include "modules/storage/StorageModule.hpp"
#include "modules/storage/StoreWriteGate.hpp"

namespace app::store {

namespace {

constexpr size_t kMaxKeyWidth = 4U;   // 支持的最高键宽（u32）
constexpr uint32_t kByteMask = 0xFFU; // 按字节拆整数时使用的掩码
constexpr uint32_t kU16Mask = 0xFFFFU;
constexpr uint32_t kBitsPerByte = 8U;

using Name = std::array<char, fw::kStoreNameMax + 1U>;

/**
 * @brief 为 NVS 名称生成零结尾副本
 * @param in: 输入字节视图
 * @param out: 输出缓冲或结果
 * @retval true 名称有效且已复制
 * @retval false 名称空或过长，输出不变
 * @note 纯计算，不分配、不阻塞，线程/ISR 安全。
 * @note StoreKey 的 ns/key 只是视图，NVS 需要 0 结尾的名字；超长直接拒绝。
 */
bool copyName(std::string_view in, Name &out) noexcept
{
    if (in.empty() || (in.size() > fw::kStoreNameMax)) {
        return false;
    }
    std::memcpy(out.data(), in.data(), in.size());
    out[in.size()] = '\0';
    return true;
}

/**
 * @brief 校验整数键并复制命名空间与键名
 * @param key: 持久化键定义
 * @param ns: 命名空间输出
 * @param name: 键名输出
 * @retval true 整数键与名称均有效
 * @retval false 任一校验失败；输出可能部分更新
 * @note 纯计算，不分配、不阻塞，线程/ISR 安全。
 */
bool namesOf(const fw::StoreKey &key, Name &ns, Name &name) noexcept
{
    return fw::valid(key) && (valueWidth(key.type) != 0U) && copyName(key.ns, ns) &&
           copyName(key.key, name);
}

/**
 * @brief 校验 Blob 键容量并复制名称
 * @param key: 持久化键定义
 * @param ns: 命名空间输出
 * @param name: 键名输出
 * @retval true Blob 键与名称均有效
 * @retval false 任一校验失败；输出可能部分更新
 * @note 纯计算，不分配、不阻塞，线程/ISR 安全。
 */
bool blobNamesOf(const fw::StoreKey &key, Name &ns, Name &name) noexcept
{
    return fw::valid(key) && (key.type == fw::StoreType::kBlob) && (key.maxSize > 0U) &&
           (key.maxSize <= kMaxBlobKeyBytes) && copyName(key.ns, ns) && copyName(key.key, name);
}

} // namespace

size_t valueWidth(fw::StoreType type) noexcept
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

esp_err_t readValue(const fw::StoreKey &key, uint32_t &value) noexcept
{
    Name ns{};
    Name name{};
    if (!namesOf(key, ns, name)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_store_open_cfg(ns.data(), NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
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

esp_err_t writeValue(const fw::StoreKey &key, uint32_t value) noexcept
{
    const WriteGuard guard;
    if (!guard.active()) {
        return ESP_ERR_INVALID_STATE;
    }
    Name ns{};
    Name name{};
    if (!namesOf(key, ns, name)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_store_open_cfg(ns.data(), NVS_READWRITE, &handle);
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

esp_err_t readBlob(const fw::StoreKey &key, std::span<uint8_t> out, size_t &len) noexcept
{
    len = 0U;
    Name ns{};
    Name name{};
    if (!blobNamesOf(key, ns, name) || (out.size() < key.maxSize)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key.ns == kPhonePnKey.ns && key.key == kPhonePnKey.key) {
        if (out.size() < NVS_STORE_PN_BYTES) {
            return ESP_ERR_INVALID_ARG;
        }
        const auto error = nvs_store_get_pn(out.data());
        if (error == ESP_OK) {
            len = NVS_STORE_PN_BYTES;
        }
        return error;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_store_open_cfg(ns.data(), NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    size_t req = out.size();
    error = nvs_get_blob(handle, name.data(), out.data(), &req);
    nvs_close(handle);
    if (error == ESP_OK) {
        len = req;
    }
    return error;
}

esp_err_t writeBlob(const fw::StoreKey &key, std::span<const uint8_t> data) noexcept
{
    const WriteGuard guard;
    if (!guard.active()) {
        return ESP_ERR_INVALID_STATE;
    }
    Name ns{};
    Name name{};
    if (!blobNamesOf(key, ns, name) || data.empty() || (data.size() > key.maxSize)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key.ns == kPhonePnKey.ns && key.key == kPhonePnKey.key) {
        return data.size() == NVS_STORE_PN_BYTES ? nvs_store_set_pn(data.data())
                                                 : ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_store_open_cfg(ns.data(), NVS_READWRITE, &handle);
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

void formatKey(std::span<char, kKeyLineBytes> out, const fw::StoreKey &key,
               StoreValueName valueName, bool defaultsOnly) noexcept
{
    constexpr size_t kValueTextBytes = 24U;
    constexpr int kNameWidth = 14;
    std::array<char, kValueTextBytes> text{};
    esp_err_t error = ESP_OK;
    if (defaultsOnly) {
        error = ESP_ERR_NVS_NOT_FOUND;
    } else if (key.type == fw::StoreType::kBlob) {
        std::array<uint8_t, kMaxBlobKeyBytes> blob{};
        size_t len = 0U;
        error = readBlob(key, blob, len);
        if (error == ESP_OK) {
            (void)std::snprintf(text.data(), text.size(), "blob(%zuB)", len);
        }
    } else {
        uint32_t value = 0U;
        error = readValue(key, value);
        if (error == ESP_OK) {
            const char *name = (valueName != nullptr) ? valueName(key.key, value) : nullptr;
            if (name == nullptr) {
                (void)std::snprintf(text.data(), text.size(), "%" PRIu32, value);
            } else {
                (void)std::snprintf(text.data(), text.size(), "%" PRIu32 " (%s)", value, name);
            }
        }
    }
    if (error != ESP_OK) {
        (void)std::snprintf(text.data(), text.size(), "%s",
                            error == ESP_ERR_NVS_NOT_FOUND ? "default" : "error");
    }

    // 精度受输出缓冲长度约束，转换为 int 安全。
    constexpr size_t kPrecisionMax = kKeyLineBytes - 1U;
    const int keyLen = static_cast<int>(std::min(key.key.size(), kPrecisionMax));
    const int helpLen = static_cast<int>(std::min(key.help.size(), kPrecisionMax));
    (void)std::snprintf(out.data(), out.size(), "  %-*.*s = %-12s  %.*s", kNameWidth, keyLen,
                        key.key.empty() ? "" : key.key.data(), text.data(), helpLen,
                        key.help.empty() ? "" : key.help.data());
}

} // namespace app::store

namespace app {

/**
 * @brief 把 NVS 错误码映射成框架错误码
 * @param error: NVS 返回码
 * @retval kInvalidArg 参数错误
 * @retval kIoError 其他传入错误码
 */
fw::Result mapStoreError(esp_err_t error) noexcept
{
    return (error == ESP_ERR_INVALID_ARG) ? fw::Result::kInvalidArg : fw::Result::kIoError;
}

namespace {

enum class LoadOutcome : uint8_t { kSaved, kDefault, kFailed };

/**
 * @brief 记录一个加载失败的键。
 * @param key: 键定义。
 * @param error: NVS 或绑定错误名称。
 * @note 仅启动任务，输出可阻塞，非 ISR。
 */
void warnLoadKey(const fw::StoreKey &key, const char *error) noexcept
{
    // NVS 名字上限 15 字节；无效键也只记录有界前缀。
    const int nsLen = static_cast<int>(std::min(key.ns.size(), fw::kStoreNameMax));
    const int keyLen = static_cast<int>(std::min(key.key.size(), fw::kStoreNameMax));
    FW_LOGW(fw::TraceCat::kStore, "Could not load %.*s/%.*s: %s", nsLen,
            key.ns.empty() ? "" : key.ns.data(), keyLen, key.key.empty() ? "" : key.key.data(),
            error);
}

/**
 * @brief 处理单键读取结果，成功时把字节交给绑定。
 * @param key: 键定义。
 * @param error: 原始 NVS 读取结果。
 * @param bytes: 成功时读取的字节。
 * @retval 保存值已应用、键缺失或加载失败。
 * @note 仅启动任务，输出可阻塞，非 ISR。
 */
LoadOutcome applyStoredKey(const fw::StoreKey &key, esp_err_t error,
                           std::span<const uint8_t> bytes) noexcept
{
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return LoadOutcome::kDefault;
    }
    if (error != ESP_OK) {
        warnLoadKey(key, esp_err_to_name(error));
        return LoadOutcome::kFailed;
    }
    const auto applied = key.binding.load(key.binding.obj, bytes);
    if (!fw::isOk(applied)) {
        warnLoadKey(key, fw::toString(applied));
        return LoadOutcome::kFailed;
    }
    return LoadOutcome::kSaved;
}

/**
 * @brief 加载一个 blob 键并交给绑定。
 * @param key: 键定义，binding.load 已验证。
 * @retval 保存值已应用、键缺失或加载失败。
 * @note 仅启动任务，可阻塞，非 ISR。
 */
LoadOutcome loadBlobKey(const fw::StoreKey &key) noexcept
{
    std::array<uint8_t, store::kMaxBlobKeyBytes> blob{};
    size_t len = 0U;
    const esp_err_t error = store::readBlob(key, blob, len);
    return applyStoredKey(key, error, std::span{blob}.first(len));
}

/**
 * @brief 读取标量键并以小端字节序交给绑定。
 * @param key: 键定义，binding.load 已验证。
 * @retval 保存值已应用、键缺失或加载失败。
 * @note 仅启动任务，可阻塞，非 ISR。
 */
LoadOutcome loadValueKey(const fw::StoreKey &key) noexcept
{
    const size_t width = store::valueWidth(key.type);
    uint32_t value = 0U;
    const esp_err_t error = store::readValue(key, value);
    std::array<uint8_t, store::kMaxKeyWidth> bytes{};
    for (size_t index = 0U; index < width; ++index) {
        // value 为 32 位、width ≤ 4，移位与掩码都不会越界。
        bytes[index] =
            static_cast<uint8_t>((value >> (store::kBitsPerByte * index)) & store::kByteMask);
    }
    return applyStoredKey(key, error, std::span{bytes}.first(width));
}

} // namespace

/**
 * @brief 让模块编码一个 blob 键并写回 NVS
 * @param key: 键定义（binding.save 已由调用方确认非空）
 * @retval kOk: 已保存；其他: 失败类型
 * @note 仅 storage 任务调用；栈上缓冲，不分配堆。
 */
fw::Result saveBlobKey(const fw::StoreKey &key) noexcept
{
    if ((key.maxSize == 0U) || (key.maxSize > store::kMaxBlobKeyBytes)) {
        return fw::Result::kInvalidArg;
    }
    std::array<uint8_t, store::kMaxBlobKeyBytes> blob{};
    size_t len = 0U;
    const fw::Result encoded =
        key.binding.save(key.binding.obj, std::span<uint8_t>{blob}.first(key.maxSize), len);
    if (!fw::isOk(encoded) || (len == 0U) || (len > key.maxSize)) {
        return fw::isOk(encoded) ? fw::Result::kInvalidArg : encoded;
    }
    const esp_err_t error = store::writeBlob(key, std::span<const uint8_t>{blob}.first(len));
    return (error == ESP_OK) ? fw::Result::kOk : mapStoreError(error);
}

/**
 * @brief 让模块编码一个标量键（宽度由键类型决定）并写回 NVS
 * @param key: 键定义（binding.save 已由调用方确认非空）
 * @retval kOk: 已保存；其他: 失败类型
 * @note 仅 storage 任务调用；不分配堆。
 */
fw::Result saveValueKey(const fw::StoreKey &key) noexcept
{
    const size_t width = store::valueWidth(key.type);
    if (width == 0U) {
        return fw::Result::kInvalidArg;
    }
    std::array<uint8_t, store::kMaxKeyWidth> bytes{};
    size_t len = 0U;
    const fw::Result encoded = key.binding.save(key.binding.obj, std::span<uint8_t>{bytes}, len);
    if (!fw::isOk(encoded) || (len != width)) {
        return fw::isOk(encoded) ? fw::Result::kInvalidArg : encoded;
    }
    uint32_t value = 0U;
    for (size_t index = 0U; index < len; ++index) {
        // len ≤ 4，移位结果落在 32 位内。
        value |= static_cast<uint32_t>(bytes[index]) << (store::kBitsPerByte * index);
    }
    const esp_err_t error = store::writeValue(key, value);
    return (error == ESP_OK) ? fw::Result::kOk : mapStoreError(error);
}

StoreLoadCounts StorageModule::loadKeys(std::span<const fw::StoreKey> keys) noexcept
{
    StoreLoadCounts counts{};
    for (const fw::StoreKey &key : keys) {
        if (key.binding.load == nullptr) {
            warnLoadKey(key, fw::toString(fw::Result::kInvalidArg));
            ++counts.failed;
            continue;
        }
        const LoadOutcome outcome =
            (key.type == fw::StoreType::kBlob) ? loadBlobKey(key) : loadValueKey(key);
        switch (outcome) {
        case LoadOutcome::kSaved:
            ++counts.saved;
            break;
        case LoadOutcome::kDefault:
            ++counts.defaults;
            break;
        case LoadOutcome::kFailed:
            ++counts.failed;
            break;
        default:
            break;
        }
    }
    const auto result = counts.failed == 0U ? fw::Result::kOk : fw::Result::kIoError;
    traceStore(StoreTrace::kLoad, static_cast<uint32_t>(result), counts.saved);
    return counts;
}

fw::Result StorageModule::saveKeys(std::span<const fw::StoreKey> keys) noexcept
{
    fw::Result overall = fw::Result::kOk;
    uint32_t saved = 0U;
    for (const fw::StoreKey &key : keys) {
        if (key.binding.save == nullptr) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        const fw::Result applied =
            (key.type == fw::StoreType::kBlob) ? saveBlobKey(key) : saveValueKey(key);
        if (!fw::isOk(applied)) {
            overall = applied;
            continue;
        }
        ++saved;
    }
    traceStore(StoreTrace::kSave, static_cast<uint32_t>(overall), saved);
    return overall;
}

} // namespace app
