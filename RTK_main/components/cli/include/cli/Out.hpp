// 控制台统一输出出口（ADR-0009 §2.8）：回复、trace、周期报告与 ESP_LOG 共用；整行格式化后一次写出。
// 线程：任意任务可调用，内部递归互斥；持锁期间可能阻塞在输出端（UART TX 环形满），不可在 ISR 中调用。
// 堆：只用静态缓冲，不分配堆。

#pragma once

#include <cstdarg>
#include <cstddef>

namespace cli {

/**
 * @brief  输出端：把 len 字节原样写出
 * @param  text: 文本起始地址，非空
 * @param  len: 字节数，0 时不调用
 * @note   由 cli::init() 在启动期安装；未安装时写 stdout。实现可阻塞（UART 环形满），不可在 ISR 中调用。
 */
using OutSink = void (*)(const char *text, size_t len) noexcept;

/** 单次格式化的整行缓冲长度；超过时整条改走 stdout（慢但不截断）。 */
inline constexpr size_t kOutLineMax = 256U;

/**
 * @brief  安装输出端并设置行尾转换
 * @param  sink: 输出端；nullptr → 写 stdout（host 测试与 USB 控制台）
 * @param  crlf: true 时把未跟在 '\r' 之后的 '\n' 展开为 "\r\n"（绕过 VFS 后行尾转换由这里负责）
 * @note   只在启动阶段调用一次（REPL 任务开始读写之前），调用时不得有其他任务并发输出；不分配堆。
 */
void setOutSink(OutSink sink, bool crlf) noexcept;

/**
 * @brief  原样输出一段文本（按 setOutSink 的 crlf 设置转换行尾）
 * @param  text: 文本指针；nullptr 或 len 为 0 时直接返回
 * @param  len: 字节数
 * @note   任意任务可调用；持锁，可能阻塞在输出端，不可在 ISR 中调用。
 */
void outWrite(const char *text, size_t len) noexcept;

/**
 * @brief  格式化后输出
 * @param  fmt: printf 格式串，非空
 * @note   一次调用对应一次（或少数几次）输出端写入，调用之间不会被其他输出插入；
 *         格式与实参由编译期 format 属性校验。线程与阻塞约束同 outWrite。
 */
void outPrintf(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
/**
 * @brief  用 va_list 格式化后输出
 * @param  fmt: printf 格式串；nullptr 时直接返回
 * @param  args: 已 va_start 的参数列表
 * @note   线程与阻塞约束同 outWrite；字符数丢弃。
 */
void outVprintf(const char *fmt, va_list args) noexcept;

/**
 * @brief  ESP_LOG 钩子用：同 outVprintf，但同一任务重入时拒绝输出
 * @param  fmt: printf 格式串；nullptr 时返回 0
 * @param  args: 已 va_start 的参数列表
 * @retval >=0 格式化后的字符数
 * @retval -1  同一任务已在输出（输出端内部又打日志），缓冲不碰，调用方应回落到原 vprintf
 * @note   任意任务可调用；持锁，可能阻塞。调用点须先排除 ISR 与调度器未运行的情况。
 */
int outLogVprintf(const char *fmt, va_list args) noexcept;

} // namespace cli
