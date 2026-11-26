// 控制台 AT 透传邮箱实现；纯逻辑，主机可测。
#include "modules/net/NetMailbox.hpp"

#include "cellular/AtParse.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace app {
namespace {

constexpr unsigned char kAsciiDel = 0x7FU; // ASCII DEL 控制字符
constexpr size_t kResultTextBytes = 32U;   // "CME ERROR <code>" 的上限

/**
 * @brief 大小写不敏感前缀判断
 * @param text: 待判断文本
 * @param prefix: 前缀，须为大写
 * @retval true text 以 prefix 开头（忽略大小写）
 */
bool startsWithNoCase(std::string_view text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0U; i < prefix.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 去掉两端空格
 * @param text: 源视图
 * @retval 去空格后的子视图
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

/**
 * @brief 追加一行到响应缓冲（结尾补 '\n'；缓冲满截断）
 * @param buf: 响应缓冲
 * @param used: 已用字节数
 * @param text: 要追加的行
 * @retval 追加后的已用字节数
 */
size_t appendLine(std::span<char> buf, size_t used, std::string_view text) noexcept
{
    if (used + 1U >= buf.size()) {
        return used;
    }
    const size_t room = buf.size() - 1U - used;
    const size_t n = std::min(text.size(), room);
    std::memcpy(buf.subspan(used).data(), text.data(), n);
    used += n;
    if ((n == text.size()) && (used + 1U < buf.size())) {
        buf[used] = '\n';
        ++used;
    }
    return used;
}

} // namespace

bool NetMailbox::forbidden(std::string_view text) noexcept
{
    text = trim(text);
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20U) || (u >= kAsciiDel) || (c == ';')) {
            return true; // 控制字符、非 ASCII 与命令串联
        }
    }
    // 白名单：只放行已批准的只读诊断命令及单参数 CFUN 故障注入（裁决 r3 I8）。
    // 整串大小写不敏感精确匹配；其余一律拒绝（含 AT+CMEE?/AT+CSCS?，它们是 AtEngine 重同步探针，
    // 调试入口不得发出同形命令，否则迟到应答可冒充探针）。
    static constexpr std::array<std::string_view, 20U> kAllowed{"AT",
                                                                "ATI",
                                                                "AT+QGMR",
                                                                "AT+GSN",
                                                                "AT+QCCID",
                                                                "AT+CSQ",
                                                                "AT+CREG?",
                                                                "AT+CEREG?",
                                                                "AT+CGREG?",
                                                                "AT+COPS?",
                                                                "AT+CPIN?",
                                                                "AT+QNWINFO",
                                                                "AT+QENG=\"SERVINGCELL\"",
                                                                "AT+CGDCONT?",
                                                                "AT+QIACT?",
                                                                "AT+QISTATE?",
                                                                "AT+CFUN?",
                                                                "AT+CFUN=0",
                                                                "AT+CFUN=1",
                                                                "AT+CFUN=4"};
    return std::none_of(kAllowed.begin(), kAllowed.end(), [text](std::string_view name) {
        return (text.size() == name.size()) && startsWithNoCase(text, name);
    });
}

std::string_view NetMailbox::replyPrefix(std::string_view text, std::span<char> out) noexcept
{
    text = trim(text);
    // "AT+XXX..." → "+XXX:"；无 '+' 的基础命令（AT/ATI）没有带前缀的应答行。
    if ((text.size() < 4U) || !startsWithNoCase(text, "AT+")) {
        return {};
    }
    text.remove_prefix(2U);
    size_t n = 0U;
    while ((n < text.size()) && (text[n] != '?') && (text[n] != '=')) {
        ++n;
    }
    if (n + 1U > out.size()) {
        return {};
    }
    for (size_t i = 0U; i < n; ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    }
    out[n] = ':';
    return {out.data(), n + 1U};
}

