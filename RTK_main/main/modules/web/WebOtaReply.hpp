// OTA HTTP 的状态码及定长 JSON 编码（ADR-0019 §2.3）。
// 无 IDF 依赖；httpd 或主机测试各自独占输出缓冲，不阻塞。
#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

#include "modules/web/WebOtaTransfer.hpp"

namespace app::web_ota {

inline constexpr const char *kStatusOk = "200 OK";
inline constexpr const char *kStatusFlash = "500 Internal Server Error";

struct OtaReply {
    const char *status;
    const char *code;
    const char *body;
};

inline constexpr OtaReply kFlashReply{kStatusFlash, "flash", R"({"ok":false,"err":"flash"})"};

/**
 * @brief 获取 ADR-0019 §2.3 的状态、结果码与错误正文
 * @param code: 流或准入结果
 * @retval 静态响应片段，成功正文由版本字段生成
 * @note 纯函数，任意任务可调用。
 */
[[nodiscard]] constexpr OtaReply replyFor(UploadCode code) noexcept
{
    switch (code) {
    case UploadCode::kOk:
        return {kStatusOk, "ok", nullptr};
    case UploadCode::kBusy:
        return {"409 Conflict", "busy", R"({"ok":false,"err":"busy"})"};
    case UploadCode::kState:
        return {"409 Conflict", "state", R"({"ok":false,"err":"state"})"};
    case UploadCode::kLength:
        return {"411 Length Required", "length", R"({"ok":false,"err":"length"})"};
    case UploadCode::kTooLarge:
        return {"413 Payload Too Large", "too_large", R"({"ok":false,"err":"too_large"})"};
    case UploadCode::kBadImage:
        return {"400 Bad Request", "bad_image", R"({"ok":false,"err":"bad_image"})"};
    case UploadCode::kValidate:
        return {"400 Bad Request", "validate", R"({"ok":false,"err":"validate"})"};
    case UploadCode::kTimeout:
        return {"408 Request Timeout", "timeout", R"({"ok":false,"err":"timeout"})"};
    case UploadCode::kAborted:
        return {"503 Service Unavailable", "aborted", R"({"ok":false,"err":"aborted"})"};
    case UploadCode::kFlash:
    default:
        return kFlashReply;
    }
}

struct OtaInfo {
    std::string_view running;
    std::string_view project;
    std::string_view part;
    std::string_view state;
    std::optional<std::string_view> other;
    size_t max = 0U;
    bool busy = false;
};

/**
 * @brief 将原文写入输出缓冲并推进剩余视图
 * @param text: 已符合 JSON 语法的文本
 * @param[out] remaining: 未使用的输出缓冲，失败时保持原样
 * @retval true 已写入
 * @retval false 容量不足
 */
[[nodiscard]] inline bool appendText(std::string_view text, std::span<char> &remaining) noexcept
{
    if (text.size() > remaining.size()) {
        return false;
    }
    std::ranges::copy(text, remaining.begin());
    remaining = remaining.subspan(text.size());
    return true;
}

/**
 * @brief 追加一个 JSON 字符串内的字节，保留 Web JSON 的转义格式
 * @param ch: 原始字节
 * @param[out] remaining: 未使用的输出缓冲，失败时保持原样
 * @retval true 已写入
 * @retval false 容量不足
 */
[[nodiscard]] inline bool appendStringByte(char ch, std::span<char> &remaining) noexcept
{
    switch (ch) {
    case '"':
        return appendText("\\\"", remaining);
    case '\\':
        return appendText("\\\\", remaining);
    case '\b':
        return appendText("\\b", remaining);
    case '\f':
        return appendText("\\f", remaining);
    case '\n':
        return appendText("\\n", remaining);
    case '\r':
        return appendText("\\r", remaining);
    case '\t':
        return appendText("\\t", remaining);
    default:
        break;
    }
    // 按无符号字节区分 ASCII 控制符，UTF-8 高位字节原样输出。
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20U) {
        constexpr std::string_view kHex = "0123456789abcdef";
        const std::array escape{'\\', 'u', '0', '0', kHex[byte >> 4U], kHex[byte & 0x0FU]};
        return appendText({escape.data(), escape.size()}, remaining);
    }
    return appendText({&ch, 1U}, remaining);
}

