// 控制台参数解析辅助（纯函数，无 IDF 依赖，供目标固件与 host 测试共用）。
// 无状态；任意任务调用，不阻塞、不分配堆。

#include "rtk/console/console.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace rtk::console {

namespace {

constexpr uint8_t kHexAlphaBase = 10U;
constexpr uint32_t kDecBase = 10U;
constexpr uint32_t kBitsPerNibble = 4U;
constexpr size_t kMacByteStride = 3U;
constexpr size_t kMacStrLen = (kMacLen * 2U) + (kMacLen - 1U);

bool hex_nibble(char ch, uint8_t &out) noexcept
{
    if ((ch >= '0') && (ch <= '9')) {
        out = static_cast<uint8_t>(ch - '0'); // 差值 0..9，收窄到 uint8_t 安全
        return true;
    }
    if ((ch >= 'a') && (ch <= 'f')) {
        out = static_cast<uint8_t>((ch - 'a') + kHexAlphaBase); // 结果 10..15，收窄到 uint8_t 安全
        return true;
    }
    if ((ch >= 'A') && (ch <= 'F')) {
        out = static_cast<uint8_t>((ch - 'A') + kHexAlphaBase); // 结果 10..15，收窄到 uint8_t 安全
        return true;
    }
    return false;
}

} // namespace

bool parse_u32(const char *s, uint32_t &out) noexcept
{
    if (s == nullptr) {
        return false;
    }
    std::string_view sv(s);
    if (sv.empty()) {
        return false;
    }

    if ((sv.size() >= 2U) && (sv[0] == '0') && ((sv[1] == 'x') || (sv[1] == 'X'))) {
        sv.remove_prefix(2U);
        if (sv.empty()) {
            return false;
        }
        uint32_t val = 0U;
        for (const char ch : sv) {
            uint8_t nib = 0U;
            if (!hex_nibble(ch, nib)) {
                return false;
            }
            if (val > (std::numeric_limits<uint32_t>::max() >> kBitsPerNibble)) {
                return false;
            }
            val = (val << kBitsPerNibble) | static_cast<uint32_t>(nib);
        }
        out = val;
        return true;
    }

    uint32_t val = 0U;
    for (const char ch : sv) {
        if ((ch < '0') || (ch > '9')) {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(ch - '0'); // '0'..'9' 差值在 0..9
        if (val > ((std::numeric_limits<uint32_t>::max() - digit) / kDecBase)) {
            return false;
        }
        val = (val * kDecBase) + digit;
    }
    out = val;
    return true;
}

bool parse_u8(const char *s, uint8_t &out) noexcept
{
    uint32_t val = 0U;
    if ((!parse_u32(s, val)) || (val > std::numeric_limits<uint8_t>::max())) {
        return false;
    }
    out = static_cast<uint8_t>(val); // 已校验 val <= 255，收窄安全
    return true;
}

bool parse_mac(const char *s, std::span<uint8_t, kMacLen> out) noexcept
{
    if (s == nullptr) {
        return false;
    }
    const std::string_view sv(s);
    if (sv.size() != kMacStrLen) {
        return false;
    }

    std::array<uint8_t, kMacLen> tmp{};
    for (size_t i = 0U; i < kMacLen; ++i) {
        const size_t pos = i * kMacByteStride;
        uint8_t hi = 0U;
        uint8_t lo = 0U;
        if ((!hex_nibble(sv[pos], hi)) || (!hex_nibble(sv[pos + 1U], lo))) {
            return false;
        }
        if (((i + 1U) < kMacLen) && (sv[pos + 2U] != ':')) {
            return false;
        }
        // hi, lo 均在 0..15，组合值在 0..255。
        tmp[i] = static_cast<uint8_t>((static_cast<uint32_t>(hi) << kBitsPerNibble) | static_cast<uint32_t>(lo));
    }

    std::copy(tmp.begin(), tmp.end(), out.begin());
    return true;
}

bool parse_hex(const char *s, std::span<uint8_t> out, size_t &written) noexcept
{
    if (s == nullptr) {
        return false;
    }
    const std::string_view sv(s);
    if ((sv.size() % 2U) != 0U) {
        return false;
    }
    const size_t byte_len = sv.size() / 2U;
    if (byte_len > out.size()) {
        return false;
    }

    // 先完整校验所有十六进制字符，保证失败时不修改 out 与 written。
    for (const char ch : sv) {
        uint8_t dummy = 0U;
        if (!hex_nibble(ch, dummy)) {
            return false;
        }
    }

    for (size_t i = 0U; i < byte_len; ++i) {
        uint8_t hi = 0U;
        uint8_t lo = 0U;
        static_cast<void>(hex_nibble(sv[i * 2U], hi));
        static_cast<void>(hex_nibble(sv[(i * 2U) + 1U], lo));
        // hi, lo 均在 0..15，组合值在 0..255。
        out[i] = static_cast<uint8_t>((static_cast<uint32_t>(hi) << kBitsPerNibble) | static_cast<uint32_t>(lo));
    }
    written = byte_len;
    return true;
}

} // namespace rtk::console
