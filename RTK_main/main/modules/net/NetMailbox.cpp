// 控制台 AT 透传邮箱实现；纯逻辑，主机可测。
#include "modules/net/NetMailbox.hpp"

#include "cellular/AtParse.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace app {
namespace {

constexpr size_t kResultTextBytes = 32U;            // "CME ERROR <code>" 的上限
constexpr unsigned char kAsciiDel = 0x7FU;          // ASCII DEL 控制字符
constexpr std::string_view kCfunName = "+CFUN";     // 功能级别命令名
constexpr std::string_view kQiopenName = "+QIOPEN"; // 打开 socket 命令名
constexpr size_t kQiopenAccessModeIndex = 6U;       // QIOPEN 第 7 个参数 <access_mode>（0 起）

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
 * @brief 取逗号分隔的第 index 个字段（0 起，已 trim）
 * @param args: 参数串
 * @param index: 字段序号
 * @param out: 输出字段视图
 * @retval true 字段存在；false 不存在
 */
bool fieldAt(std::string_view args, size_t index, std::string_view &out) noexcept
{
    size_t start = 0U;
    for (size_t i = 0U; i < index; ++i) {
        const size_t comma = args.find(',', start);
        if (comma == std::string_view::npos) {
            return false;
        }
        start = comma + 1U;
    }
    const size_t end = args.find(',', start);
    const size_t len = (end == std::string_view::npos) ? std::string_view::npos : (end - start);
    out = trim(cellular::subview(args, start, len));
    return true;
}

/**
 * @brief 命令名之后紧跟的参数部分（跳过 '='）
 * @param rest: 去掉 "AT" 后的命令文本
 * @param nameLen: 命令名长度
 * @retval 参数视图
 */
std::string_view argsAfter(std::string_view rest, size_t nameLen) noexcept
{
    rest.remove_prefix(nameLen);
    if (!rest.empty() && (rest.front() == '=')) {
        rest.remove_prefix(1U);
    }
    return rest;
}

/**
 * @brief 判断命令名精确匹配：以 name 开头且其后不是字母数字/下划线
 * @param rest: 去掉 "AT" 后的命令文本
 * @param name: 命令名（大写）
 * @retval true 精确匹配
 */
bool isCmd(std::string_view rest, std::string_view name) noexcept
{
    if (!startsWithNoCase(rest, name)) {
        return false;
    }
    if (rest.size() == name.size()) {
        return true;
    }
    const char next = rest[name.size()];
    return !std::isalnum(static_cast<unsigned char>(next)) && (next != '_');
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
    if (text.size() < 2U) {
        return true;
    }
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20U) || (u == kAsciiDel) || (c == ';')) {
            return true; // 控制字符与命令串联
        }
    }
    if (!startsWithNoCase(text, "AT")) {
        return true;
    }
    const std::string_view rest = cellular::subview(text, 2U, std::string_view::npos);
    if (!rest.empty() && (std::toupper(static_cast<unsigned char>(rest.front())) == 'E')) {
        return true; // ATE0/ATE1：回显约定由驱动管理
    }
    if (isCmd(rest, "+IPR") || isCmd(rest, "+QISWTMD") || isCmd(rest, "+QICSGP")) {
        return true;
    }
    if (isCmd(rest, kCfunName)) {
        std::string_view mode;
        if (rest.size() > kCfunName.size() && rest[kCfunName.size()] == '?') {
            return false;
        }
        if (!fieldAt(argsAfter(rest, kCfunName.size()), 0U, mode)) {
            return true;
        }
        return (mode != "0") && (mode != "1") && (mode != "4");
    }
    // 会进入“等待数据”的命令：提示符数据阶段不经 AtEngine 的 payload 通道，禁止裸发。
    static constexpr std::array<std::string_view, 8U> kDataCmds{
        "+QISEND",   "+QISENDEX", "+QSSLSEND", "+QHTTPPOST",
        "+QHTTPPUT", "+QFTPPUT",  "+CMGS",     "+QMTPUB"};
    for (const std::string_view name : kDataCmds) {
        if (isCmd(rest, name)) {
            return true;
        }
    }
    if (isCmd(rest, kQiopenName)) {
        // AT+QIOPEN=<ctx>,<connectID>,<type>,<host>,<port>[,<local>[,<access_mode>]]：access_mode≠0 为透传/直吐
        std::string_view mode;
        if (fieldAt(argsAfter(rest, kQiopenName.size()), kQiopenAccessModeIndex, mode) &&
            !mode.empty() && (mode != "0")) {
            return true;
        }
    }
    return false;
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
    abandon_.store(false, std::memory_order_relaxed);
    state_.store(static_cast<uint8_t>(State::kPending), std::memory_order_release);
    return fw::Status::kOk;
}

bool NetMailbox::take(std::string_view &cmd, uint32_t &token) noexcept
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
    if ((rsp.token != token_) || (state() != State::kRunning)) {
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
    if (abandon_.exchange(false, std::memory_order_acq_rel)) {
        state_.store(static_cast<uint8_t>(State::kIdle), std::memory_order_release);
        return;
    }
    state_.store(static_cast<uint8_t>(State::kDone), std::memory_order_release);
}

void NetMailbox::release() noexcept
{
    uint8_t s = state_.load(std::memory_order_acquire);
    if (s == static_cast<uint8_t>(State::kDone) || s == static_cast<uint8_t>(State::kPending)) {
        if (state_.compare_exchange_strong(s, static_cast<uint8_t>(State::kIdle),
                                           std::memory_order_acq_rel)) {
            return;
        }
        s = state_.load(std::memory_order_acquire);
    }
    if (s == static_cast<uint8_t>(State::kRunning)) {
        abandon_.store(true, std::memory_order_release);
        // 标记放弃的瞬间 kNet 可能刚好写完 kDone：回读补偿，避免 kDone 无人释放。
        s = static_cast<uint8_t>(State::kDone);
        if (state_.compare_exchange_strong(s, static_cast<uint8_t>(State::kIdle),
                                           std::memory_order_acq_rel)) {
            abandon_.store(false, std::memory_order_release);
        }
    }
}

} // namespace app
