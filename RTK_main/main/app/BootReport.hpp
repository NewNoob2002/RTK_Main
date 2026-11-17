// 启动步骤表的文本输出（R8a）：启动摘要与 `sys boot` 共用同一格式，避免两处漂移。
// 线程：仅启动任务（放行前）或 console REPL 任务调用；非 ISR，不分配堆。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "app/BootSteps.hpp"
#include "cli/Out.hpp"

namespace app {

/**
 * 打印步骤记录、总计与最慢步骤。
 * 每行一次 out_write，保证整行原子；reason 为空时省略尾段。
 */
inline void print_boot_table(std::span<const BootRecord> records) noexcept
{
    if (records.empty()) {
        cli::out_printf("boot: no records\r\n");
        return;
    }
    const BootRecord &first = records.front();
    const BootRecord &last = records.back();
    const uint32_t total_ms = (last.start_ms + last.took_ms) - first.start_ms;
    const BootRecord *slowest = nullptr;
    uint32_t skipped = 0U;
    cli::out_printf("%-9s %-7s %8s %7s %9s %9s %9s\r\n", "step", "result", "start ms", "took ms",
                    "heap free", "heap min", "largest");
    for (const BootRecord &record : records) {
        if ((slowest == nullptr) || (record.took_ms > slowest->took_ms)) {
            slowest = &record;
        }
        if (record.result == BootResult::kSkipped) {
            ++skipped;
        }
        if (record.reason != nullptr) {
            cli::out_printf("%-9s %-7s %8u %7u %9u %9u %9u  %s\r\n", record.name,
                            boot_result_name(record.result), static_cast<unsigned>(record.start_ms),
                            static_cast<unsigned>(record.took_ms),
                            static_cast<unsigned>(record.heap_free),
                            static_cast<unsigned>(record.heap_min),
                            static_cast<unsigned>(record.heap_largest), record.reason);
        } else {
            cli::out_printf(
                "%-9s %-7s %8u %7u %9u %9u %9u\r\n", record.name, boot_result_name(record.result),
                static_cast<unsigned>(record.start_ms), static_cast<unsigned>(record.took_ms),
                static_cast<unsigned>(record.heap_free), static_cast<unsigned>(record.heap_min),
                static_cast<unsigned>(record.heap_largest));
        }
    }
    cli::out_printf("boot: %u steps, %u skipped, %u ms total, slowest %s %u ms\r\n",
                    static_cast<unsigned>(records.size()), static_cast<unsigned>(skipped),
                    static_cast<unsigned>(total_ms), slowest->name,
                    static_cast<unsigned>(slowest->took_ms));
}

} // namespace app
