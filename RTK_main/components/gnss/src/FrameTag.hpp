// 帧标签（frame_tag）的公共小工具：把帧里的可打印词拷进缓冲、按 printf 格式写回。
// 仅组件内部使用（src/ 私有头），不参与公共接口。
// 线程/ISR 安全：是（纯函数，不阻塞、不分配）。
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

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
inline constexpr uint32_t kRtcmMsgShift = 4U;
inline constexpr uint32_t kRtcmMsgMask = 0x0FFFU;
/** UBX class/id 位于同步字之后。 */
inline constexpr size_t kUbxClassOffset = 2U;
inline constexpr uint8_t kTagPrintableMin = 0x20U; // 空格也算可打印（命令名可能含空格，如 "UNLOG COM2"）
inline constexpr uint8_t kTagPrintableMax = 0x7EU;
inline constexpr uint8_t kCommaByte = static_cast<uint8_t>(',');

/** 把 frame[start, size) 的 ASCII 名字拷进 out（最多 out.size()-1 字节），返回写入长度。 */
inline size_t copy_word(std::span<const uint8_t> frame, size_t start, std::span<char> out) noexcept
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

/** snprintf 到 out，返回不含 NUL 的长度（截断时返回 out.size()-1）。 */
[[gnu::format(printf, 2, 3)]] inline size_t format_tag(std::span<char> out, const char *fmt, ...) noexcept
{
    if (out.empty()) {
        return 0U;
    }
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(out.data(), out.size(), fmt, args);
    va_end(args);
    if (written <= 0) {
        out[0] = '\0';
        return 0U;
    }
    const size_t len = static_cast<size_t>(written);
    return len < out.size() ? len : (out.size() - 1U);
}

/** frame 是否以 prefix 开头（含前缀本身）。 */
inline bool starts_with(std::span<const uint8_t> frame, const char *prefix) noexcept
{
    size_t len = 0U;
    while (prefix[len] != '\0') {
        if ((len >= frame.size()) || (frame[len] != static_cast<uint8_t>(prefix[len]))) {
            return false;
        }
        ++len;
    }
    return true;
}

} // namespace gnss::detail
