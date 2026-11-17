// 解码器诊断文本写入器（ADR-0005 §5 / ADR-0009 §2.11）：供各组件的 Decoder::explain 实现共用（streamDemux
// 自带解码器、device_proto 的产品帧、gnss 的方言应答解码器）。不属于冻结接口。
// 只写 ASCII，越界即停止增长（explain 契约允许截断）；无全局状态、不阻塞、无堆：可重入、线程与 ISR 安全。

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

/**
 * @brief  定宽十六进制/十进制数字与字面量拼接的有界写入器
 * @note   空间不足时静默丢弃后续字符，finish() 返回实际写入长度（即被截断）；无全局状态，可重入、ISR 安全。
 */
class TextWriter {
public:
    /**
     * @brief  绑定输出缓冲（不写入任何内容）
     * @param  out: 输出缓冲；超出其长度的写入被丢弃
     */
    explicit TextWriter(std::span<char> out) noexcept : out_(out) {}

    /**
     * @brief  追加一段字面量
     * @param  value: 待追加的字符序列，逐字符写入
     * @retval *this，便于链式调用
     */
    TextWriter &literal(std::string_view value) noexcept
    {
        for (const char ch : value) {
            put(ch);
        }
        return *this;
    }

    /**
     * @brief  追加十进制的无符号数（最多 10 位，无前导 0）
     * @param  value: 待转换的值；大于 32 位范围时按低 32 位截断
     * @retval *this，便于链式调用
     */
    TextWriter &decimal(std::size_t value) noexcept
    {
        return number(static_cast<uint32_t>(value), text::kDecimalBase, 0U);
    }

    /**
     * @brief  写 "0x" + 至少 width 位大写十六进制
     * @param  value: 待转换的值，按 32 位处理
     * @param  width: 最小位数，不足时前导补 '0'（0 表示不补）
     * @retval *this，便于链式调用
     */
    TextWriter &hex(uint32_t value, uint8_t width) noexcept
    {
        put('0');
        put('x');
        return number(value, text::kHexadecimalBase, width);
    }

    /**
     * @brief  写一个原始字节的可见形式：可打印 ASCII 加引号，否则 0xNN
     * @param  value: 待写出的字节
     * @retval *this，便于链式调用
     */
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

    /**
     * @brief  补 0 结束文本
     * @retval 已写长度（不含结尾 0）；缓冲已满时没写 0，长度即缓冲容量
     */
    std::size_t finish() noexcept
    {
        if (len_ < out_.size()) {
            out_[len_] = '\0';
        }
        return len_;
    }

private:
    /**
     * @brief  写入一个字符
     * @param  ch: 字符；缓冲已满时静默丢弃
     */
    void put(char ch) noexcept
    {
        if (len_ < out_.size()) {
            out_[len_] = ch;
            ++len_;
        }
    }

    /**
     * @brief  按给定进制写 value（低位先算、最后倒序输出），不足 width 位补 '0'
     * @param  value: 待转换的 32 位无符号值；0 时输出 "0"
     * @param  base: 进制（10 或 16）
     * @param  width: 最小位数，0 表示不补
     * @retval *this，便于链式调用
     */
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
