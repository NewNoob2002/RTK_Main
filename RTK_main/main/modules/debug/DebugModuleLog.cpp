// 调试日志输出：文本行、十六进制转储与事件抽取。
// 仅 console REPL 任务与 trace_out 输出任务；逐行写控制台，读日志环形缓冲，不分配堆、非 ISR。

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "AppContext.hpp"
#include "DebugLine.hpp"
#include "fw/Log.hpp"
#include "modules/debug/DebugModule.hpp"
#include "modules/debug/TraceText.hpp"

namespace app {

namespace {

/* 事件行 = "[ssss.mmm] " 前缀 + 正文 + "\r\n"；前缀最长 "[4294967.295] " 14 字节，取 24 留余量。 */
constexpr size_t kLinePrefixBytes = 24U;
/* 十六进制转储的 ASCII 列：不可打印字节用 '.' 占位。 */
constexpr uint8_t kPrintableMin = 0x20U;
constexpr uint8_t kPrintableMax = 0x7EU;
constexpr char kDotChar = '.';
/* 时间戳拆分用：毫秒与秒。 */
constexpr uint32_t kMsPerSecond = 1000U;

using detail::clampLen;
using detail::writeLine;

} // namespace

void DebugModule::printLogText(const fw::LogHeader &hdr, std::span<const uint8_t> payload,
                               char *out, size_t cap) noexcept
{
    static_assert(kLineBytes + kLinePrefixBytes >= fw::kLogLineMax, "log line buffer too small");
    // 与直写通路共用 fw::logFormatLine，两条通路的行格式（时间、类别、级别前缀）完全一致。
    const size_t len = fw::logFormatLine(std::span<char>(out, cap), hdr, payload);
    writeLine(out, len);
}

namespace {

/**
 * @brief 追加一行的偏移前缀、十六进制字节、对齐空格与 ASCII 列
 * @param dump: 输出行缓冲（覆盖写）
 * @param line: 本行字节（≤ kHexBytesPerLine）
 * @param offset: 本行在载荷中的偏移（用于前缀）
 * @param perLine: 每行字节数
 * @retval 写入的字节数
 * @note 仅 console REPL 任务；非 ISR，不分配堆。
 */
size_t formatHexLine(std::span<char> dump, std::span<const uint8_t> line, size_t offset,
                     size_t perLine) noexcept
{
    // 缩进 4 格 + 4 位十六进制偏移（载荷最多 256 字节，2 位足够，固定 4 位便于对齐）。
    const int prefix =
        std::snprintf(dump.data(), dump.size(), "    %04lx: ", static_cast<unsigned long>(offset));
    size_t pos = (prefix > 0) ? static_cast<size_t>(prefix) : 0U;
    for (size_t i = 0U; i < line.size(); ++i) {
        const int n = std::snprintf(&dump[pos], (pos < dump.size()) ? (dump.size() - pos) : 0U,
                                    "%02x ", static_cast<unsigned>(line[i]));
        if ((n > 0) && ((pos + static_cast<size_t>(n)) < dump.size())) {
            pos += static_cast<size_t>(n);
        }
    }
    // 不足 16 字节时补空格，让 ASCII 列对齐。
    for (size_t i = line.size(); i < perLine; ++i) {
        const int n =
            std::snprintf(&dump[pos], (pos < dump.size()) ? (dump.size() - pos) : 0U, "   ");
        if ((n > 0) && ((pos + static_cast<size_t>(n)) < dump.size())) {
            pos += static_cast<size_t>(n);
        }
    }
    if (pos < dump.size()) {
        dump[pos] = ' ';
        ++pos;
    }
    for (size_t i = 0U; i < line.size(); ++i) {
        const uint8_t byte = line[i];
        const char ch = ((byte >= kPrintableMin) && (byte <= kPrintableMax))
                            ? static_cast<char>(byte)
                            : kDotChar;
        const int n =
            std::snprintf(&dump[pos], (pos < dump.size()) ? (dump.size() - pos) : 0U, "%c", ch);
        if ((n > 0) && ((pos + static_cast<size_t>(n)) < dump.size())) {
            pos += static_cast<size_t>(n);
        }
    }
    return pos;
}

} // namespace

void DebugModule::printLogHex(const fw::LogHeader &hdr, std::span<const uint8_t> payload, char *out,
                              size_t cap) noexcept
{
    // 不打标题行：转储紧跟在同一调用点的文本句子之后，句子已给出对象与字节数（ADR-0009 §2.11 示例）。
    std::array<char, kHexLineBytes> dump{};
    for (size_t offset = 0U; offset < payload.size(); offset += kHexBytesPerLine) {
        const size_t lineLen = ((payload.size() - offset) < kHexBytesPerLine)
                                   ? (payload.size() - offset)
                                   : kHexBytesPerLine;
        const size_t pos = formatHexLine(std::span<char>{dump}, payload.subspan(offset, lineLen),
                                         offset, kHexBytesPerLine);
        writeLine(dump.data(), pos);
        writeLine("\r\n", 2U);
    }
    if (hdr.total > payload.size()) {
        const int tail = std::snprintf(out, cap, "    (+%lu bytes not shown)\r\n",
                                       static_cast<unsigned long>(hdr.total - payload.size()));
        writeLine(out, clampLen(tail, cap));
    }
}

bool DebugModule::pumpEvents(char *line, size_t cap) noexcept
{
    std::array<char, kLineBytes + kLinePrefixBytes> out{};
    bool printed = pumpDrops(out.data(), out.size());

    for (;;) {
        uint32_t traceTime = 0U;
        uint32_t logTime = 0U;
        const bool haveTrace = ring_.peekTime(traceTime);
        const bool haveLog = log_ring_.peekTime(logTime);
        if (!haveTrace && !haveLog) {
            break;
        }
        // 两个环按时间戳合并输出，保证控制台上的先后顺序与发生时一致。
        if (haveTrace && ((!haveLog) || (traceTime <= logTime))) {
            fw::TraceEvent event{};
            if (!ring_.pop(event)) {
                break;
            }
            debug::formatEvent(event, line, cap);
            const int n = std::snprintf(out.data(), out.size(), "[%4lu.%03lu] %s\r\n",
                                        static_cast<unsigned long>(event.tMs / kMsPerSecond),
                                        static_cast<unsigned long>(event.tMs % kMsPerSecond), line);
            writeLine(out.data(), clampLen(n, out.size()));
        } else {
            fw::LogHeader hdr{};
            std::array<uint8_t, fw::kLogHexMax> payload{};
            size_t copied = 0U;
            logOutputSeq_.fetch_add(1U, std::memory_order_relaxed);
            if (!log_ring_.pop(hdr, payload, copied)) {
                logOutputSeq_.fetch_add(1U, std::memory_order_release);
                break;
            }
            const std::span<const uint8_t> view{payload.data(), copied};
            if (hdr.kind == fw::LogKind::kHex) {
                printLogHex(hdr, view, out.data(), out.size());
            } else {
                printLogText(hdr, view, out.data(), out.size());
            }
            logOutputSeq_.fetch_add(1U, std::memory_order_release);
        }
        printed = true;
    }
    return printed;
}

} // namespace app
