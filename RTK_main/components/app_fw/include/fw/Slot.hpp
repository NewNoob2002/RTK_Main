// DataCenter 的单个主题：单写者、多读者、读最新值。写与读都在注入的锁内整体拷贝 T，
// 因此读者永远拿到一致的快照（不会读到写了一半的结构体）。
// 约束：T 必须可平凡拷贝，且 sizeof(T) ≤ kMaxSlotBytes（限制临界区时长）。
// 冻结接口（ADR-0007 §3.4/§3.5）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "fw/Lock.hpp"

namespace fw {

inline constexpr size_t kMaxSlotBytes = 512;

/**
 * 单值主题槽：保存 T 的最新快照与写入版本号，锁由 bind_lock() 注入。
 * 生命周期：静态存储、不分配堆；写入者唯一，读者数量不限。
 */
template <typename T> class Slot {
    static_assert(std::is_trivially_copyable_v<T>, "Slot<T> 需要可平凡拷贝的 T");
    static_assert(sizeof(T) <= kMaxSlotBytes, "Slot<T> 过大会拉长临界区");

public:
    constexpr Slot() noexcept = default;

    /** 启动前（单线程）调用一次；之后不得再改。 */
    constexpr void bindLock(const LockOps &ops) noexcept
    {
        lock_ = ops;
    }

    /** 只允许唯一的写者任务调用。version 每次 +1（u32 回绕无害，只用于判断“变了没有”）。 */
    void write(const T &value) noexcept
    {
        const LockGuard guard(lock_);
        value_ = value;
        ++version_;
    }

    /** 任意任务可调用（不可在 ISR 中调用）。返回值拷贝。 */
    [[nodiscard]] T read() const noexcept
    {
        const LockGuard guard(lock_);
        return value_;
    }

    /** 同时取值与版本号；version 为 0 表示从未写过。 */
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
