// 系统内存诊断的格式化：静态段、boot owner 与重叠的能力堆视图。
// 线程：仅 console 任务或单线程 host 检查调用；只读已封存/已终止的 arena，不分配堆，非 ISR 安全。
#pragma once

#include <cinttypes>
#include <span>

#include "cli/console.hpp"
#include "cli/out.hpp"
#include "rtos/boot_alloc.hpp"

namespace app {

struct MemoryHeapView {
    const char *name;
    uint32_t caps;
    size_t total;
    size_t free;
    size_t minimum;
    size_t largest;
};

// heaps 第一项固定为 INTERNAL|8BIT；调用者在启动阶段结束后收集各能力视图。
inline void print_memory_report(const rtos::BootArena &arena, size_t data_bytes, size_t bss_bytes,
                                std::span<const MemoryHeapView> heaps) noexcept
{
    cli::print_section("static");
    cli::print_kv("data", "%zu B", data_bytes);
    cli::print_kv("bss", "%zu B", bss_bytes);
    cli::print_kv("total", "%zu B", data_bytes + bss_bytes);

    cli::print_section("boot_arena");
    cli::print_kv("state", "%s", rtos::BootArena::state_name(arena.state()));
    cli::print_kv("requested", "%zu B  actual=%zu B  blocks=%zu/%zu", arena.total_bytes(), arena.total_actual_bytes(),
                  arena.entries().size(), rtos::BootArena::kMaxEntries);
    for (const auto &entry : arena.entries()) {
        cli::out_printf("  %-20s req=%6zu B  act=%6zu B  align=%-2zu  caps=0x%" PRIx32 "  addr=%p\n",
                        (entry.tag != nullptr) ? entry.tag : "?", entry.bytes, entry.actual_bytes, entry.align,
                        entry.caps, entry.ptr);
    }

    cli::print_section("heap_caps");
    cli::out_printf("  note: capability views overlap; do not add them together\n");
    for (const auto &view : heaps) {
        cli::print_kv(view.name, "total=%zu B  free=%zu B  min=%zu B  largest=%zu B", view.total, view.free,
                      view.minimum, view.largest);
    }
    constexpr size_t kInternalMinBudget = size_t{80} * size_t{1024};
    constexpr size_t kInternalLargestBudget = size_t{32} * size_t{1024};
    const bool budget_ok = !heaps.empty() && heaps.front().minimum >= kInternalMinBudget &&
                           heaps.front().largest >= kInternalLargestBudget;
    cli::print_kv("internal_budget", "%s (min>=%zu B, largest>=%zu B)", budget_ok ? "PASS" : "FAIL", kInternalMinBudget,
                  kInternalLargestBudget);
    cli::print_section("mode");
    if (arena.degraded()) {
        cli::print_kv("status", "degraded (console-only: %s)", arena.reason());
    } else {
        cli::print_kv("status", "normal");
    }
}

} // namespace app
