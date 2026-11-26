// AT 响应行解析辅助实现。纯函数，不分配堆。
#include "cellular/AtParse.hpp"

#include <array>
#include <limits>

namespace cellular {
namespace {

constexpr int32_t kCmeNotInserted = 10;
constexpr int32_t kCmePinRequired = 11;
constexpr int32_t kCmePukRequired = 12;
constexpr int32_t kCmePin2Required = 17;
constexpr int32_t kCmePuk2Required = 18;
constexpr int32_t kCmeIncorrectPin = 16;
constexpr int32_t kMaxRssi = 31;
constexpr int32_t kRssiUnknown = 99;
constexpr int32_t kMaxRegStat = 5;
constexpr int32_t kDecimalBase = 10;
constexpr int32_t kRegStatDenied = 3; // 3GPP TS 27.007：注册被拒
constexpr int32_t kRegStatRoaming = 5;

/**
 * @brief  去掉字符串首尾空格
 * @param  text: 输入
 * @retval 去空格后的视图
 * @note   纯函数。
 */
std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ')) {
        text.remove_prefix(1U);
    }
    while (!text.empty() && (text.back() == ' ')) {
        text.remove_suffix(1U);
    }
    return text;
}

} // namespace

bool parseInt(std::string_view text, int32_t &out) noexcept
{
    text = trim(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '-')) {
        negative = true;
        text.remove_prefix(1U);
    }
    int64_t acc = 0;
    size_t digits = 0U;
    for (const char ch : text) {
        if ((ch < '0') || (ch > '9')) {
            return false; // 整个字段必须是数字：拒绝 "12abc"、"1 2" 等尾部杂字符
        }
        acc = (acc * kDecimalBase) + (ch - '0');
        ++digits;
        if (acc > std::numeric_limits<int32_t>::max()) {
            return false;
        }
    }
    if (digits == 0U) {
        return false;
    }
    // acc 已限制 ≤ INT32_MAX，窄化无损。
    out = static_cast<int32_t>(negative ? -acc : acc);
    return true;
}

std::string_view fieldAt(std::string_view text, size_t index) noexcept
{
    size_t start = 0U;
    for (size_t i = 0U; i < index; ++i) {
        const size_t comma = text.find(',', start);
        if (comma == std::string_view::npos) {
            return {};
        }
        start = comma + 1U;
    }
    const size_t end = text.find(',', start);
    return trim(subview(text, start,
                        (end == std::string_view::npos) ? std::string_view::npos : (end - start)));
}

std::string_view argsOf(std::string_view line) noexcept
{
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return {};
    }
    return subview(line, colon + 1U);
}

bool parseCsq(std::string_view line, uint8_t &csq) noexcept
{
    if (!line.starts_with("+CSQ:")) {
        return false;
    }
    int32_t rssi = 0;
    if (!parseInt(fieldAt(argsOf(line), 0U), rssi)) {
        return false;
    }
    if (rssi == kRssiUnknown) {
        csq = kCsqUnknown;
        return true;
    }
    if ((rssi < 0) || (rssi > kMaxRssi)) {
        return false;
    }
    // rssi 已限制在 0..31，窄化无损。
    csq = static_cast<uint8_t>(rssi);
    return true;
}

/**
 * @brief 判断注册行参数是否具备查询应答形状 <n>,<stat>（r5 F3：n 与 stat 均为非空纯数字）
 * @param args: 冒号之后的参数文本
 * @retval true n、stat 两个字段都存在且为纯数字
 */
bool hasRegQueryShape(std::string_view args) noexcept
{
    int32_t tmp = 0;
    const std::string_view first = fieldAt(args, 0U);
    const std::string_view second = fieldAt(args, 1U);
    return !first.empty() && parseInt(first, tmp) && !second.empty() && (second.front() != '"') &&
           parseInt(second, tmp);
}