fw::Status NetMailbox::submit(std::string_view text) noexcept
{
    text = trim(text);
    if (text.empty() || (text.size() >= cmd_.size()) || forbidden(text)) {
        return fw::Status::kInvalidArg;
    }
    uint8_t expected = static_cast<uint8_t>(State::kIdle);
    if (!state_.compare_exchange_strong(expected, static_cast<uint8_t>(State::kFilling),
                                        std::memory_order_acq_rel)) {
        return fw::Status::kBusy;
    }
    std::memcpy(cmd_.data(), text.data(), text.size());
    cmdLen_ = text.size();
    state_.store(static_cast<uint8_t>(State::kPending), std::memory_order_release);
    return fw::Status::kOk;
}

bool NetMailbox::take(std::string_view &cmd, uint32_t &token, std::string_view &prefix) noexcept
{
    uint8_t expected = static_cast<uint8_t>(State::kPending);
    if (!state_.compare_exchange_strong(expected, static_cast<uint8_t>(State::kRunning),
                                        std::memory_order_acq_rel)) {
        return false;
    }
    ++seq_;
    if (seq_ == 0U) {
        seq_ = 1U; // 0 保留为“无请求”
    }
    token_ = seq_;
    rspLen_ = 0U;
    rsp_.fill('\0');
    ok_ = false;
    cmd = std::string_view(cmd_.data(), cmdLen_);
    prefix = replyPrefix(cmd, std::span<char>(prefix_));
    token = token_;
    return true;
}

void NetMailbox::untake() noexcept
{
    uint8_t expected = static_cast<uint8_t>(State::kRunning);
    static_cast<void>(state_.compare_exchange_strong(
        expected, static_cast<uint8_t>(State::kPending), std::memory_order_acq_rel));
}

void NetMailbox::onLine(std::string_view line) noexcept
{
    if (state() != State::kRunning) {
        return;
    }
    if (startsWithNoCase(line, "+QICSGP:")) {
        line = "+QICSGP: <redacted>"; // 凭据行不出现在响应里
    }
    rspLen_ = appendLine(std::span<char>(rsp_), rspLen_, line);
}

void NetMailbox::onDone(const cellular::AtResponse &rsp) noexcept
{
    if ((rsp.token != token_) || (state() == State::kIdle) || (state() == State::kDone)) {
        return; // 过期回调：不属于当前请求
    }
    ok_ = (rsp.result == cellular::AtResult::kOk);
    if (!ok_) {
        static constexpr std::array<std::string_view, 5U> kNames{"OK", "ERROR", "CME ERROR",
                                                                 "TIMEOUT", "ABORTED"};
        const size_t idx = static_cast<size_t>(rsp.result);
        const std::string_view name = idx < kNames.size() ? kNames[idx] : std::string_view("?");
        std::array<char, kResultTextBytes> text{};
        if (rsp.result == cellular::AtResult::kCmeError) {
            std::snprintf(text.data(), text.size(), "%.*s %d", static_cast<int>(name.size()),
                          name.data(), static_cast<int>(rsp.errorCode));
        } else {
            std::snprintf(text.data(), text.size(), "%.*s", static_cast<int>(name.size()),
                          name.data());
        }
        rspLen_ = appendLine(std::span<char>(rsp_), rspLen_,
                             std::string_view(text.data(), ::strnlen(text.data(), text.size())));
    }
    // 唯一的完成边沿：Running→Done；若控制台已放弃（Abandoned）则回收为 Idle。
    uint8_t expected = static_cast<uint8_t>(State::kRunning);
    if (!state_.compare_exchange_strong(expected, static_cast<uint8_t>(State::kDone),
                                        std::memory_order_acq_rel)) {
        state_.store(static_cast<uint8_t>(State::kIdle), std::memory_order_release);
    }
}

void NetMailbox::release() noexcept
{
    uint8_t s = state_.load(std::memory_order_acquire);
    while (true) {
        uint8_t next = 0U;
        if ((s == static_cast<uint8_t>(State::kDone)) ||
            (s == static_cast<uint8_t>(State::kPending))) {
            next = static_cast<uint8_t>(State::kIdle);
        } else if (s == static_cast<uint8_t>(State::kRunning)) {
            next = static_cast<uint8_t>(State::kAbandoned); // kNet 在命令终结时回收
        } else {
            return; // Idle/Filling/Abandoned：无事可做
        }
        if (state_.compare_exchange_weak(s, next, std::memory_order_acq_rel)) {
            return;
        }
    }
}

} // namespace app
