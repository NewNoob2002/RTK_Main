// GNSS 控制台速率转换和连板串口的透传保护。
// 纯函数，可在主机验证，非阻塞、无堆分配。
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

namespace app::detail {

/**
 * @brief 解析完整的无符号十进制整数。
 * @param text: 输入。
 * @param out: 成功时得到数值。
 * @retval true 解析成功；false 非整数。
 */
inline bool parseGnssUnsigned(std::string_view text, uint32_t &out) noexcept
{
    if (text.empty()) {
        return false;
    }
    // from_chars 需要同一视图内的起止指针。
    const char *const begin = text.data();
    const char *const end =
        begin + text.size(); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

inline constexpr uint32_t kGnssHzScale = 1000000U; // 兼容原先接受的六位小数输入。
inline constexpr uint32_t kGnssMaxPeriod = 127U;
inline constexpr int kGnssDecimalPlaces = 6;
inline constexpr uint32_t kDecimalRadix = 10U;

/**
 * @brief 识别 1/n 及其对应的十进制输入。
 * @param text: 小于 1 Hz 的表示。
 * @retval 周期秒数；0 表示无效。
 */
inline uint32_t parseGnssPeriod(std::string_view text) noexcept
{
    uint32_t value = 0U;
    if (text.starts_with("1/")) {
        return parseGnssUnsigned(text.substr(2U), value) && value >= 2U && value <= kGnssMaxPeriod
                   ? value
                   : 0U;
    }
    if (!text.starts_with("0.")) {
        return 0U;
    }
    auto digits = text.substr(2U);
    while (!digits.empty() && digits.back() == '0') {
        digits.remove_suffix(1U);
    }
    constexpr size_t kMaxDigits = kGnssDecimalPlaces;
    if (digits.size() > kMaxDigits || !parseGnssUnsigned(digits, value) || value == 0U) {
        return 0U;
    }
    for (size_t length = digits.size(); length < kMaxDigits; ++length) {
        value *= kDecimalRadix;
    }
    const uint32_t period = (kGnssHzScale + value / 2U) / value;
    if (period < 2U || period > kGnssMaxPeriod) {
        return 0U;
    }
    return (kGnssHzScale + period / 2U) / period == value ? period : 0U;
}

/**
 * @brief 把控制台 Hz 记法转换成协议使用的 int8 间隔。
 * @param text: 输入。
 * @param interval: 成功时得到间隔。
 * @retval true 合法；false 无对应编码。
 */
inline bool parseGnssHz(std::string_view text, int8_t &interval) noexcept
{
    if (text == "off" || text == "0") {
        interval = 0;
        return true;
    }
    if (text == "1" || text == "2" || text == "5" || text == "10" || text == "20") {
        uint32_t rate = 0U;
        if (!parseGnssUnsigned(text, rate)) {
            return false;
        }
        interval = static_cast<int8_t>(rate); // 最高 20 Hz。
        return true;
    }
    const uint32_t period = parseGnssPeriod(text);
    if (period == 0U) {
        return false;
    }
    interval = static_cast<int8_t>(-static_cast<int32_t>(period)); // 周期 ≤ 127 秒。
    return true;
}

inline constexpr size_t kGnssRateTextBytes = 16U;

/**
 * @brief 显示 NVS/协议间隔的 Hz 形式。
 * @param interval: 已编码的间隔。
 * @param out: NUL 结尾的文本。
 */
inline void formatGnssHz(int8_t interval, std::span<char, kGnssRateTextBytes> out) noexcept
{
    if (interval == 0) {
        (void)std::snprintf(out.data(), out.size(), "off");
    } else if (interval == -1) {
        (void)std::snprintf(out.data(), out.size(), "1 Hz");
    } else if (interval > 0) {
        (void)std::snprintf(out.data(), out.size(), "%d Hz", static_cast<int>(interval));
    } else {
        // 先拓宽到 int，兼容协议以前保存的 -128。
        (void)std::snprintf(out.data(), out.size(), "1/%d Hz", -static_cast<int>(interval));
    }
}

/**
 * @brief 取走一个由空格或 TAB 分隔的命令词。
 * @param text: 剩余命令。
 * @retval 首词，无词时返回空视图。
 */
inline std::string_view gnssCommandWord(std::string_view &text) noexcept
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        text = {};
        return {};
    }
    text.remove_prefix(first);
    const size_t end = text.find_first_of(" \t");
    const auto word = text.substr(0U, end);
    text.remove_prefix(word.size());
    return word;
}

/**
 * @brief 用接收机 ASCII 命令的大小写规则比较词。
 * @param word: 输入词。
 * @param upper: 大写参照。
 * @retval true 相同；false 不同。
 */
inline bool gnssWordIs(std::string_view word, std::string_view upper) noexcept
{
    if (word.size() != upper.size()) {
        return false;
    }
    for (size_t i = 0U; i < word.size(); ++i) {
        const char ch = word[i];
        if ((ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch) != upper[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 判定透传是否会改动板上相连的接收机串口。
 * @param text: ASCII 命令。
 * @param port: Kconfig 选择的接收机端口。
 * @retval true 需通过 gnss baud 同步切速。
 */
inline bool changesConnectedGnssBaud(std::string_view text, uint8_t port) noexcept
{
    const auto verb = gnssCommandWord(text);
    constexpr std::string_view kPubxConfig = "$PUBX,41,";
    if (gnssWordIs(verb.substr(0U, kPubxConfig.size()), kPubxConfig)) {
        // UBX-22008968 §2.9.1：PUBX,41 的下一字段是 UART 号，命令同时设置波特率。
        const auto fields = verb.substr(kPubxConfig.size());
        const auto end = fields.find(',');
        uint32_t target = 0U;
        return end != std::string_view::npos && parseGnssUnsigned(fields.substr(0U, end), target) &&
               target == port;
    }
    if (!gnssWordIs(verb, "CONFIG") && !gnssWordIs(verb, "COM")) {
        return false;
    }
    auto target = gnssCommandWord(text);
    uint32_t baud = 0U;
    if (gnssWordIs(verb, "COM") && parseGnssUnsigned(target, baud)) {
        return true; // ComNav 省略端口，作用于当前串口。
    }
    constexpr size_t kComPrefixLength = 3U;
    if (target.size() <= kComPrefixLength ||
        !gnssWordIs(target.substr(0U, kComPrefixLength), "COM")) {
        return false;
    }
    target.remove_prefix(kComPrefixLength);
    uint32_t number = 0U;
    return parseGnssUnsigned(target, number) && number == port &&
           parseGnssUnsigned(gnssCommandWord(text), baud);
}

} // namespace app::detail
