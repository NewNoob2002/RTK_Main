// 控制台统一输出出口（ADR-0009 §2.8）：命令回复、trace 事件行、周期报告与 ESP_LOG 共用。
// 先在静态缓冲里格式化整行，再一次交给输出端（UART 直写或 stdout），绕开 UART VFS 的逐字节写出。
// 线程：任意任务可调用（内部互斥，持锁期间可能阻塞在 UART TX 环形缓冲上）；不可在 ISR 中调用；不分配堆。

#pragma once

#include <cstdarg>
#include <cstddef>

namespace cli {

/** 输出端：把 len 字节原样写出（可阻塞）。由 console init() 安装；未安装时写 stdout。 */
using OutSink = void (*)(const char *text, size_t len) noexcept;

/** 单次格式化的整行缓冲长度；超过时整条改走 stdout（慢但不截断）。 */
inline constexpr size_t kOutLineMax = 256U;

/**
 * 安装输出端。sink 为 nullptr → 写 stdout（host 测试与 USB 控制台）。
 * crlf=true 时把未跟在 '\r' 之后的 '\n' 展开为 "\r\n"（绕过 VFS 后行尾转换由这里负责）。
 * 只在启动阶段调用一次，调用时不得有并发输出。
 */
void setOutSink(OutSink sink, bool crlf) noexcept;

/** 原样输出一段文本（按 crlf 设置转换行尾）。 */
void outWrite(const char *text, size_t len) noexcept;

/** 格式化后输出；一次调用对应一次（或少数几次）输出端写入，调用之间不会被其他输出插入。 */
void outPrintf(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
void outVprintf(const char *fmt, va_list args) noexcept;

/**
 * ESP_LOG 钩子用：同 out_vprintf，但同一任务重入（输出端内部又打日志）时不碰缓冲，返回 -1，
 * 调用方此时应回落到原来的 vprintf。成功返回格式化后的字符数。
 */
int outLogVprintf(const char *fmt, va_list args) noexcept;

} // namespace cli
