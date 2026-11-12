// 冻结接口（ADR-0007 §3.5）。实现方不得修改。
// 锁注入：纯组件不能包含 FreeRTOS 头，跨任务共享的 Slot/FanoutRing 通过函数指针表拿到锁。
// 目标板：rtk::os::spin_lock_ops(portMUX_TYPE&)（rtk_os 提供）；host 单线程测试：kNoLock。
// 约束：lock/unlock 之间只允许做有界的内存拷贝，不得调用阻塞 API。
//
// Generated-by: Claude Code + claude-opus-5-5
// Owner: UNKNOWN（待人工审查签字）
#pragma once

namespace app {

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
    explicit LockGuard(const LockOps &ops) noexcept : ops_(ops)
    {
        if (ops_.lock != nullptr) {
            ops_.lock(ops_.ctx);
        }
    }
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

} // namespace app
