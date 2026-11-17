// 控制台统一输出出口实现（ADR-0009 §2.8）：整行格式化后一次写出，绕开 UART VFS 的逐字节写出。
// 线程：递归互斥保护静态缓冲与跨调用的行尾状态，s_busy 识别同任务重入；持锁可能阻塞，不可在 ISR 中调用。
// 堆：静态缓冲 + 静态递归互斥（目标板不用 std::recursive_mutex，避免拉入堆与 locale）；不分配堆。

#include "cli/Out.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <span>

#if defined(ESP_PLATFORM)
#include "fw/Atomic.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h" // vTaskDelay（等待创建者发布 kReady）
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
static_assert(fw::kAtomicUsable<uint32_t>, "输出锁的初始化状态须无锁：不得拉入 libatomic 的锁");

/**
 * @brief 静态递归互斥；首个调用者创建，之后只读 handle_。
 *
 * 状态机：kUninit →（CAS 成功的唯一创建者）kIniting → kReady。创建者调用
 * xSemaphoreCreateRecursiveMutexStatic 后无论成败都发布 kReady，其余任务自旋等待到 kReady 再取用；
 * 创建失败时 handle_ 为 nullptr，lock/unlock 退化为无操作（输出不互斥，但不阻塞、不崩溃）。
 * 创建不放进临界区：临界区内不允许调用 FreeRTOS API（ESP-001）。
 */
class OutLock {
public:
    /** @brief 取互斥量；第一个到达的任务完成递归互斥量初始化，其余任务等它就绪。 @note
     * 任意任务可调用；可能阻塞（等创建者或信号量），不可在 ISR 调用。 */
    void lock() noexcept
    {
        for (;;) {
            uint32_t expected = kUninit;
            if (state_.compare_exchange_weak(expected, kIniting, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                handle_ = xSemaphoreCreateRecursiveMutexStatic(&storage_);
                state_.store(kReady, std::memory_order_release);
                break;
            }
            if (expected == kReady) {
                break;
            }
            vTaskDelay(1); // 创建者可能被抢占：让出 1 tick 后重试（state_ 尚未发布）
        }
        if (handle_ != nullptr) {
            (void)xSemaphoreTakeRecursive(handle_, portMAX_DELAY);
        }
    }
    /** @brief 释放互斥量；初始化失败（handle_ 为空）时为空操作。 @note 任意任务可调用，不阻塞；不可在 ISR 调用。 */
    void unlock() noexcept
    {
        if (handle_ != nullptr) {
            (void)xSemaphoreGiveRecursive(handle_);
        }
    }

private:
    static constexpr uint32_t kUninit = 0U;
    static constexpr uint32_t kIniting = 1U;
    static constexpr uint32_t kReady = 2U;

    StaticSemaphore_t storage_{};
    SemaphoreHandle_t handle_ = nullptr; // kReady 之后只读
    std::atomic<uint32_t> state_{kUninit};
};
#else
using OutLock = std::recursive_mutex;
#endif

/** 作用域加锁（不用 std::lock_guard，避免目标板拉入 <mutex>）。 */
class Guard {
public:
    /**
     * @brief  加锁，析构时释放
     * @param  lock: 输出互斥；构造时加锁，可能阻塞
     * @note   任意任务可构造；持锁期间不可在 ISR 中调用。
     */
    explicit Guard(OutLock &lock) noexcept : lock_(lock)
    {
        lock_.lock();
    }
    /** @brief 释放构造时拿到的锁。 @note 必须与构造函数在同一任务；不阻塞。 */
    ~Guard()
    {
        lock_.unlock();
    }
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

/**
 * @brief  写 stdout，保持与原 printf 路径一致（VFS 负责行尾转换）
 * @param  text: 文本
 * @param  len: 字节数
 * @note   sink 未安装时的回落路径；可能阻塞在 VFS 写出，调用方已持锁。
 */
void writeStdout(const char *text, size_t len) noexcept
{
    (void)std::fwrite(text, 1U, len, stdout);
}

/**
 * @brief  持锁把文本交给输出端，按 crlf 设置转换行尾
 * @param  text: 文本；nullptr 或 len 为 0 时直接返回
 * @param  len: 字节数
 * @note   调用方必须已持有 s_lock；可能阻塞在输出端或 VFS。
 */
void emitLocked(const char *text, size_t len) noexcept
{
    if ((text == nullptr) || (len == 0U)) {
        return;
    }
    const std::span<const char> src{text, len};
    if (s_sink == nullptr) {
        writeStdout(src.data(), src.size());
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

/**
 * @brief  持锁格式化后输出；超长时整条交给 stdout（VFS 逐字节，慢但不截断）
 * @param  fmt: printf 格式串，非空（调用方已校验）
 * @param  args: 参数列表；副本留给超长回落路径，原列表被 vsnprintf 消费
 * @retval >=0 vsnprintf 报告的长度
 * @retval <0  格式化失败，未输出
 * @note   调用方必须已持有 s_lock；可能阻塞。
 */
int vprintfLocked(const char *fmt, va_list args) noexcept
{
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(s_fmt.data(), s_fmt.size(), fmt, args);
    if ((n >= 0) && (static_cast<size_t>(n) < s_fmt.size())) {
        emitLocked(s_fmt.data(), static_cast<size_t>(n));
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

void setOutSink(OutSink sink, bool crlf) noexcept
{
    const Guard guard(s_lock);
    s_sink = sink;
    s_crlf = crlf;
    s_prev = '\n';
}

void outWrite(const char *text, size_t len) noexcept
{
    const Guard guard(s_lock);
    s_busy = true;
    emitLocked(text, len);
    s_busy = false;
}

void outVprintf(const char *fmt, va_list args) noexcept
{
    if (fmt == nullptr) {
        return;
    }
    const Guard guard(s_lock);
    s_busy = true;
    (void)vprintfLocked(fmt, args);
    s_busy = false;
}

void outPrintf(const char *fmt, ...) noexcept
{
    va_list args;
    va_start(args, fmt);
    outVprintf(fmt, args);
    va_end(args);
}

int outLogVprintf(const char *fmt, va_list args) noexcept
{
    if (fmt == nullptr) {
        return 0;
    }
    const Guard guard(s_lock);
    if (s_busy) {
        return -1; // 同任务重入：缓冲正在使用，交回调用方
    }
    s_busy = true;
    const int n = vprintfLocked(fmt, args);
    s_busy = false;
    return n;
}

} // namespace cli
