// 系统内存诊断的格式化：静态段、boot owner 与重叠的能力堆视图。
// 线程：仅 console 任务或单线程 host 检查调用；只读已封存/已终止的 arena，不分配堆，非 ISR 安全。
#pragma once

#include <cinttypes>
#include <span>

#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "rtos/BootAlloc.hpp"

namespace app {

struct MemoryHeapView {
    const char *name;
    uint32_t caps;
    size_t total;
    size_t free;
    size_t minimum;
    size_t largest;
};

struct InternalHeapBudget {
    size_t minimum;
    size_t largest;
    const char *source;
};

/**
 * @brief 选择单镜像堆门槛，连续空闲块仍为 ADR-0011 的记录环保留余量
 * @param wifiConfigEnabled: 固件是否编入 Wi-Fi 配置模式
 * @retval InternalHeapBudget 与构建配置对应的门槛及其来源
 * @note 纯运算，线程及 ISR 安全。
 */
[[nodiscard]] constexpr InternalHeapBudget internalHeapBudget(bool wifiConfigEnabled) noexcept
{
    constexpr size_t kWifiConfigMinimum = 40960U;
    constexpr size_t kBluetoothOnlyMinimum = 81920U;
    constexpr size_t kInternalLargestBudget = 32768U;
    return {wifiConfigEnabled ? kWifiConfigMinimum : kBluetoothOnlyMinimum, kInternalLargestBudget,
            wifiConfigEnabled ? "adr0017" : "adr0011"};
}

/**
 * @brief 打印内部 8 位堆的预算判定及来源
 * @param minimum: 历史最小空闲字节数
 * @param largest: 当前最大连续空闲块字节数
 * @param wifiConfigEnabled: 固件是否编入 Wi-Fi 配置模式
 * @note 仅 console 任务或单线程 host；输出可阻塞，非 ISR。
 */
inline void printInternalHeapBudget(size_t minimum, size_t largest, bool wifiConfigEnabled) noexcept
{
    const auto budget = internalHeapBudget(wifiConfigEnabled);
    const bool budgetOk = minimum >= budget.minimum && largest >= budget.largest;
    cli::printKv("internal_budget", "%s (min>=%zu B, largest>=%zu B)", budgetOk ? "PASS" : "FAIL",
                 budget.minimum, budget.largest);
    cli::printKv("internal_budget_source", "%s", budget.source);
}

/**
 * @brief 打印静态段、启动分配台账与能力堆预算
 * @param arena: 启动分配台账
 * @param dataBytes: .data 段字节数
 * @param bssBytes: .bss 段字节数
 * @param heaps: 重叠能力堆视图；首项须为 INTERNAL|8BIT
 * @param wifiConfigEnabled: 固件是否编入 Wi-Fi 配置模式
 * @note 仅启动完成后的 console 或单线程 host；arena 须封存或终止，输出可阻塞，非 ISR。
 */
inline void printMemoryReport(const rtos::BootArena &arena, size_t dataBytes, size_t bssBytes,
                              std::span<const MemoryHeapView> heaps,
                              bool wifiConfigEnabled) noexcept
{
    cli::printSection("static");
    cli::printKv("data", "%zu B", dataBytes);
    cli::printKv("bss", "%zu B", bssBytes);
    cli::printKv("total", "%zu B", dataBytes + bssBytes);

    cli::printSection("boot_arena");
    cli::printKv("state", "%s", rtos::BootArena::stateName(arena.state()));
    cli::printKv("requested", "%zu B  actual=%zu B  blocks=%zu/%zu", arena.totalBytes(),
                 arena.totalActualBytes(), arena.entries().size(), rtos::BootArena::kMaxEntries);
    for (const auto &entry : arena.entries()) {
        cli::outPrintf("  %-20s req=%6zu B  act=%6zu B  align=%-2zu  caps=0x%" PRIx32 "  addr=%p\n",
                       (entry.tag != nullptr) ? entry.tag : "?", entry.bytes, entry.actualBytes,
                       entry.align, entry.caps, entry.ptr);
    }

    cli::printSection("heap_caps");
    cli::outPrintf("  note: capability views overlap; do not add them together\n");
    for (const auto &view : heaps) {
        cli::printKv(view.name, "total=%zu B  free=%zu B  min=%zu B  largest=%zu B", view.total,
                     view.free, view.minimum, view.largest);
    }
    printInternalHeapBudget(heaps.empty() ? 0U : heaps.front().minimum,
                            heaps.empty() ? 0U : heaps.front().largest, wifiConfigEnabled);
    cli::printSection("mode");
    if (arena.degraded()) {
        cli::printKv("status", "degraded (console-only: %s)", arena.reason());
    } else {
        cli::printKv("status", "normal");
    }
}

} // namespace app
