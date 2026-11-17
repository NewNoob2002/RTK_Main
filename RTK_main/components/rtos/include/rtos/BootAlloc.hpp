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
    size_t bytes = 0U;       // 调用方请求的逻辑字节数
    size_t actualBytes = 0U; // 分配器报告的可用块字节数（含对齐取整，不含隐藏堆块头）
    size_t align = 0U;
    uint32_t caps = 0U;
};

/** 可选分配器钩子，仅用于 host 故障注入与泄漏测试；生产路径传 nullptr 使用默认内部 8-bit 堆。 */
struct BootAllocatorOps {
    void *(*alloc)(size_t align, size_t bytes, void *ctx) noexcept = nullptr;
    void (*free)(void *ptr, void *ctx) noexcept = nullptr;
    size_t (*allocatedSize)(void *ptr, void *ctx) noexcept = nullptr;
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

    /**
     * @brief  打开台账进入 OPEN 态并选定分配器
     * @param  ops: 可选分配器钩子；nullptr 使用默认内部 8-bit 堆
     * @retval fw::Status::kOk 已打开
     * @retval fw::Status::kInvalidState 不是 UNINIT 态（重复 open）
     * @retval fw::Status::kInvalidArg ops 的 alloc 或 free 为空
     * @note   只在启动期单线程调用；不分配堆
     */
    [[nodiscard]] fw::Status open(const BootAllocatorOps *ops = nullptr) noexcept;

    /**
     * @brief  在 OPEN 态分配一块对齐字节数组并清零，记入台账
     * @param  tag: 分配标签，只记首次；重名按失败处理
     * @param  bytes: 请求的逻辑字节数，必须大于 0
     * @param  align: 对齐字节数；0 表示 alignof(void*)，否则必须是 2 的幂
     * @param[out] outPtr: 成功时写入分配地址；失败时置 nullptr
     * @retval fw::Status::kOk 成功
     * @retval fw::Status::kInvalidState 不在 OPEN 态，或分配器未配置
     * @retval fw::Status::kInvalidArg tag 为空、bytes 为 0、标签重名、对齐非 2 的幂或尺寸溢出
     * @retval fw::Status::kNoSpace 台账已满或底层分配失败（首因记入 reason()）
     * @note   只在启动期单线程调用（seal 之前）；不阻塞；不可在 ISR 中调用
     */
    [[nodiscard]] fw::Status allocate(const char *tag, size_t bytes, size_t align,
                                      void *&outPtr) noexcept;

    /**
     * @brief  按元素类型分配 count 个 T 的数组，失败时 out 保持空 span
     * @param  tag: 分配标签，透传给 allocate()
     * @param  count: 元素个数，必须大于 0 且不溢出
     * @param[out] out: 成功时指向 count 个 T；失败时为空 span
     * @param  align: 对齐字节数；0 表示 alignof(T)，且不得小于 alignof(T)
     * @retval fw::Status::kOk 成功
     * @retval fw::Status::kInvalidArg count 为 0 或溢出，或 align 小于 alignof(T)
     * @retval 其余状态来自 allocate()（kInvalidState / kNoSpace）
     * @note   只在启动期单线程调用；不阻塞
     */
    template <typename T>
    [[nodiscard]] fw::Status allocArray(const char *tag, size_t count, std::span<T> &out,
                                        size_t align = alignof(T)) noexcept
    {
        out = std::span<T>{};
        const size_t typedAlign = (align == 0U) ? alignof(T) : align;
        if ((count == 0U) || (count > (SIZE_MAX / sizeof(T))) || (typedAlign < alignof(T))) {
            return fw::Status::kInvalidArg;
        }
        void *raw = nullptr;
        const fw::Status st = allocate(tag, count * sizeof(T), typedAlign, raw);
        if (!fw::isOk(st)) {
            return st;
        }
        out = std::span<T>{static_cast<T *>(raw), count};
        return fw::Status::kOk;
    }

    /**
     * @brief  封存台账（OPEN -> SEALED），此后禁止分配
     * @retval fw::Status::kOk 已封存
     * @retval fw::Status::kInvalidState 不在 OPEN 态
     * @note   只在启动期单线程调用；封存后状态与台账只读
     */
    [[nodiscard]] fw::Status seal() noexcept;

    /**
     * @brief  启动期回滚：释放所有已分配块并进入 ABORTED 态
     * @param  reason: 可选原因文本，会截断到 kMaxReasonBytes-1；nullptr 表示用默认文本
     * @retval fw::Status::kOk 已释放并进入 ABORTED
     * @retval fw::Status::kInvalidState 已 SEALED（不释放任何块）
     * @note   仅 UNINIT/OPEN 有效；前提是尚无消费者持有返回的指针，否则会悬垂
     */
    [[nodiscard]] fw::Status abort(const char *reason = nullptr) noexcept;

    /**
     * @brief  记录启动期后段（已 seal/已有消费者）发生的降级原因
     * @param  reason: 原因文本；已记录过原因时忽略，不改变 SEALED 状态
     */
    void setDegradedReason(const char *reason) noexcept;

    /**
     * @brief  当前台账状态
     * @retval 状态枚举值
     */
    [[nodiscard]] ArenaState state() const noexcept
    {
        return state_;
    }

    /**
     * @brief  台账中的分配项数（abort 后归零）
     * @retval 条数
     */
    [[nodiscard]] size_t count() const noexcept
    {
        return count_;
    }

    /**
     * @brief  调用方请求的逻辑字节总数
     * @retval 字节数；abort 后归零
     */
    [[nodiscard]] size_t totalBytes() const noexcept
    {
        return total_bytes_;
    }

    /**
     * @brief  分配器报告的实际占用字节总数
     * @retval 字节数（含对齐取整）；abort 后归零
     */
    [[nodiscard]] size_t totalActualBytes() const noexcept
    {
        return total_actual_bytes_;
    }

    /**
     * @brief  当前仍被持有的块数
     * @retval 块数
     */
    [[nodiscard]] size_t liveCount() const noexcept
    {
        return live_count_;
    }

    /**
     * @brief  已被 abort 释放的块数
     * @retval 块数
     */
    [[nodiscard]] size_t freedCount() const noexcept
    {
        return freed_count_;
    }

    /**
     * @brief  台账项只读视图
     * @retval 前 count() 条台账项；count() 为 0 时为空 span
     */
    [[nodiscard]] std::span<const BootAllocEntry> entries() const noexcept
    {
        return std::span<const BootAllocEntry>{entries_.data(), count_};
    }

    /**
     * @brief  是否已降级
     * @retval true 已 ABORTED 或已记录降级原因
     */
    [[nodiscard]] bool degraded() const noexcept
    {
        return (state_ == ArenaState::kAborted) || (reason_[0] != '\0');
    }

    /**
     * @brief  降级/失败原因文本
     * @retval 以 NUL 结尾的原因；无原因时为空串
     */
    [[nodiscard]] const char *reason() const noexcept
    {
        return reason_.data();
    }

    /**
     * @brief  状态枚举的可读名
     * @param  state: 台账状态
     * @retval "UNINIT"/"OPEN"/"SEALED"/"ABORTED"；未知值返回 "UNKNOWN"
     */
    [[nodiscard]] static const char *stateName(ArenaState state) noexcept;

private:
    void copyReason(const char *text) noexcept;

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
