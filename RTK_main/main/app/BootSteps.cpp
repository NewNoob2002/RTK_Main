// 启动步骤执行器实现（ADR-0009）：顺序执行步骤表，记录结果/耗时/堆快照，按 OnFail 决定降级或继续。
// 线程：仅启动任务调用一次，非 ISR；不分配堆、不阻塞、不打印（日志由调用方统一输出）。
#include "app/BootSteps.hpp"

#include "app/BootReasons.hpp"

#include <cstdio>
#include <string_view>

namespace app {

namespace {

/**
 * @brief 从可选时钟钩子采样时间
 * @param hooks: 注入的时钟与堆采样钩子，可为空
 * @retval 钩子返回的毫秒值；无钩子返回 0
 * @note 仅启动任务；同步调用注入钩子，非 ISR。
 */
uint32_t sampleNowMs(const BootHooks *hooks) noexcept
{
    return ((hooks != nullptr) && (hooks->nowMs != nullptr)) ? hooks->nowMs() : 0U;
}

/**
 * @brief 把可选堆采样结果填入启动记录
 * @param hooks: 注入的时钟与堆采样钩子，可为空
 * @param record: 被更新的启动记录
 * @note 仅启动任务；无钩子时保持记录，非 ISR。
 */
void sampleHeap(const BootHooks *hooks, BootRecord &record) noexcept
{
    if ((hooks == nullptr) || (hooks->heap == nullptr)) {
        return;
    }
    const BootHeap heap = hooks->heap();
    record.heapFree = heap.freeBytes;
    record.heapMin = heap.minBytes;
    record.heapLargest = heap.largestBytes;
}

/**
 * @brief 故障注入：按步骤名命中时不执行步骤体，原因写入 ctx 的固定缓冲。
 * @param ctx: 注入配置与原因缓冲
 * @param name: 非空的当前步骤名
 * @retval nullptr 未命中注入步骤
 * @retval 非空 ctx 内的原因文本，格式化失败时使用静态回退文本
 * @note 仅启动任务，非 ISR；ctx 必须覆盖返回文本的读取期。
 */
const char *faultReasonFor(BootCtx &ctx, const char *name) noexcept
{
    const std::string_view faultStep{ctx.faultStep != nullptr ? ctx.faultStep : ""};
    if (faultStep.empty() || (faultStep != std::string_view{name})) {
        return nullptr;
    }
    // "fault:" 加步骤名短于缓冲上限；写失败时退回常量，避免返回空原因。
    const int written = std::snprintf(ctx.faultReason.data(), ctx.faultReason.size(), "%s%s",
                                      boot_reason::kFaultPrefix, name);
    return (written > 0) ? ctx.faultReason.data() : boot_reason::kFaultFallback;
}

} // namespace

const char *bootResultName(BootResult result) noexcept
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

size_t runBootSteps(std::span<const BootStep> steps, BootCtx &ctx,
                    std::span<BootRecord> records) noexcept
{
    size_t executed = 0U;
    const size_t limit = (steps.size() < records.size()) ? steps.size() : records.size();
    for (size_t i = 0U; i < limit; ++i) {
        BootRecord &record = records[i];
        record = BootRecord{};
        record.name = steps[i].name;
        record.startMs = sampleNowMs(ctx.hooks);

        const char *reason = faultReasonFor(ctx, steps[i].name);
        if (reason != nullptr) {
            record.result = BootResult::kFail;
        } else {
            ctx.reason = nullptr;
            record.result = steps[i].fn(ctx);
            reason = ctx.reason;
        }

        record.tookMs = sampleNowMs(ctx.hooks) - record.startMs;
        record.reason = reason;
        sampleHeap(ctx.hooks, record);
        ++executed;

        if (record.result != BootResult::kFail) {
            continue;
        }
        if (steps[i].onFail == OnFail::kContinue) {
            // 已记录并报告本步原因，后续步骤不应继承它。
            ctx.reason = nullptr;
            continue;
        }
        if ((ctx.hooks != nullptr) && (ctx.hooks->onDegrade != nullptr)) {
            ctx.hooks->onDegrade(ctx, reason);
        }
        break;
    }
    return executed;
}

} // namespace app