bool parseReg(std::string_view line, RegStatus &reg) noexcept
{
    if (!line.starts_with("+CEREG:") && !line.starts_with("+CREG:") &&
        !line.starts_with("+CGREG:")) {
        return false;
    }
    const std::string_view args = argsOf(line);
    // n、stat 均为纯数字 → 查询应答 <n>,<stat>；否则（缺失或带引号的 tac/ci）→ URC <stat>[,...]。
    const bool isQuery = hasRegQueryShape(args);
    int32_t stat = 0;
    if (!parseInt(fieldAt(args, isQuery ? 1U : 0U), stat) || (stat < 0) || (stat > kMaxRegStat)) {
        return false;
    }
    switch (stat) {
    case 0:
    case 4:
        reg = RegStatus::kNotRegistered;
        break;
    case 1:
        reg = RegStatus::kHome;
        break;
    case 2:
        reg = RegStatus::kSearching;
        break;
    case kRegStatDenied:
        reg = RegStatus::kDenied;
        break;
    case kRegStatRoaming:
    default:
        reg = RegStatus::kRoaming;
        break;
    }
    return true;
}

bool parseCpin(std::string_view line, SimStatus &sim) noexcept
{
    if (!line.starts_with("+CPIN:")) {
        return false;
    }
    const std::string_view code = trim(argsOf(line));
    if (code == "READY") {
        sim = SimStatus::kReady;
    } else if ((code == "NOT INSERTED") || (code == "NOT READY")) {
        sim = SimStatus::kAbsent;
    } else if (code.starts_with("SIM PIN") || code.starts_with("SIM PUK") ||
               code.starts_with("PH-")) {
        sim = SimStatus::kPinRequired;
    } else {
        sim = SimStatus::kError;
    }
    return true;
}

int32_t cmeCodeFromText(std::string_view text) noexcept
{
    text = trim(text);
    struct Entry final {
        std::string_view text;
        int32_t code;
    };
    static constexpr std::array<Entry, 12> kTable{{
        {"operation not allowed", 3},
        {"operation not supported", 4},
        {"SIM not inserted", kCmeNotInserted},
        {"SIM PIN required", kCmePinRequired},
        {"SIM PUK required", kCmePukRequired},
        {"SIM failure", 13},
        {"SIM busy", 14},
        {"SIM wrong", 15},
        {"incorrect password", kCmeIncorrectPin},
        {"SIM PIN2 required", kCmePin2Required},
        {"SIM PUK2 required", kCmePuk2Required},
        {"unknown", 100},
    }};
    for (const Entry &e : kTable) {
        if (text == e.text) {
            return e.code;
        }
    }
    return 0;
}

bool isRegQueryLine(std::string_view line) noexcept
{
    if (!line.starts_with("+CEREG:") && !line.starts_with("+CREG:") &&
        !line.starts_with("+CGREG:")) {
        return false;
    }
    return hasRegQueryShape(argsOf(line));
}

bool isCpinLine(std::string_view line) noexcept
{
    return line.starts_with("+CPIN:");
}

SimStatus simFromCmeCode(int32_t code) noexcept
{
    switch (code) {
    case kCmeNotInserted:
        return SimStatus::kAbsent;
    case kCmePinRequired:
    case kCmePukRequired:
    case kCmePin2Required:
    case kCmePuk2Required:
    case kCmeIncorrectPin:
        return SimStatus::kPinRequired;
    default:
        return SimStatus::kError;
    }
}

bool parseQiurc(std::string_view line, std::string_view &kind, int32_t &id) noexcept
{
    if (!line.starts_with("+QIURC:")) {
        return false;
    }
    const std::string_view args = argsOf(line);
    const size_t open = args.find('"');
    if (open == std::string_view::npos) {
        return false;
    }
    const size_t close = args.find('"', open + 1U);
    if (close == std::string_view::npos) {
        return false;
    }
    kind = subview(args, open + 1U, close - open - 1U);
    if (kind.empty()) {
        return false;
    }
    const std::string_view rest = subview(args, close + 1U);
    id = -1;
    if (!rest.empty() && (rest.front() == ',')) {
        int32_t value = 0;
        if (parseInt(fieldAt(subview(rest, 1U), 0U), value)) {
            id = value;
        }
    }
    return true;
}

} // namespace cellular
