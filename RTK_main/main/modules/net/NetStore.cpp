// net.* NVS 读写实现，见 NetStore.hpp。
#include "modules/net/NetStore.hpp"

#include <algorithm>
#include <cstring>
#include <span>

#include "fw/StoreKey.hpp"
#include "modules/storage/StoreKeys.hpp"

namespace app::net_store {
namespace {

constexpr size_t kFieldMax = cellular::LinkConfig::kFieldBytes - 1U;

inline constexpr fw::StoreKey kEnableKey{
    .ns = "net",
    .key = "en",
    .type = fw::StoreType::kU8,
    .maxSize = 1U,
    .min = 0,
    .max = 1,
    .help = "4G enable (0/1)",
};
inline constexpr fw::StoreKey kApnKey{
    .ns = "net", .key = "apn", .type = fw::StoreType::kBlob, .maxSize = 32U, .help = "4G APN"};
inline constexpr fw::StoreKey kUserKey{
    .ns = "net", .key = "usr", .type = fw::StoreType::kBlob, .maxSize = 32U, .help = "4G user"};
inline constexpr fw::StoreKey kPassKey{
    .ns = "net", .key = "pwd", .type = fw::StoreType::kBlob, .maxSize = 32U, .help = "4G password"};
static_assert(fw::valid(kEnableKey) && fw::valid(kApnKey) && fw::valid(kUserKey) &&
              fw::valid(kPassKey));

/**
 * @brief 读取 [len][chars] 文本 blob
 * @param key: 持久化键
 * @param[out] dst: 目标字段（失败时清零）
 * @retval true 读到有效值
 * @note 仅启动期单线程；可阻塞于 NVS，非 ISR。
 */
bool readText(const fw::StoreKey &key, Field &dst) noexcept
{
    std::array<uint8_t, cellular::LinkConfig::kFieldBytes> raw{};
    size_t len = 0U;
    dst.fill('\0');
    if (store::readBlob(key, raw, len) != ESP_OK || len == 0U || raw[0] > kFieldMax ||
        static_cast<size_t>(raw[0]) + 1U > len) {
        return false;
    }
    const size_t n = raw[0];
    const std::span<const uint8_t> body = std::span<const uint8_t>(raw).subspan(1U, n);
    std::transform(body.begin(), body.end(), dst.begin(),
                   [](uint8_t b) noexcept { return static_cast<char>(b); });
    if (!validText(std::string_view(dst.data(), n))) {
        dst.fill('\0');
        return false;
    }
    return true;
}

/**
 * @brief 文本键到 NVS 键的映射
 * @param key: 文本键
 * @retval 对应 NVS 键常量引用
 */
const fw::StoreKey &keyOf(TextKey key) noexcept
{
    switch (key) {
    case TextKey::kApn:
        return kApnKey;
    case TextKey::kUser:
        return kUserKey;
    case TextKey::kPass:
    default:
        return kPassKey;
    }
}

} // namespace

bool validText(std::string_view text) noexcept
{
    constexpr char kMinVisible = 0x21;
    constexpr char kMaxVisible = 0x7E;
    if (text.size() > kFieldMax) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char ch) {
        return ch >= kMinVisible && ch <= kMaxVisible && ch != '"' && ch != '\\';
    });
}

bool loadEnable() noexcept
{
    uint32_t enable = 0U;
    return store::readValue(kEnableKey, enable) == ESP_OK && enable == 1U;
}

void load(Saved &out) noexcept
{
    out = Saved{};
    uint32_t enable = 0U;
    if (store::readValue(kEnableKey, enable) == ESP_OK) {
        out.enable = (enable != 0U);
    }
    static_cast<void>(readText(kApnKey, out.apn));
    static_cast<void>(readText(kUserKey, out.user));
    static_cast<void>(readText(kPassKey, out.pass));
}

esp_err_t saveEnable(bool enable) noexcept
{
    return store::writeValue(kEnableKey, enable ? 1U : 0U);
}

esp_err_t saveText(TextKey key, std::string_view text) noexcept
{
    if (!validText(text)) {
        return ESP_ERR_INVALID_ARG;
    }
    std::array<uint8_t, cellular::LinkConfig::kFieldBytes> raw{};
    raw[0] = static_cast<uint8_t>(text.size());
    const std::span<uint8_t> body = std::span<uint8_t>(raw).subspan(1U, text.size());
    std::transform(text.begin(), text.end(), body.begin(),
                   [](char c) noexcept { return static_cast<uint8_t>(c); });
    return store::writeBlob(keyOf(key), std::span<const uint8_t>(raw.data(), text.size() + 1U));
}

} // namespace app::net_store
