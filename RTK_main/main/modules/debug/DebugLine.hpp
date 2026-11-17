// 调试模块内部契约：控制台整行写出与 snprintf 截断，只在 modules/debug/*.cpp 之间共享。
// 仅 console REPL 任务调用；经 cli 统一出口，不分配堆、非 ISR。

#pragma once

#include <cstddef>

#include "cli/Out.hpp"

namespace app::detail {

/**
 * @brief 整行写控制台（经 cli 统一出口）
 * @param text: 待写文本
 * @param len: 字节数
 * @note console REPL 任务；阻塞在控制台锁上，ISR 不可用。
 */
inline void writeLine(const char *text, size_t len) noexcept
{
    cli::outWrite(text, len);
}

/**
 * @brief 把 snprintf 结果截断到缓冲区内
 * @param written: snprintf 返回值
 * @param cap: 缓冲区容量
 * @retval 写入的字节数
 * @note 纯函数，任意任务可调用。
 */
inline size_t clampLen(int written, size_t cap) noexcept
{
    if (written <= 0) {
        return 0U;
    }
    const auto len = static_cast<size_t>(written);
    return (len < cap) ? len : (cap - 1U);
}

} // namespace app::detail
