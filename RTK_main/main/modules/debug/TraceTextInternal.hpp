// 追踪文本内部契约：追加文本与值格式化，只在 modules/debug/TraceText*.cpp 之间共享。
// 仅 DebugModule 输出任务调用（唯一格式化者），非 ISR；无堆、无阻塞、不调用日志。

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>

#include "modules/debug/TraceText.hpp"

namespace app::debug::detail {

/**
 * @brief 追加一段定长文本并保持 0 结尾
 * @param out: 输出缓冲
 * @param cap: 缓冲容量
 * @param pos: 当前写入位置（原地推进）
 * @param fmt: printf 风格格式
 * @note 仅 DebugModule 输出任务；不分配堆。
 */
inline void append(char *out, size_t cap, size_t &pos, const char *fmt, ...) noexcept
{
    if ((out == nullptr) || (cap == 0U) || (pos >= (cap - 1U))) {
        return;
    }
    // 理由（下面三处 NOLINT）：C 可变参数只能走裸指针——va_list 在 x86_64 上就是数组类型、
    // vsnprintf 不接受 span；越界已由上面的 pos 检查排除，写入长度由返回值夹紧。
    va_list args;
    va_start(args,
             fmt); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay): va_list 即数组
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const int written = vsnprintf(&out[pos], cap - pos, fmt, args);
    va_end(args); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay): va_list 即数组
    if (written <= 0) {
        return;
    }
    const size_t len = static_cast<size_t>(written);
    // 返回值为“若空间足够会写多少”，因此越界时直接顶到末尾。
    pos = (len < (cap - pos)) ? (pos + len) : (cap - 1U);
}

/** 按字段值类型渲染一段文本（平铺 18 分支映射）。
 * @param out: 输出缓冲
 * @param cap: 缓冲容量
 * @param pos: 当前写入位置（原地推进）
 * @param value: 值类型
 * @param ev: 事件本体（取 a/b/session 等）
 * @note 仅 DebugModule 输出任务；不分配堆。
 */
void appendValue(char *out, size_t cap, size_t &pos, TraceValue value,
                 const fw::TraceEvent &ev) noexcept;

} // namespace app::debug::detail
