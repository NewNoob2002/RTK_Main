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

constexpr bool isPowerOfTwo(size_t value) noexcept
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

#ifdef ESP_PLATFORM
constexpr uint32_t kDefaultCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

// 平台默认分配器（ESP：内部 RAM 堆）：直接填入 BootAlloc::Allocator 的默认函数指针，不另包类。
void *defaultAlloc(size_t align, size_t bytes, void * /*ctx*/) noexcept
{
    return heap_caps_aligned_alloc(align, bytes, kDefaultCaps);
}

void defaultFree(void *ptr, void * /*ctx*/) noexcept
{
    heap_caps_free(ptr);
}

size_t defaultAllocatedSize(void *ptr, void * /*ctx*/) noexcept
{
    return heap_caps_get_allocated_size(ptr);
}
#else
constexpr uint32_t kDefaultCaps = 0U;

// 主机默认分配器（host 测试构建）：同上，作为 Allocator 的默认函数指针。
void *defaultAlloc(size_t align, size_t bytes, void * /*ctx*/) noexcept
{
    return std::aligned_alloc(align, bytes);
}

void defaultFree(void *ptr, void * /*ctx*/) noexcept
{
    std::free(ptr);
}

size_t defaultAllocatedSize(void * /*ptr*/, void * /*ctx*/) noexcept
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
        ops_.alloc = &defaultAlloc;
        ops_.free = &defaultFree;
        ops_.allocatedSize = &defaultAllocatedSize;
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

fw::Status BootArena::allocate(const char *tag, size_t bytes, size_t align, void *&outPtr) noexcept
{
    outPtr = nullptr;
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
    const size_t rawAlign = (align == 0U) ? alignof(void *) : align;
    if (!isPowerOfTwo(rawAlign)) {
        return fw::Status::kInvalidArg;
    }
    const size_t effAlign = (rawAlign < sizeof(void *)) ? sizeof(void *) : rawAlign;
    if (bytes > (SIZE_MAX - (effAlign - 1U))) {
        return fw::Status::kInvalidArg;
    }
    // POSIX aligned_alloc 要求分配大小为对齐值的整数倍；台账分别记录请求字节与实际占用。
    const size_t allocBytes = ((bytes + effAlign - 1U) / effAlign) * effAlign;
    if (count_ >= kMaxEntries) {
        if (reason_[0] == '\0') {
            copyReason("arena full");
        }
        return fw::Status::kNoSpace;
    }
    if ((ops_.alloc == nullptr) || (ops_.free == nullptr)) {
        return fw::Status::kInvalidState;
    }

    void *const ptr = ops_.alloc(effAlign, allocBytes, ops_.ctx);
    if (ptr == nullptr) {
        if (reason_[0] == '\0') {
            std::array<char, kMaxReasonBytes> msg{};
            (void)std::snprintf(msg.data(), msg.size(), "alloc %s", tag);
            copyReason(msg.data());
        }
        return fw::Status::kNoSpace;
    }
    std::memset(ptr, 0, bytes);
    const size_t probedSize =
        (ops_.allocatedSize != nullptr) ? ops_.allocatedSize(ptr, ops_.ctx) : 0U;
    const size_t actualBytes = (probedSize >= allocBytes) ? probedSize : allocBytes;

    entries_[count_] = BootAllocEntry{
        .tag = tag,
        .ptr = ptr,
        .bytes = bytes,
        .actualBytes = actualBytes,
        .align = effAlign,
        .caps = kDefaultCaps,
    };
    ++count_;
    total_bytes_ += bytes;
    total_actual_bytes_ += actualBytes;
    ++live_count_;
    outPtr = ptr;
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
        copyReason(reason);
    } else if (reason_[0] == '\0') {
        copyReason("aborted");
    }
    return fw::Status::kOk;
}

void BootArena::setDegradedReason(const char *reason) noexcept
{
    if ((reason != nullptr) && (*reason != '\0') && (reason_[0] == '\0')) {
        copyReason(reason);
    }
}

const char *BootArena::stateName(ArenaState state) noexcept
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

void BootArena::copyReason(const char *text) noexcept
{
    reason_.fill('\0');
    if (text == nullptr) {
        return;
    }
    (void)std::snprintf(reason_.data(), reason_.size(), "%s", text);
}

} // namespace rtos
