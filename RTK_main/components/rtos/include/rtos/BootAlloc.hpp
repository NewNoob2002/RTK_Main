// 启动期只增不减内部 RAM 分配台账（ADR-0011 §7）：在进入稳态前一次性分配大块缓冲区并 seal。
// 线程安全：仅启动阶段单线程调用 open/allocate/seal/abort；seal 后 entries()/state()/reason() 只读安全。ISR 安全：否。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Status.hpp"

namespace rtos {

enum class ArenaState : uint8_t {
    kUninit = 0U,
    kOpen = 1U,
    kSealed = 2U,
    kAborted = 3U,
};

struct BootAllocEntry {
    const char *tag = nullptr;
    void *ptr = nullptr;
    size_t bytes = 0U;        // 调用方请求的逻辑字节数
    size_t actual_bytes = 0U; // 分配器报告的可用块字节数（含对齐取整，不含隐藏堆块头）
    size_t align = 0U;
    uint32_t caps = 0U;
};

/** 可选分配器钩子，仅用于 host 故障注入与泄漏测试；生产路径传 nullptr 使用默认内部 8-bit 堆。 */
struct BootAllocatorOps {
    void *(*alloc)(size_t align, size_t bytes, void *ctx) noexcept = nullptr;
    void (*free)(void *ptr, void *ctx) noexcept = nullptr;
    size_t (*allocated_size)(void *ptr, void *ctx) noexcept = nullptr;
    void *ctx = nullptr;
};

/**
 * 启动期分配器与台账（ADR-0011 §7.2）。
 * 状态机：UNINIT -> OPEN -> SEALED / ABORTED。
 * 约束：默认构造 constexpr 不触碰堆；仅允许在 OPEN 态分配；seal() 后拒绝新分配；
 *       已 SEALED 时 abort() 拒绝释放并返回 kInvalidState，避免悬垂引用。
 */
class BootArena {
public:
    static constexpr size_t kMaxEntries = 32U;
    static constexpr size_t kMaxReasonBytes = 64U;

    constexpr BootArena() noexcept = default;

    BootArena(const BootArena &) = delete;
    BootArena &operator=(const BootArena &) = delete;
    BootArena(BootArena &&) = delete;
    BootArena &operator=(BootArena &&) = delete;
    ~BootArena() = default;

    /** 打开台账进入 OPEN 态；只允许从 UNINIT 转换。 */
    [[nodiscard]] fw::Status open(const BootAllocatorOps *ops = nullptr) noexcept;

    /** 在 OPEN 态分配一块对齐的内部 8-bit 字节数组并清零；非 OPEN 返回 kInvalidState。 */
    [[nodiscard]] fw::Status allocate(const char *tag, size_t bytes, size_t align, void *&out_ptr) noexcept;

    /** 类型安全数组分配包装；失败时保持 out 为空 span。 */
    template <typename T>
    [[nodiscard]] fw::Status alloc_array(const char *tag, size_t count, std::span<T> &out,
                                         size_t align = alignof(T)) noexcept
    {
        out = std::span<T>{};
        const size_t typed_align = (align == 0U) ? alignof(T) : align;
        if ((count == 0U) || (count > (SIZE_MAX / sizeof(T))) || (typed_align < alignof(T))) {
            return fw::Status::kInvalidArg;
        }
        void *raw = nullptr;
        const fw::Status st = allocate(tag, count * sizeof(T), typed_align, raw);
        if (!fw::is_ok(st)) {
            return st;
        }
        out = std::span<T>{static_cast<T *>(raw), count};
        return fw::Status::kOk;
    }

    /** 封存台账（OPEN -> SEALED）；此后 allocate() 一律返回 kInvalidState。 */
    [[nodiscard]] fw::Status seal() noexcept;

    /**
     * 启动期回滚（仅 UNINIT/OPEN 有效，且前提为尚无消费者持有指针）。
     * 释放所有已分配块并进入 ABORTED 态；若已 SEALED 则不释放任何块并返回 kInvalidState。
     */
    [[nodiscard]] fw::Status abort(const char *reason = nullptr) noexcept;

    /** 记录启动期后段（已 seal/已有消费者）发生的降级原因，不改变 SEALED 状态。 */
    void set_degraded_reason(const char *reason) noexcept;

    [[nodiscard]] ArenaState state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] size_t count() const noexcept
    {
        return count_;
    }

    [[nodiscard]] size_t total_bytes() const noexcept
    {
        return total_bytes_;
    }

    [[nodiscard]] size_t total_actual_bytes() const noexcept
    {
        return total_actual_bytes_;
    }

    [[nodiscard]] size_t live_count() const noexcept
    {
        return live_count_;
    }

    [[nodiscard]] size_t freed_count() const noexcept
    {
        return freed_count_;
    }

    [[nodiscard]] std::span<const BootAllocEntry> entries() const noexcept
    {
        return std::span<const BootAllocEntry>{entries_.data(), count_};
    }

    [[nodiscard]] bool degraded() const noexcept
    {
        return (state_ == ArenaState::kAborted) || (reason_[0] != '\0');
    }

    [[nodiscard]] const char *reason() const noexcept
    {
        return reason_.data();
    }

    [[nodiscard]] static const char *state_name(ArenaState state) noexcept;

private:
    void copy_reason(const char *text) noexcept;

    ArenaState state_ = ArenaState::kUninit;
    BootAllocatorOps ops_{};
    std::array<BootAllocEntry, kMaxEntries> entries_{};
    size_t count_ = 0U;
    size_t total_bytes_ = 0U;
    size_t total_actual_bytes_ = 0U;
    size_t live_count_ = 0U;
    size_t freed_count_ = 0U;
    std::array<char, kMaxReasonBytes> reason_{};
};

} // namespace rtos
