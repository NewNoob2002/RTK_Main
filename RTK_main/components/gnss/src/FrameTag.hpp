// 帧标签（frameTag）的公共小工具（组件内部头）：把帧里的可打印词与文本/数字拼进缓冲。
// 线程安全：是（纯函数，无状态）。ISR 安全：是（不阻塞、不分配）。
// 堆与阻塞：不使用堆、不阻塞；不用 printf 族（vsnprintf 代价高且触发 vararg 规范告警）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace gnss::detail {

/** 帧标识缓冲区：`UNICORE_CMD ` + 命令名足够，24 字节（与 main 原 describe_frame() 相同）。 */
inline constexpr size_t kFrameTagBytes = 24U;
/** NMEA 五位消息名（如 GNGGA）在 '$' 之后（跳过 talker 用）。 */
inline constexpr size_t kNmeaTagOffset = 1U;
inline constexpr size_t kNmeaTagLen = 6U;
/** `$command,` 之后的命令名起点。 */
inline constexpr size_t kCommandTagOffset = 9U;
/** RTCM3 消息号占 12 位，起点为长度字段第 1 字节。 */
inline constexpr size_t kRtcmTagOffset = 3U;
inline constexpr uint32_t kRtcmMsgShift = 4U; // RTCM3 头：12 位消息号横跨两字节，高 8 位左移 4
/** 半字节位宽（十六进制一位），appendHex2 用。 */
inline constexpr uint32_t kNibbleShift = 4U;
inline constexpr uint32_t kRtcmMsgMask = 0x0FFFU;
/** 半字节掩码（十六进制一位）。 */
inline constexpr uint32_t kRtcmNibbleMask = 0x0FU;
/** UBX class/id 位于同步字之后。 */
inline constexpr size_t kUbxClassOffset = 2U;
inline constexpr uint8_t kTagPrintableMin =
    0x20U; // 空格也算可打印（命令名可能含空格，如 "UNLOG COM2"）
inline constexpr uint8_t kTagPrintableMax = 0x7EU;
inline constexpr uint8_t kCommaByte = static_cast<uint8_t>(',');
/** uint32 十进制最多 10 位；进制固定为 10（不用 printf 族）。 */
inline constexpr size_t kMaxUint32Digits = 10U;
inline constexpr uint32_t kDecimalRadix = 10U;

/**
 * @brief  把 frame[start, size) 的 ASCII 名字拷进 out
 * @param  frame: 源帧
 * @param  start: 起始下标，须 ≤ frame.size()
 * @param  out: 输出缓冲，最多写入 out.size()-1 字节（留 NUL）
 * @retval 写入长度（不含 NUL）；遇非可打印字节或 ',' 即截断
 * @note   out 为空时只返回 0，不写任何字节
 */
inline size_t copyWord(std::span<const uint8_t> frame, size_t start, std::span<char> out) noexcept
{
    size_t len = 0U;
    for (size_t i = start; (i < frame.size()) && ((len + 1U) < out.size()); ++i) {
        const uint8_t byte = frame[i];
        if ((byte == kCommaByte) || (byte < kTagPrintableMin) || (byte > kTagPrintableMax)) {
            break;
        }
        out[len] = static_cast<char>(byte);
        ++len;
    }
    if (!out.empty()) {
        out[len] = '\0';
    }
    return len;
}

/**
 * @brief  把 text 追加到 out[pos..]
 * @param  out: 输出缓冲，始终保证 NUL 结尾
 * @param  pos: 追加起点（当前已写长度）
 * @param  text: 追加内容，不要求 NUL 结尾
 * @retval 新的写入长度（不含 NUL）；缓冲不足时截断并返回 pos
 */
inline size_t appendText(std::span<char> out, size_t pos, std::string_view text) noexcept
{
    for (const char ch : text) {
        if ((pos + 1U) >= out.size()) {
            return pos;
        }
        out[pos] = ch;
        ++pos;
    }
    out[pos] = '\0';
    return pos;
}

/**
 * @brief  以十进制追加 value
 * @param  out: 输出缓冲
 * @param  pos: 追加起点
 * @param  value: 待写数值
 * @retval 新的写入长度（缓冲不足时截断）
 */
inline size_t appendUint(std::span<char> out, size_t pos, uint32_t value) noexcept
{
    std::array<char, kMaxUint32Digits> digits{};
    size_t len = 0U;
    uint32_t rest = value;
    for (;;) {
        digits[len] = static_cast<char>('0' + static_cast<char>(rest % kDecimalRadix));
        rest /= kDecimalRadix;
        ++len;
        if (rest == 0U) {
            break;
        }
    }
    while (len > 0U) {
        --len;
        if ((pos + 1U) >= out.size()) {
            return pos;
        }
        out[pos] = digits[len];
        ++pos;
    }
    out[pos] = '\0';
    return pos;
}

/**
 * @brief  以两位小写十六进制追加 value（0x00..0xFF）
 * @param  out: 输出缓冲
 * @param  pos: 追加起点
 * @param  value: 待写字节
 * @retval 新的写入长度（缓冲不足时保持 pos）
 */
inline size_t appendHex2(std::span<char> out, size_t pos, uint8_t value) noexcept
{
    constexpr std::string_view kDigits = "0123456789abcdef";
    const auto high =
        static_cast<size_t>((static_cast<uint32_t>(value) >> kNibbleShift) & kRtcmNibbleMask);
    const auto low = static_cast<size_t>(static_cast<uint32_t>(value) & kRtcmNibbleMask);
    if ((pos + 2U) >= out.size()) {
        return pos;
    }
    out[pos] = kDigits[high];
    out[pos + 1U] = kDigits[low];
    out[pos + 2U] = '\0';
    return pos + 2U;
}

/**
 * @brief  组成 "<prefix><frame[start..] 的可打印词>"
 * @param  out: 输出缓冲
 * @param  prefix: 前缀文本（如 "NMEA "、"RTCM3 "）
 * @param  frame: 源帧
 * @param  start: 词在帧内的起始下标
 * @retval 写入总长度（不含 NUL）；prefix 写不下时只返回 prefix 长度
 */
inline size_t tagWithWord(std::span<char> out, std::string_view prefix,
                          std::span<const uint8_t> frame, size_t start) noexcept
{
    const size_t pos = appendText(out, 0U, prefix);
    if ((pos == 0U) || (pos + 1U) >= out.size()) {
        return pos;
    }
    return pos + copyWord(frame, start, out.subspan(pos));
}

/**
 * @brief  frame 是否以 prefix 开头
 * @param  frame: 帧字节
 * @param  prefix: 前缀文本
 * @retval true 命中；false 长度不足或不匹配
 */
inline bool startsWith(std::span<const uint8_t> frame, std::string_view prefix) noexcept
{
    size_t len = 0U;
    while (len < prefix.size()) {
        if ((len >= frame.size()) || (frame[len] != static_cast<uint8_t>(prefix[len]))) {
            return false;
        }
        ++len;
    }
    return true;
}

} // namespace gnss::detail
