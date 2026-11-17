// 启动期只增不减内部 RAM 分配器实现（ADR-0011 §7）；第 2 档堆：仅启动期一次性分配与回滚。
// 线程安全：仅启动阶段单线程调用，非 ISR 安全；seal() 后状态与台账只读。

#include "rtos/BootAlloc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace rtos {

namespace {

constexpr bool is_power_of_two(size_t value) noexcept
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

#ifdef ESP_PLATFORM
constexpr uint32_t kDefaultCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

void *default_alloc(size_t align, size_t bytes, void * /*ctx*/) noexcept
{
    return heap_caps_aligned_alloc(align, bytes, kDefaultCaps);
}

void default_free(void *ptr, void * /*ctx*/) noexcept
{
    heap_caps_free(ptr);
}

size_t default_allocated_size(void *ptr, void * /*ctx*/) noexcept
{
    return heap_caps_get_allocated_size(ptr);
}
#else
constexpr uint32_t kDefaultCaps = 0U;

void *default_alloc(size_t align, size_t bytes, void * /*ctx*/) noexcept
{
    return std::aligned_alloc(align, bytes);
}

void default_free(void *ptr, void * /*ctx*/) noexcept
{
    std::free(ptr);
}

size_t default_allocated_size(void * /*ptr*/, void * /*ctx*/) noexcept
{
    return 0U;
}
#endif

} // namespace

fw::Status BootArena::open(const BootAllocatorOps *ops) noexcept
{
    if (state_ != ArenaState::kUninit) {
        return fw::Status::kInvalidState;
    }
    if (ops != nullptr) {
        if ((ops->alloc == nullptr) || (ops->free == nullptr)) {
            return fw::Status::kInvalidArg;
        }
        ops_ = *ops;
    } else {
        ops_.alloc = &default_alloc;
        ops_.free = &default_free;
        ops_.allocated_size = &default_allocated_size;
        ops_.ctx = nullptr;
    }
    count_ = 0U;
    total_bytes_ = 0U;
    total_actual_bytes_ = 0U;
    live_count_ = 0U;
    freed_count_ = 0U;
    reason_.fill('\0');
    state_ = ArenaState::kOpen;
    return fw::Status::kOk;
}

fw::Status BootArena::allocate(const char *tag, size_t bytes, size_t align, void *&out_ptr) noexcept
{
    out_ptr = nullptr;
    if (state_ != ArenaState::kOpen) {
        return fw::Status::kInvalidState;
    }
    if ((tag == nullptr) || (*tag == '\0') || (bytes == 0U)) {
        return fw::Status::kInvalidArg;
    }
    for (size_t i = 0U; i < count_; ++i) {
        if ((entries_[i].tag != nullptr) && (std::strcmp(entries_[i].tag, tag) == 0)) {
            return fw::Status::kInvalidArg;
        }
    }
    const size_t raw_align = (align == 0U) ? alignof(void *) : align;
    if (!is_power_of_two(raw_align)) {
        return fw::Status::kInvalidArg;
    }
    const size_t eff_align = (raw_align < sizeof(void *)) ? sizeof(void *) : raw_align;
    if (bytes > (SIZE_MAX - (eff_align - 1U))) {
        return fw::Status::kInvalidArg;
    }
    // POSIX aligned_alloc 要求分配大小为对齐值的整数倍；台账分别记录请求字节与实际占用。
    const size_t alloc_bytes = ((bytes + eff_align - 1U) / eff_align) * eff_align;
    if (count_ >= kMaxEntries) {
        if (reason_[0] == '\0') {
            copy_reason("arena full");
        }
        return fw::Status::kNoSpace;
    }
    if ((ops_.alloc == nullptr) || (ops_.free == nullptr)) {
        return fw::Status::kInvalidState;
    }

    void *const ptr = ops_.alloc(eff_align, alloc_bytes, ops_.ctx);
    if (ptr == nullptr) {
        if (reason_[0] == '\0') {
            std::array<char, kMaxReasonBytes> msg{};
            (void)std::snprintf(msg.data(), msg.size(), "alloc %s", tag);
            copy_reason(msg.data());
        }
        return fw::Status::kNoSpace;
    }
    std::memset(ptr, 0, bytes);
    const size_t probed_size =
        (ops_.allocated_size != nullptr) ? ops_.allocated_size(ptr, ops_.ctx) : 0U;
    const size_t actual_bytes = (probed_size >= alloc_bytes) ? probed_size : alloc_bytes;

    entries_[count_] = BootAllocEntry{
        .tag = tag,
        .ptr = ptr,
        .bytes = bytes,
        .actual_bytes = actual_bytes,
        .align = eff_align,
        .caps = kDefaultCaps,
    };
    ++count_;
    total_bytes_ += bytes;
    total_actual_bytes_ += actual_bytes;
    ++live_count_;
    out_ptr = ptr;
    return fw::Status::kOk;
}

fw::Status BootArena::seal() noexcept
{
    if (state_ != ArenaState::kOpen) {
        return fw::Status::kInvalidState;
    }
    state_ = ArenaState::kSealed;
    return fw::Status::kOk;
}

fw::Status BootArena::abort(const char *reason) noexcept
{
    if ((state_ != ArenaState::kOpen) && (state_ != ArenaState::kUninit)) {
        return fw::Status::kInvalidState;
    }
    for (size_t i = 0U; i < count_; ++i) {
        if (entries_[i].ptr != nullptr) {
            if (ops_.free != nullptr) {
                ops_.free(entries_[i].ptr, ops_.ctx);
            }
            entries_[i].ptr = nullptr;
            ++freed_count_;
        }
    }
    live_count_ = 0U;
    total_bytes_ = 0U;
    total_actual_bytes_ = 0U;
    count_ = 0U;
    state_ = ArenaState::kAborted;
    if ((reason != nullptr) && (*reason != '\0')) {
        copy_reason(reason);
    } else if (reason_[0] == '\0') {
        copy_reason("aborted");
    }
    return fw::Status::kOk;
}

void BootArena::set_degraded_reason(const char *reason) noexcept
{
    if ((reason != nullptr) && (*reason != '\0') && (reason_[0] == '\0')) {
        copy_reason(reason);
    }
}

const char *BootArena::state_name(ArenaState state) noexcept
{
    switch (state) {
    case ArenaState::kUninit:
        return "UNINIT";
    case ArenaState::kOpen:
        return "OPEN";
    case ArenaState::kSealed:
        return "SEALED";
    case ArenaState::kAborted:
        return "ABORTED";
    default:
        return "UNKNOWN";
    }
}

void BootArena::copy_reason(const char *text) noexcept
{
    reason_.fill('\0');
    if (text == nullptr) {
        return;
    }
    (void)std::snprintf(reason_.data(), reason_.size(), "%s", text);
}

} // namespace rtos
