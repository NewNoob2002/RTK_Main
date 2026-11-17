// 厂商 ASCII 命令的公共小工具（组件内部头）：十进制/周期/引号字段/校验尾等文本操作。
// 原来 Unicore 与 ComNav 两个驱动各有一份等价实现，这里合并为一份，避免改一处漏一处。
// 只往 CmdBuf 或入参 span 上工作；不发送、不计时、不分配，可视为纯函数。
// 线程/ISR 安全：是（无可变状态，不阻塞、不分配）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gnss/Receiver.hpp"

namespace gnss::ascii {

/** 可打印 ASCII 区间（含空格）：命令名可能含空格，如 "UNLOG COM2"。 */
inline constexpr uint8_t kPrintableMin = 0x20U;
inline constexpr uint8_t kPrintableMax = 0x7EU;
/** 十进制写出时的最多位数（uint32 最大 10 位）。 */
inline constexpr size_t kUintDigits = 10U;
inline constexpr uint8_t kDecimalBase = 10U;
/** 'A' 在十六进制中的数值，用于 hexValue。 */
inline constexpr uint8_t kHexDigitOffset = 10U;
inline constexpr uint8_t kHexShift = 4U;
/** 一秒的毫秒数与百分之一秒的毫秒数（周期文本换算用）。 */
inline constexpr uint32_t kMsPerSecond = 1000U;
inline constexpr uint32_t kMillisPerCentiSecond = 10U;
/** 应答尾 "*hh\r\n" 的总字节数与校验和位数。 */
inline constexpr size_t kTailBytes = 5U;
inline constexpr size_t kChecksumDigits = 2U;
/** 引号字段的定界符与校验段起始符。 */
inline constexpr char kQuote = '"';
inline constexpr char kStar = '*';
inline constexpr char kCr = '\r';
inline constexpr char kLf = '\n';

/**
 * @brief 是否为十六进制数字（0-9 A-F a-f）。
 * @param c: 待判定字节。
 * @retval true: 是十六进制数字。
 * @retval false: 其他。
 */
inline bool isHex(uint8_t c) noexcept
{
    return ((c >= static_cast<uint8_t>('0')) && (c <= static_cast<uint8_t>('9'))) ||
           ((c >= static_cast<uint8_t>('A')) && (c <= static_cast<uint8_t>('F'))) ||
           ((c >= static_cast<uint8_t>('a')) && (c <= static_cast<uint8_t>('f')));
}

/**
 * @brief 十六进制数字的数值（调用方须先用 isHex 判定）。
 * @param c: 十六进制数字字节。
 * @retval 0..15: 数值（非法字节按小写分支计算，不抛错）。
 */
inline uint8_t hexValue(uint8_t c) noexcept
{
    if ((c >= static_cast<uint8_t>('0')) && (c <= static_cast<uint8_t>('9'))) {
        return static_cast<uint8_t>(c - static_cast<uint8_t>('0'));
    }
    if ((c >= static_cast<uint8_t>('A')) && (c <= static_cast<uint8_t>('F'))) {
        return static_cast<uint8_t>(c - static_cast<uint8_t>('A') + kHexDigitOffset);
    }
    return static_cast<uint8_t>(c - static_cast<uint8_t>('a') + kHexDigitOffset);
}

/**
 * @brief 是否为可打印 ASCII（0x20..0x7E）。
 * @param c: 待判定字节。
 * @retval true: 可打印。
 * @retval false: 控制字符或非 ASCII。
 */
inline bool isPrintable(uint8_t c) noexcept
{
    return (c >= kPrintableMin) && (c <= kPrintableMax);
}

/**
 * @brief 是否为命令报文体允许的字节：可打印且不是 '$' 与 '*'（两者是帧结构字符）。
 * @param c: 待判定字节。
 * @retval true: 可作为报文体字节。
 * @retval false: 帧结构字符或不可打印。
 */
inline bool isBodyByte(uint8_t c) noexcept
{
    return isPrintable(c) && (c != static_cast<uint8_t>('$')) && (c != static_cast<uint8_t>(kStar));
}

/**
 * @brief 以十进制把 value 追加到 out。
 * @param out: 目标命令缓冲，剩余空间不足时不改动内容。
 * @param value: 待写数值；0 不写任何字节（保持与基线脚本逐字节一致，调用方需要 "0" 时自行写）。
 * @retval true: 写入成功（或 value 为 0）。
 * @retval false: 缓冲空间不足，out 内容不变。
 */
inline bool appendUint(CmdBuf &out, uint32_t value) noexcept
{
    std::array<uint8_t, kUintDigits> digits{};
    size_t count = 0;
    while ((value != 0U) && (count < digits.size())) {
        digits[count] = static_cast<uint8_t>(static_cast<uint32_t>('0') + (value % kDecimalBase));
        ++count;
        value /= kDecimalBase;
    }
    for (size_t i = count; i > 0U; --i) {
        if (!out.append(std::span<const uint8_t>(&digits[i - 1U], 1U))) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 把周期毫秒写成秒文本（"1"/"0.5"/"0.2"/"0.1"/"0.05"），接收机只接受 10 ms 整数倍。
 * @param out: 目标命令缓冲。
 * @param periodMs: 周期（毫秒）。
 * @retval true: 写入成功。
 * @retval false: 不是 10 ms 整数倍，或缓冲空间不足。
 */
inline bool appendPeriod(CmdBuf &out, uint32_t periodMs) noexcept
{
    if ((periodMs % kMillisPerCentiSecond) != 0U) {
        return false;
    }
    const uint32_t whole = periodMs / kMsPerSecond;
    // periodMs 已是 10 ms 倍数，因此 (periodMs % 1000) / 10 必为整数百分位。
    const uint32_t centi = (periodMs % kMsPerSecond) / kMillisPerCentiSecond;
    if (whole == 0U) {
        if (!out.append("0")) {
            return false;
        }
    } else if (!appendUint(out, whole)) {
        return false;
    }
    if (centi == 0U) {
        return true;
    }
    if (!out.append(".")) {
        return false;
    }
    // 只写有效数字：100 ms 写 "0.1"、500 ms 写 "0.5"、50 ms 写 "0.05"（与基线脚本一致）。
    if ((centi % kMillisPerCentiSecond) == 0U) {
        return appendUint(out, centi / kMillisPerCentiSecond);
    }
    if ((centi < kMillisPerCentiSecond) && !out.append("0")) {
        return false;
    }
    return appendUint(out, centi);
}

/**
 * @brief 取 frame 中 pos 之后的第一个引号字段。
 * @param frame: 整帧字节。
 * @param pos: 入参为搜索起点，成功时移到结束引号之后。
 * @param out: 成功时指向引号内文本（不拷贝）。
 * @retval true: 找到完整引号字段。
 * @retval false: 未闭合，或先遇到 '*'（校验段开始，说明没有该字段）。
 */
inline bool nextQuoted(std::span<const uint8_t> frame, size_t &pos,
                       std::span<const uint8_t> &out) noexcept
{
    while ((pos < frame.size()) && (frame[pos] != static_cast<uint8_t>(kQuote))) {
        if (frame[pos] == static_cast<uint8_t>(kStar)) {
            return false;
        }
        ++pos;
    }
    if (pos >= frame.size()) {
        return false;
    }
    ++pos;
    const size_t start = pos;
    while ((pos < frame.size()) && (frame[pos] != static_cast<uint8_t>(kQuote))) {
        ++pos;
    }
    if (pos >= frame.size()) {
        return false;
    }
    out = frame.subspan(start, pos - start);
    ++pos;
    return true;
}

/**
 * @brief 字段是否与 text 完全相等（逐字节，大小写敏感）。
 * @param field: 字段字节。
 * @param text: 期望文本。
 * @retval true: 长度与内容都相同。
 * @retval false: 任一不同。
 */
inline bool quotedEquals(std::span<const uint8_t> field, std::string_view text) noexcept
{
    if (field.size() != text.size()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (field[i] != static_cast<uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 字段是否以 text 开头（用于型号前缀匹配，如 "UM9"）。
 * @param field: 字段字节。
 * @param text: 期望前缀。
 * @retval true: 前缀匹配（字段可更长）。
 * @retval false: 字段更短或任一字节不同。
 */
inline bool quotedStartsWith(std::span<const uint8_t> field, std::string_view text) noexcept
{
    if (field.size() < text.size()) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if (field[i] != static_cast<uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 字节串是否以给定前缀文本开头（不需要 NUL 结尾的入参）。
 * @param text: 待检查字节。
 * @param prefix: 期望前缀。
 * @retval true: 前缀匹配。
 * @retval false: 长度不足或字节不同。
 */
inline bool startsWith(std::span<const uint8_t> text, std::string_view prefix) noexcept
{
    return quotedStartsWith(text, prefix);
}

/**
 * @brief 字节串是否以给定前缀字节开头（用于与已发送命令逐字节回显比较）。
 * @param text: 待检查字节。
 * @param prefix: 期望前缀字节。
 * @retval true: 前缀匹配。
 * @retval false: 长度不足或字节不同。
 */
inline bool bytesStartsWith(std::span<const uint8_t> text, std::span<const uint8_t> prefix) noexcept
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (text[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 应答尾是否为 "*hh\r\n" 结构（两条校验位为十六进制数字）。
 * @param frame: 整帧字节。
 * @retval true: 以 "\r\n" 结尾，且终止符之前 5 字节为 '*' + 两位十六进制。
 * @retval false: 结构不符（长度不足或字节不对）。
 */
inline bool hasHexTail(std::span<const uint8_t> frame) noexcept
{
    const size_t size = frame.size();
    if (size < kTailBytes) {
        return false;
    }
    const size_t starPos = size - kTailBytes;
    return (frame[size - 1U] == static_cast<uint8_t>(kLf)) &&
           (frame[size - 2U] == static_cast<uint8_t>(kCr)) &&
           (frame[starPos] == static_cast<uint8_t>(kStar)) && isHex(frame[starPos + 1U]) &&
           isHex(frame[starPos + 2U]);
}

/**
 * @brief 去掉命令文本末尾的 CR/LF，得到可回显比较的长度。
 * @param sent: 已发送的命令字节（可能带 "\r\n"）。
 * @retval 长度: sent 去掉尾部 CR/LF 后的字节数。
 */
inline size_t stripLineEnd(std::span<const uint8_t> sent) noexcept
{
    size_t len = sent.size();
    while ((len > 0U) && ((sent[len - 1U] == static_cast<uint8_t>(kCr)) ||
                          (sent[len - 1U] == static_cast<uint8_t>(kLf)))) {
        --len;
    }
    return len;
}

/**
 * @brief 计算 [begin, end) 的 XOR 校验（NMEA 风格）。
 * @param frame: 整帧字节。
 * @param begin: 参与校验的起点。
 * @param end: 参与校验的终点（不含）。
 * @retval 值: XOR 结果（范围非法时按空区间处理，返回 0）。
 */
inline uint8_t xorChecksum(std::span<const uint8_t> frame, size_t begin, size_t end) noexcept
{
    uint8_t checksum = 0;
    if ((begin >= end) || (end > frame.size())) {
        return checksum;
    }
    for (size_t i = begin; i < end; ++i) {
        checksum = static_cast<uint8_t>(checksum ^ frame[i]);
    }
    return checksum;
}

} // namespace gnss::ascii
