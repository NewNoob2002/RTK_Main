// GNSS 下行发送命令日志格式化（区分 ASCII 文本与二进制 UBX 十六进制，ADR-0009 §2.12）。
// 线程：纯函数，无内部状态，不分配堆；由 kIo 写入日志与主机测试共用。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

namespace app::detail {

/**
 * @brief 去除末尾 CR/LF 后判断是否为全可打印 ASCII 命令正文。
 * @param raw: 原始 TX 字节流。
 * @retval asciiLen: 可打印正文长度；含非可打印字符或为空时返回 0。
 */
[[nodiscard]] inline size_t printableAsciiTxLen(std::span<const uint8_t> raw) noexcept
{
    size_t len = raw.size();
    while ((len > 0U) && ((raw[len - 1U] == '\r') || (raw[len - 1U] == '\n'))) {
        --len;
    }
    if (len == 0U) {
        return 0U;
    }
    constexpr uint8_t kAsciiPrintableMin = 0x20U;
    constexpr uint8_t kAsciiPrintableMax = 0x7EU;
    for (size_t i = 0U; i < len; ++i) {
        if ((raw[i] < kAsciiPrintableMin) || (raw[i] > kAsciiPrintableMax)) {
            return 0U;
        }
    }
    return len;
}

/**
 * @brief 将 TX 命令格式化为日志文本（ASCII 保持 "..."；二进制输出十六进制摘要）。
 * @param raw: 实际发送给 GNSS UART 的原始字节。
 * @param out: 目标 NUL 结尾字符串缓冲。
 * @retval written: 写入 out 的字符数（不含末尾 NUL）。
 */
inline size_t formatGnssTxLog(std::span<const uint8_t> raw, std::span<char> out) noexcept
{
    if (out.empty()) {
        return 0U;
    }
    out[0U] = '\0';
    constexpr size_t kQuoteAndNulBytes = 3U;
    const size_t asciiLen = printableAsciiTxLen(raw);
    if (asciiLen > 0U) {
        const size_t maxText =
            (out.size() > kQuoteAndNulBytes) ? (out.size() - kQuoteAndNulBytes) : 0U;
        const size_t textLen = std::min(asciiLen, maxText);
        out[0U] = '"';
        if (textLen > 0U) {
            std::memcpy(out.subspan(1U, textLen).data(), raw.data(), textLen);
        }
        if ((1U + textLen + 1U) < out.size()) {
            out[1U + textLen] = '"';
            out[2U + textLen] = '\0';
            return 2U + textLen;
        }
        out[out.size() - 1U] = '\0';
        return out.size() - 1U;
    }

    constexpr size_t kMaxHexBytes = 16U;
    constexpr size_t kUbxMinHeaderBytes = 4U;
    constexpr size_t kUbxClassIdx = 2U;
    constexpr size_t kUbxIdIdx = 3U;
    constexpr uint8_t kUbxSync1 = 0xB5U;
    constexpr uint8_t kUbxSync2 = 0x62U;
    size_t pos = 0U;
    const auto append = [&](int n) noexcept {
        if (n > 0) {
            const size_t add = static_cast<size_t>(n);
            pos = std::min(pos + add, out.size() - 1U);
        }
    };
    if ((raw.size() >= kUbxMinHeaderBytes) && (raw[0U] == kUbxSync1) && (raw[1U] == kUbxSync2)) {
        append(std::snprintf(
            out.data(), out.size(),
            "UBX 0x%02X 0x%02X, %u bytes:", static_cast<unsigned>(raw[kUbxClassIdx]),
            static_cast<unsigned>(raw[kUbxIdIdx]), static_cast<unsigned>(raw.size())));
    } else {
        append(
            std::snprintf(out.data(), out.size(), "%u bytes:", static_cast<unsigned>(raw.size())));
    }
    const size_t shown = std::min(raw.size(), kMaxHexBytes);
    for (size_t i = 0U; (i < shown) && ((pos + 1U) < out.size()); ++i) {
        append(std::snprintf(out.subspan(pos).data(), out.size() - pos, " %02X",
                             static_cast<unsigned>(raw[i])));
    }
    if ((raw.size() > kMaxHexBytes) && ((pos + 1U) < out.size())) {
        append(std::snprintf(out.subspan(pos).data(), out.size() - pos, " ..."));
    }
    return pos;
}

} // namespace app::detail
