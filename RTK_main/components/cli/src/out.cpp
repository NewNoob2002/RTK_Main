// 控制台统一输出出口实现（ADR-0009 §2.8）。只依赖 C/C++ 标准库，host 测试直接编译。
// 背景：IDF v6.1 UART VFS（esp_driver_uart/src/uart_vfs.c:227）对每个字符调用一次 uart_write_bytes，
// printf 在 2 Mbaud / 240 MHz 下只有约 24.6 KB/s；这里整行格式化后一次写出。
// 线程：一把递归互斥保护静态缓冲与跨调用的行尾状态；s_busy 识别同一任务重入（输出端里又打日志）。
// 目标板用静态 FreeRTOS 递归互斥：std::recursive_mutex 会拉入 system_error/format.o（堆、locale）
// 与 __cxa_atexit，违反 g7b 链接约束；host 构建仍用标准库互斥。

#include "cli/out.hpp"

#include <array>
#include <cstdio>
#include <span>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <mutex>
#endif

namespace cli {

namespace {

constexpr size_t kXlatMax = kOutLineMax + 64U; // 行尾展开缓冲：一般整行一次写完，满了分段写

OutSink s_sink = nullptr; // nullptr → stdout
bool s_crlf = false;
char s_prev = '\n'; // 上一个已输出字符，用于跨调用判断 "\r\n" 是否已成对

#if defined(ESP_PLATFORM)
/** 静态递归互斥；首次加锁时创建。创建在短临界区内完成，多个任务同时首次输出也只创建一次。 */
class OutLock {
public:
    void lock() noexcept
    {
        if (handle_ == nullptr) {
            portENTER_CRITICAL(&init_mux_);
            if (handle_ == nullptr) {
                handle_ = xSemaphoreCreateRecursiveMutexStatic(&storage_);
            }
            portEXIT_CRITICAL(&init_mux_);
        }
        (void)xSemaphoreTakeRecursive(handle_, portMAX_DELAY);
    }
    void unlock() noexcept { (void)xSemaphoreGiveRecursive(handle_); }

private:
    StaticSemaphore_t storage_{};
    SemaphoreHandle_t volatile handle_ = nullptr;
    portMUX_TYPE init_mux_ = portMUX_INITIALIZER_UNLOCKED;
};
#else
using OutLock = std::recursive_mutex;
#endif

/** 作用域加锁（不用 std::lock_guard，避免目标板拉入 <mutex>）。 */
class Guard {
public:
    explicit Guard(OutLock &lock) noexcept : lock_(lock) { lock_.lock(); }
    ~Guard() { lock_.unlock(); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    Guard(Guard &&) = delete;
    Guard &operator=(Guard &&) = delete;

private:
    OutLock &lock_;
};

OutLock s_lock;
bool s_busy = false; // 持锁期间置位；同任务重入时 recursive_mutex 能拿到锁，靠它拒绝
std::array<char, kOutLineMax> s_fmt{};
std::array<char, kXlatMax> s_xlat{};

/** 写 stdout，保持与原 printf 路径一致（VFS 负责行尾转换）。 */
void write_stdout(const char *text, size_t len) noexcept
{
    (void)std::fwrite(text, 1U, len, stdout);
}

/** 持锁调用：按设置转换行尾后交给输出端。 */
void emit_locked(const char *text, size_t len) noexcept
{
    if ((text == nullptr) || (len == 0U)) {
        return;
    }
    const std::span<const char> src{text, len};
    if (s_sink == nullptr) {
        write_stdout(src.data(), src.size());
        return;
    }
    // linenoise 的提示符/回显和回落路径仍走 stdout，先冲刷，保证顺序。
    (void)std::fflush(stdout);
    if (!s_crlf) {
        s_sink(src.data(), src.size());
        s_prev = src.back();
        return;
    }
    size_t used = 0U;
    for (const char c : src) {
        if ((used + 2U) > s_xlat.size()) {
            s_sink(s_xlat.data(), used);
            used = 0U;
        }
        if ((c == '\n') && (s_prev != '\r')) {
            s_xlat[used] = '\r';
            ++used;
        }
        s_xlat[used] = c;
        ++used;
        s_prev = c;
    }
    s_sink(s_xlat.data(), used);
}

/** 持锁调用：格式化后输出；超长时整条交给 stdout（VFS 逐字节，慢但不截断）。 */
int vprintf_locked(const char *fmt, va_list args) noexcept
{
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(s_fmt.data(), s_fmt.size(), fmt, args);
    if ((n >= 0) && (static_cast<size_t>(n) < s_fmt.size())) {
        emit_locked(s_fmt.data(), static_cast<size_t>(n));
    } else if (n >= 0) {
        (void)std::fflush(stdout);
        (void)std::vfprintf(stdout, fmt, copy);
        (void)std::fflush(stdout);
        s_prev = '\n';
    }
    va_end(copy);
    return n;
}

} // namespace

void set_out_sink(OutSink sink, bool crlf) noexcept
{
    const Guard guard(s_lock);
    s_sink = sink;
    s_crlf = crlf;
    s_prev = '\n';
}

void out_write(const char *text, size_t len) noexcept
{
    const Guard guard(s_lock);
    s_busy = true;
    emit_locked(text, len);
    s_busy = false;
}

void out_vprintf(const char *fmt, va_list args) noexcept
{
    if (fmt == nullptr) {
        return;
    }
    const Guard guard(s_lock);
    s_busy = true;
    (void)vprintf_locked(fmt, args);
    s_busy = false;
}

void out_printf(const char *fmt, ...) noexcept
{
    va_list args;
    va_start(args, fmt);
    out_vprintf(fmt, args);
    va_end(args);
}

int out_log_vprintf(const char *fmt, va_list args) noexcept
{
    if (fmt == nullptr) {
        return 0;
    }
    const Guard guard(s_lock);
    if (s_busy) {
        return -1; // 同任务重入：缓冲正在使用，交回调用方
    }
    s_busy = true;
    const int n = vprintf_locked(fmt, args);
    s_busy = false;
    return n;
}

} // namespace cli
