// 启动步骤表的文本输出（ADR-0009）：启动摘要与 `sys boot` 共用同一格式，避免两处漂移。
// 线程：仅启动任务（放行前）或 console REPL 任务调用；非 ISR，不分配堆。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "app/BootSteps.hpp"
#include "cli/Out.hpp"

namespace app {

/**
 * @brief 打印步骤记录、总计与最慢步骤。 每行一次 outWrite，保证整行原子；reason 为空时省略尾段。
 * @param records: 启动步骤记录视图
 * @note 仅启动任务或 console REPL 任务，输出可等待控制台锁，非 ISR。
 */
inline void printBootTable(std::span<const BootRecord> records) noexcept
{
    if (records.empty()) {
        cli::outPrintf("boot: no records\r\n");
        return;
    }
    const BootRecord &first = records.front();
    const BootRecord &last = records.back();
    const uint32_t totalMs = (last.startMs + last.tookMs) - first.startMs;
    const BootRecord *slowest = nullptr;
    uint32_t skipped = 0U;
    cli::outPrintf("%-9s %-7s %8s %7s %9s %9s %9s\r\n", "step", "result", "start ms", "took ms",
                   "heap free", "heap min", "largest");
    for (const BootRecord &record : records) {
        if ((slowest == nullptr) || (record.tookMs > slowest->tookMs)) {
            slowest = &record;
        }
        if (record.result == BootResult::kSkipped) {
            ++skipped;
        }
        if (record.reason != nullptr) {
            cli::outPrintf(
                "%-9s %-7s %8u %7u %9u %9u %9u  %s\r\n", record.name, bootResultName(record.result),
                static_cast<unsigned>(record.startMs), static_cast<unsigned>(record.tookMs),
                static_cast<unsigned>(record.heapFree), static_cast<unsigned>(record.heapMin),
                static_cast<unsigned>(record.heapLargest), record.reason);
        } else {
            cli::outPrintf(
                "%-9s %-7s %8u %7u %9u %9u %9u\r\n", record.name, bootResultName(record.result),
                static_cast<unsigned>(record.startMs), static_cast<unsigned>(record.tookMs),
                static_cast<unsigned>(record.heapFree), static_cast<unsigned>(record.heapMin),
                static_cast<unsigned>(record.heapLargest));
        }
    }
    cli::outPrintf("boot: %u steps, %u skipped, %u ms total, slowest %s %u ms\r\n",
                   static_cast<unsigned>(records.size()), static_cast<unsigned>(skipped),
                   static_cast<unsigned>(totalMs), slowest->name,
                   static_cast<unsigned>(slowest->tookMs));
}

} // namespace app
