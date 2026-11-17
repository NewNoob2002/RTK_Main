// 锁注入：纯组件不包含 FreeRTOS 头，跨任务共享的 Slot/FanoutRing 通过函数指针表取锁（ADR-0007 §3.5）。
// 目标板用 rtos::spinLockOps(portMUX_TYPE&)，host 单线程测试用 kNoLock；锁内只允许有界拷贝，不得阻塞。
// 纯函数指针表与 RAII 守卫，无分配；可在任务中使用，不可在 ISR 中用需要阻塞的锁。
#pragma once

namespace fw {

/** 函数指针锁表；lock/unlock 必须同为 nullptr 或同为非 nullptr，ctx 原样传给两者。 */
struct LockOps {
    void (*lock)(void *ctx) noexcept = nullptr;
    void (*unlock)(void *ctx) noexcept = nullptr;
    void *ctx = nullptr;
};

/** 空锁：lock/unlock 为 nullptr，LockGuard 遇到 nullptr 什么也不做。只用于单线程场景。 */
inline constexpr LockOps kNoLock{};

/** RAII 守卫；构造加锁、析构解锁。不可拷贝/移动。线程安全性取决于注入的 LockOps。 */
class LockGuard {
public:
    /**
     * @brief  构造时加锁。
     * @param  ops: 锁表；按引用保存，调用方须保证其生命周期覆盖本守卫。
     * @note   ops.lock 为 nullptr 时什么也不做；是否阻塞由注入的锁决定。
     */
    explicit LockGuard(const LockOps &ops) noexcept : ops_(ops)
    {
        if (ops_.lock != nullptr) {
            ops_.lock(ops_.ctx);
        }
    }
    /**
     * @brief  析构时解锁，与构造配对。
     * @note   ops.unlock 为 nullptr 时什么也不做；不可在 ISR 中用需要阻塞的锁。
     */
    ~LockGuard() noexcept
    {
        if (ops_.unlock != nullptr) {
            ops_.unlock(ops_.ctx);
        }
    }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;
    LockGuard(LockGuard &&) = delete;
    LockGuard &operator=(LockGuard &&) = delete;

private:
    const LockOps &ops_;
};

} // namespace fw
