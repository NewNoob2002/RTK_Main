// 网页可改键表与值校验（docs/web/ws-protocol.md §4）：枚举、MAC、SN、布尔。
// 纯查表/纯解析；由网页模块任务调用；不阻塞，非 ISR 安全；本文件不分配堆。
#include "web_proto/KeyTable.hpp"

namespace web {
namespace {

constexpr size_t kMacTextBytes = 17; // "AA:BB:CC:DD:EE:FF"
constexpr size_t kMacBytes = 6;
constexpr int kHexLetterBase = 10;
constexpr uint32_t kDecimalBase = 10U;

/**
 * @brief  判断字符是否为 ASCII 字母或数字。
 * @param  c: 待判断字符。
 * @retval true  '0'..'9'、'A'..'Z' 或 'a'..'z'
 * @retval false 其余字符
 */
bool isAsciiAlnum(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * @brief  把十六进制字符转为 0..15。
 * @param  c: 待转换字符。
 * @retval 0..15 合法十六进制字符
 * @retval -1    非十六进制字符
 */
int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + kHexLetterBase;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + kHexLetterBase;
    }
    return -1;
}

/**
 * @brief  按十进制严格解析 int32（不接受前导零、空格、正号与负零）。
 * @param  text:  文本值。
 * @param[out] value: 输出数值；失败时不写。
 * @retval true  合法 int32
 * @retval false 空串、格式错误、溢出或负零（"-0"）
 * @note   "-0" 虽合法 JSON 数字，但不是协议 §4 列举的取值，统一按非法处理。
 */
bool parseStrictInt(std::string_view text, int32_t &value) noexcept
{
    if (text.empty()) {
        return false;
    }
    size_t pos = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        pos = 1;
        if (pos == text.size()) {
            return false;
        }
    }
    if (text[pos] == '0' && text.size() - pos != 1) {
        return false;
    }
    constexpr uint32_t kPositiveLimit = 2147483647U;
    constexpr uint32_t kNegativeLimit = 2147483648U;
    const uint32_t limit = negative ? kNegativeLimit : kPositiveLimit;
    uint32_t magnitude = 0;
    for (; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (c < '0' || c > '9') {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (magnitude > (limit - digit) / kDecimalBase) {
            return false;
        }
        magnitude = magnitude * kDecimalBase + digit;
    }
    if (negative && magnitude == 0) {
        return false;
    }
    if (!negative) {
        value = static_cast<int32_t>(magnitude);
        return true;
    }
    if (magnitude == kNegativeLimit) {
        // kNegativeLimit = 2^31，取反会溢出 int32；写成 INT32_MIN 的等价形式。
        value = -static_cast<int32_t>(kNegativeLimit - 1U) - 1;
        return true;
    }
    value = -static_cast<int32_t>(magnitude);
    return true;
}

/**
 * @brief  校验枚举键的取值是否在允许集合内。
 * @param  allowed: 允许的整数集合。
 * @param  text:    文本值。
 * @param[out] out: 成功时提交规范整数；失败保持原内容。
 * @retval kNone     通过
 * @retval kBadValue 格式错误或不在集合内
 */
ErrorCode validateEnum(std::span<const int32_t> allowed, std::string_view text,
                       CanonicalValue &out) noexcept
{
    int32_t number = 0;
    if (!parseStrictInt(text, number)) {
        return ErrorCode::kBadValue;
    }
    for (const int32_t candidate : allowed) {
        if (candidate == number) {
            out.kind = ValueKind::kInt;
            out.number = number;
            return ErrorCode::kNone;
        }
    }
    return ErrorCode::kBadValue;
}

/**
 * @brief  校验布尔键（"0"/"1"）。
 * @param  text: 文本值。
 * @param[out] out: 成功时提交规范整数；失败保持原内容。
 * @retval kNone     通过
 * @retval kBadValue 不是 "0" 或 "1"
 */
ErrorCode validateBool(std::string_view text, CanonicalValue &out) noexcept
{
    if (text == "0" || text == "1") {
        out.kind = ValueKind::kInt;
        out.number = text == "1" ? 1 : 0;
        return ErrorCode::kNone;
    }
    return ErrorCode::kBadValue;
}

/**
 * @brief  校验并解析 MAC 文本（AA:BB:CC:DD:EE:FF，十六进制不区分大小写）。
 * @param  text: 文本值。
 * @param[out] out: 成功时提交 6 字节规范值（其余字节清零）；失败保持原内容。
 * @retval kNone     通过
 * @retval kBadValue 长度、分隔符或十六进制字符非法
 * @note   全部字节先在局部数组校验，通过后一次提交，避免失败留下半份前缀。
 */
ErrorCode validateMac(std::string_view text, CanonicalValue &out) noexcept
{
    if (text.size() != kMacTextBytes) {
        return ErrorCode::kBadValue;
    }
    std::array<uint8_t, kMacBytes> bytes{};
    for (size_t i = 0; i < kMacBytes; ++i) {
        const size_t base = i * 3;
        const int highValue = hexValue(text[base]);
        const int lowValue = hexValue(text[base + 1]);
        if (highValue < 0 || lowValue < 0) {
            return ErrorCode::kBadValue;
        }
        if (i + 1 < kMacBytes && text[base + 2] != ':') {
            return ErrorCode::kBadValue;
        }
        // 两个半字节已保证 0..15，合并后 0..255，窄化到 uint8_t 安全。
        const auto high = static_cast<uint32_t>(highValue);
        const auto low = static_cast<uint32_t>(lowValue);
        bytes[i] = static_cast<uint8_t>((high << 4U) | low);
    }
    out.bytes.fill(0);
    for (size_t i = 0; i < kMacBytes; ++i) {
        out.bytes[i] = bytes[i];
    }
    out.kind = ValueKind::kBytes;
    out.size = static_cast<uint8_t>(kMacBytes);
    return ErrorCode::kNone;
}

/**
 * @brief  校验 SN 文本（1–16 个 `[0-9A-Za-z-]`）。
 * @param  text: 文本值。
 * @param[out] out: 成功时提交字符字节串（size 之后清零）；失败保持原内容。
 * @retval kNone     通过
 * @retval kBadValue 为空、超长或含非法字符
 * @note   先整串校验再提交，避免失败留下半份前缀。
 */
ErrorCode validateSn(std::string_view text, CanonicalValue &out) noexcept
{
    if (text.empty() || text.size() > kMaxSnBytes) {
        return ErrorCode::kBadValue;
    }
    for (const char c : text) {
        if (!isAsciiAlnum(c) && c != '-') {
            return ErrorCode::kBadValue;
        }
    }
    out.bytes.fill(0);
    for (size_t i = 0; i < text.size(); ++i) {
        // 上面只放行 ASCII 字符，转 uint8_t 不改变值。
        out.bytes[i] = static_cast<uint8_t>(text[i]);
    }
    out.kind = ValueKind::kBytes;
    out.size = static_cast<uint8_t>(text.size());
    return ErrorCode::kNone;
}

/**
 * @brief  校验闭区间整数键的取值范围。
 * @param  minValue: 最小允许值（含）。
 * @param  maxValue: 最大允许值（含）。
 * @param  text:     文本值。
 * @param[out] out: 成功时提交规范整数；失败保持原内容。
 * @retval kNone     通过
 * @retval kBadValue 格式错误或越界
 */
ErrorCode validateIntRange(int32_t minValue, int32_t maxValue, std::string_view text,
                           CanonicalValue &out) noexcept
{
    int32_t number = 0;
    if (!parseStrictInt(text, number) || number < minValue || number > maxValue) {
        return ErrorCode::kBadValue;
    }
    out.kind = ValueKind::kInt;
    out.number = number;
    return ErrorCode::kNone;
}

} // namespace

const KeySpec *findKey(std::string_view name) noexcept
{
    for (const KeySpec &spec : kKeyTable) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

ErrorCode validateValue(const KeySpec &spec, std::string_view text, CanonicalValue &out) noexcept
{
    switch (spec.kind) {
    case KeyKind::kEnum:
        return validateEnum(spec.allowed, text, out);
    case KeyKind::kMac:
        return validateMac(text, out);
    case KeyKind::kSn:
        return validateSn(text, out);
    case KeyKind::kBool:
        return validateBool(text, out);
    case KeyKind::kIntRange:
        return validateIntRange(spec.minValue, spec.maxValue, text, out);
    default:
        return ErrorCode::kBadValue;
    }
}

} // namespace web
