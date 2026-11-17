// DataCenter 的单个主题：单写者、多读者、读最新值；锁内整体拷贝 T，读者总拿到一致快照（ADR-0007 §3.4/§3.5）。
// 任意任务可读写，不可在 ISR 中使用；写入者唯一，读者数量不限。
// 静态存储、不分配堆；T 须可平凡拷贝且 sizeof(T) ≤ kMaxSlotBytes 以限制临界区时长。
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "fw/Lock.hpp"

namespace fw {

inline constexpr size_t kMaxSlotBytes = 512;

/**
 * 单值主题槽：保存 T 的最新快照与写入版本号，锁由 bindLock() 注入。
 * 生命周期：静态存储、不分配堆；写入者唯一，读者数量不限。
 */
template <typename T> class Slot {
    static_assert(std::is_trivially_copyable_v<T>, "Slot<T> 需要可平凡拷贝的 T");
    static_assert(sizeof(T) <= kMaxSlotBytes, "Slot<T> 过大会拉长临界区");

public:
    constexpr Slot() noexcept = default;

    /**
     * @brief  注入共享锁。
     * @param  ops: 锁表；按值保存，lock/unlock 须成对。
     * @note   仅启动前单线程调用一次，之后不得再改；constexpr，无分配。
     */
    constexpr void bindLock(const LockOps &ops) noexcept
    {
        lock_ = ops;
    }

    /**
     * @brief  写入最新值并自增版本号。
     * @param  value: 要发布的值，锁内整体拷贝。
     * @note   只允许唯一的写者任务调用，不可在 ISR 使用；持锁时间等于一次拷贝，不分配堆。
     */
    void write(const T &value) noexcept
    {
        const LockGuard guard(lock_);
        value_ = value;
        ++version_;
    }

    /**
     * @brief  读取最新值快照。
     * @retval T 的副本，锁内拷贝完成，不与写入撕裂
     * @note   任意任务可调用，不可在 ISR 中使用；持注入锁。
     */
    [[nodiscard]] T read() const noexcept
    {
        const LockGuard guard(lock_);
        return value_;
    }

    /**
     * @brief  同时读取最新值与写入版本号。
     * @param[out] version: 该快照的版本号；0 表示从未写过。
     * @retval T 的副本，与返回的版本号来自同一次持锁。
     * @note   任意任务可调用，不可在 ISR 中使用；持注入锁。
     */
    [[nodiscard]] T read(uint32_t &version) const noexcept
    {
        const LockGuard guard(lock_);
        version = version_;
        return value_;
    }

private:
    LockOps lock_{};
    T value_{};
    uint32_t version_ = 0;
};

} // namespace fw