/**
 * @brief 追加带引号的 JSON 字符串
 * @param text: 原始文本
 * @param[out] remaining: 未使用的输出缓冲，失败时可能写入部分字符串
 * @retval true 已写入
 * @retval false 容量不足
 */
[[nodiscard]] inline bool appendString(std::string_view text, std::span<char> &remaining) noexcept
{
    if (!appendText("\"", remaining)) {
        return false;
    }
    for (const char ch : text) {
        if (!appendStringByte(ch, remaining)) {
            return false;
        }
    }
    return appendText("\"", remaining);
}

/**
 * @brief 追加无符号十进制数
 * @param value: 数值
 * @param[out] remaining: 未使用的输出缓冲，失败时保持原样
 * @retval true 已写入
 * @retval false 容量不足
 */
[[nodiscard]] inline bool appendNumber(size_t value, std::span<char> &remaining) noexcept
{
    // Xtensa 的 std::to_chars 会在小整数路径也生成 256 B 栈帧。
    std::array<char, std::numeric_limits<size_t>::digits10 + 1U> digits{};
    constexpr size_t kDecimalBase = 10U;
    size_t first = digits.size();
    do {
        // 余数为 0..9，映射到十进制 ASCII 字符。
        digits[--first] = static_cast<char>('0' + value % kDecimalBase);
        value /= kDecimalBase;
    } while (value != 0U);
    const auto text = std::span{digits}.subspan(first);
    return appendText({text.data(), text.size()}, remaining);
}

/**
 * @brief 编码 OTA 成功应答，按契约顺序写入字段
 * @param version: 新镜像版本
 * @param[out] out: 调用方的应答缓冲，容量不足时内容不保证
 * @retval 完整 JSON 字节数，无容量时返回 0
 * @note 单任务独占输出缓冲，不阻塞。
 */
[[nodiscard]] inline size_t renderSuccess(std::string_view version, std::span<char> out) noexcept
{
    auto remaining = out;
    if (!appendText(R"({"ok":true,"ver":)", remaining) || !appendString(version, remaining) ||
        !appendText(R"(,"reboot_ms":)", remaining) || !appendNumber(kRebootMs, remaining) ||
        !appendText("}", remaining)) {
        return 0U;
    }
    return out.size() - remaining.size();
}

/**
 * @brief 编码槽信息，不可读的另一槽以 null 表示
 * @param info: 当前槽与另一槽的快照
 * @param[out] out: 调用方的应答缓冲，容量不足时内容不保证
 * @retval 完整 JSON 字节数，无容量时返回 0
 * @note 单任务独占输出缓冲，不阻塞。
 */
[[nodiscard]] inline size_t renderInfo(const OtaInfo &info, std::span<char> out) noexcept
{
    auto remaining = out;
    if (!appendText(R"({"running":)", remaining) || !appendString(info.running, remaining) ||
        !appendText(R"(,"project":)", remaining) || !appendString(info.project, remaining) ||
        !appendText(R"(,"part":)", remaining) || !appendString(info.part, remaining) ||
        !appendText(R"(,"state":)", remaining) || !appendString(info.state, remaining) ||
        !appendText(R"(,"other":)", remaining) ||
        !(info.other ? appendString(*info.other, remaining) : appendText("null", remaining)) ||
        !appendText(R"(,"max":)", remaining) || !appendNumber(info.max, remaining) ||
        !appendText(R"(,"busy":)", remaining) ||
        !appendText(info.busy ? "true}" : "false}", remaining)) {
        return 0U;
    }
    return out.size() - remaining.size();
}

} // namespace app::web_ota
