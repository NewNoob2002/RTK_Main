// 解码器诊断文本写入器（ADR-0005 §5 / ADR-0009 §2.11）：供各组件的 Decoder::explain 实现共用
// （stream_demux 自带解码器、device_proto 的产品帧、gnss 的方言应答解码器）。不属于冻结接口。
// 只写 ASCII；越界即停止增长（explain 契约允许截断），finish() 补 0 并返回实际长度。
// 纯函数式使用：调用方持有输出 span，无全局状态，可重入、ISR 安全。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace demux::text_writer {

namespace text {

constexpr uint8_t kDecimalBase = 10U;
constexpr uint8_t kHexadecimalBase = 16U;
constexpr uint8_t kMaxDigits = 10U; // 32 位十进制最长 10 位
constexpr uint8_t kFirstDigit = 10U;
constexpr char kDigitZero = '0';
constexpr char kUpperA = 'A';
constexpr uint8_t kPrintableMin = 0x20U;
constexpr uint8_t kPrintableMax = 0x7EU;

} // namespace text

/** 定宽十六进制/十进制数字与字面量拼接的有界写入器。 */
class TextWriter {
public:
    explicit TextWriter(std::span<char> out) noexcept : out_(out) {}

    TextWriter &literal(std::string_view value) noexcept
    {
        for (const char ch : value) {
            put(ch);
        }
        return *this;
    }

    TextWriter &decimal(std::size_t value) noexcept
    {
        return number(static_cast<uint32_t>(value), text::kDecimalBase, 0U);
    }

    /** 写 "0x" + 至少 width 位大写十六进制。 */
    TextWriter &hex(uint32_t value, uint8_t width) noexcept
    {
        put('0');
        put('x');
        return number(value, text::kHexadecimalBase, width);
    }

    /** 写一个原始字节的可见形式：可打印 ASCII 加引号，否则 0xNN。 */
    TextWriter &byteValue(uint8_t value) noexcept
    {
        if ((value >= text::kPrintableMin) && (value <= text::kPrintableMax)) {
            put('\'');
            put(static_cast<char>(value));
            put('\'');
            return *this;
        }
        return hex(static_cast<uint32_t>(value), 2U);
    }

    /** 补 0 结束（空间不足则不写，按截断处理）并返回已写长度（不含 0）。 */
    std::size_t finish() noexcept
    {
        if (len_ < out_.size()) {
            out_[len_] = '\0';
        }
        return len_;
    }

private:
    void put(char ch) noexcept
    {
        if (len_ < out_.size()) {
            out_[len_] = ch;
            ++len_;
        }
    }

    TextWriter &number(uint32_t value, uint8_t base, uint8_t width) noexcept
    {
        std::array<char, text::kMaxDigits> digits{};
        std::size_t written = 0U;
        uint32_t rest = value;
        while ((rest != 0U) && (written < text::kMaxDigits)) {
            const uint32_t digit = rest % base;
            rest /= base;
            const char ch = (digit < text::kFirstDigit)
                                ? static_cast<char>(text::kDigitZero + digit)
                                : static_cast<char>(text::kUpperA + (digit - text::kFirstDigit));
            digits[written] = ch;
            ++written;
        }
        if (written == 0U) {
            digits[0] = text::kDigitZero;
            written = 1U;
        }
        for (std::size_t pad = written; pad < width; ++pad) {
            put(text::kDigitZero);
        }
        while (written != 0U) {
            --written;
            put(digits[written]);
        }
        return *this;
    }

    std::span<char> out_;
    std::size_t len_ = 0U;
};

} // namespace demux::text_writer
