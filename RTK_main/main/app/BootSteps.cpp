// 启动步骤执行器实现（R8a）：顺序执行步骤表，记录结果/耗时/堆快照，按 OnFail 决定降级或继续。
// 线程：仅启动任务调用一次，非 ISR；不分配堆、不阻塞、不打印（日志由调用方统一输出）。
#include "app/BootSteps.hpp"

#include "app/BootReasons.hpp"

#include <cstdio>
#include <string_view>

namespace app {

namespace {

uint32_t sample_now_ms(const BootHooks *hooks) noexcept
{
    return ((hooks != nullptr) && (hooks->now_ms != nullptr)) ? hooks->now_ms() : 0U;
}

void sample_heap(const BootHooks *hooks, BootRecord &record) noexcept
{
    if ((hooks == nullptr) || (hooks->heap == nullptr)) {
        return;
    }
    const BootHeap heap = hooks->heap();
    record.heap_free = heap.free_bytes;
    record.heap_min = heap.min_bytes;
    record.heap_largest = heap.largest_bytes;
}

/** 故障注入：按步骤名命中时不执行步骤体，原因写入 ctx 的固定缓冲。 */
const char *fault_reason_for(BootCtx &ctx, const char *name) noexcept
{
    const std::string_view fault_step{ctx.fault_step != nullptr ? ctx.fault_step : ""};
    if (fault_step.empty() || (fault_step != std::string_view{name})) {
        return nullptr;
    }
    // "fault:" 加步骤名短于缓冲上限；写失败时退回常量，避免返回空原因。
    const int written =
        std::snprintf(ctx.fault_reason.data(), ctx.fault_reason.size(), "%s%s", boot_reason::kFaultPrefix, name);
    return (written > 0) ? ctx.fault_reason.data() : boot_reason::kFaultFallback;
}

} // namespace

const char *boot_result_name(BootResult result) noexcept
{
    switch (result) {
    case BootResult::kOk:
        return "ok";
    case BootResult::kSkipped:
        return "skipped";
    case BootResult::kAbsent:
        return "absent";
    case BootResult::kFail:
        return "fail";
    case BootResult::kPending:
        return "pending";
    default:
        return "unknown";
    }
}

size_t run_boot_steps(std::span<const BootStep> steps, BootCtx &ctx, std::span<BootRecord> records) noexcept
{
    size_t executed = 0U;
    const size_t limit = (steps.size() < records.size()) ? steps.size() : records.size();
    for (size_t i = 0U; i < limit; ++i) {
        BootRecord &record = records[i];
        record = BootRecord{};
        record.name = steps[i].name;
        record.start_ms = sample_now_ms(ctx.hooks);

        const char *reason = fault_reason_for(ctx, steps[i].name);
        if (reason != nullptr) {
            record.result = BootResult::kFail;
        } else {
            ctx.reason = nullptr;
            record.result = steps[i].fn(ctx);
            reason = ctx.reason;
        }

        record.took_ms = sample_now_ms(ctx.hooks) - record.start_ms;
        record.reason = reason;
        sample_heap(ctx.hooks, record);
        ++executed;

        if (record.result != BootResult::kFail) {
            continue;
        }
        if (steps[i].on_fail == OnFail::kContinue) {
            // 已记录并报告本步原因，后续步骤不应继承它。
            ctx.reason = nullptr;
            continue;
        }
        if ((ctx.hooks != nullptr) && (ctx.hooks->on_degrade != nullptr)) {
            ctx.hooks->on_degrade(ctx, reason);
        }
        break;
    }
    return executed;
}

} // namespace app
