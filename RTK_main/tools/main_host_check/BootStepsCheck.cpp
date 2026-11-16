// Host 单元测试（R8a）：启动步骤执行器的结果分类、降级中断、继续语义、故障注入与记录边界。
// 线程：单线程测试进程，非 ISR。
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "app/BootSteps.hpp"

namespace {

int s_failures = 0;

/** 断言失败时打印表达式并累计，测试继续跑完以便一次看到全部失败。 */
void check(bool ok, const char *expr) noexcept
{
    if (!ok) {
        std::printf("FAIL %s\n", expr);
        ++s_failures;
    }
}

#define CHECK(cond) check((cond), #cond)

// 数字是步骤下标、边界记录数与注入的时间/堆快照；由上方具名常量与数组下标表达，仍属测试向量。
// NOLINTBEGIN(readability-magic-numbers)

/** 注入用的起点毫秒、每次读表的步进与堆快照，断言按这三组常数计算。 */
constexpr uint32_t kBaseMs = 100U;
constexpr uint32_t kStepMs = 5U;
constexpr uint32_t kHeapFreeBytes = 1000U;
constexpr uint32_t kHeapMinBytes = 900U;
constexpr uint32_t kHeapLargestBytes = 800U;
constexpr app::BootHeap kHeapSnapshot{kHeapFreeBytes, kHeapMinBytes, kHeapLargestBytes};
/** 步骤体被调用的次数下标上限：ok/fail/fail_silent/skipped/ok_second。 */
constexpr size_t kCallSlots = 5U;

struct Fake {
    uint32_t now = kBaseMs;
    app::BootHeap heap{kHeapSnapshot};
    uint32_t degrade_calls = 0U;
    const char *degrade_reason = nullptr;
    std::array<uint32_t, kCallSlots> calls{};
};

Fake s_fake{};

app::BootHeap heap_hook() noexcept
{
    return s_fake.heap;
}

uint32_t now_hook() noexcept
{
    // 每次读表推进 kStepMs，使 took_ms 可断言。
    s_fake.now += kStepMs;
    return s_fake.now;
}

void degrade_hook(app::BootCtx & /*ctx*/, const char *reason) noexcept
{
    ++s_fake.degrade_calls;
    s_fake.degrade_reason = reason;
}

void reset_fake() noexcept
{
    s_fake.now = kBaseMs;
    s_fake.heap = kHeapSnapshot;
    s_fake.degrade_calls = 0U;
    s_fake.degrade_reason = nullptr;
    s_fake.calls.fill(0U);
}

constexpr app::BootHooks kFakeHooks{&now_hook, &heap_hook, &degrade_hook};

app::BootResult step_ok(app::BootCtx & /*ctx*/) noexcept
{
    ++s_fake.calls[0];
    return app::BootResult::kOk;
}

app::BootResult step_ok_second(app::BootCtx & /*ctx*/) noexcept
{
    ++s_fake.calls[1];
    return app::BootResult::kOk;
}

app::BootResult step_fail(app::BootCtx &ctx) noexcept
{
    ++s_fake.calls[2];
    ctx.reason = "boom";
    return app::BootResult::kFail;
}

app::BootResult step_fail_silent(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    ++s_fake.calls[3];
    return app::BootResult::kFail;
}

app::BootResult step_skipped(app::BootCtx & /*ctx*/) noexcept
{
    ++s_fake.calls[4];
    return app::BootResult::kSkipped;
}

/** 全部成功：记录数、结果、耗时与堆快照都按注入的钩子写入。 */
void test_all_ok() noexcept
{
    reset_fake();
    constexpr std::array<app::BootStep, 3U> kSteps{{
        {"a", &step_ok, app::OnFail::kDegrade},
        {"b", &step_ok_second, app::OnFail::kDegrade},
        {"c", &step_skipped, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 3U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::run_boot_steps(kSteps, ctx, records);
    CHECK(executed == kSteps.size());
    CHECK(s_fake.degrade_calls == 0U);
    CHECK(records[0].name != nullptr && std::string_view{records[0].name} == "a");
    CHECK(records[0].result == app::BootResult::kOk);
    CHECK(records[0].took_ms == kStepMs);
    CHECK(records[0].heap_free == kHeapFreeBytes && records[0].heap_min == kHeapMinBytes &&
          records[0].heap_largest == kHeapLargestBytes);
    CHECK(records[2].result == app::BootResult::kSkipped);
    CHECK(records[0].reason == nullptr);
    CHECK(s_fake.calls[1] == 1U && s_fake.calls[4] == 1U);
}

/** kDegrade：记录失败与原因，调用降级回调，停止后续步骤（未执行记录保持 kPending）。 */
void test_degrade_stops() noexcept
{
    reset_fake();
    constexpr std::array<app::BootStep, 3U> kSteps{{
        {"a", &step_ok, app::OnFail::kDegrade},
        {"boom", &step_fail, app::OnFail::kDegrade},
        {"c", &step_ok_second, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 3U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::run_boot_steps(kSteps, ctx, records);
    CHECK(executed == 2U);
    CHECK(records[1].result == app::BootResult::kFail);
    CHECK(records[1].reason != nullptr && std::string_view{records[1].reason} == "boom");
    CHECK(s_fake.degrade_calls == 1U);
    CHECK(s_fake.degrade_reason != nullptr && std::string_view{s_fake.degrade_reason} == "boom");
    CHECK(s_fake.calls[1] == 0U); // 第三步未执行
    CHECK(records[2].result == app::BootResult::kPending);
}

/** kContinue：记录失败但继续执行下一步，且不触发降级回调。 */
void test_continue_keeps_going() noexcept
{
    reset_fake();
    constexpr std::array<app::BootStep, 2U> kSteps{{
        {"soft", &step_fail_silent, app::OnFail::kContinue},
        {"b", &step_ok_second, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 2U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::run_boot_steps(kSteps, ctx, records);
    CHECK(executed == kSteps.size());
    CHECK(records[0].result == app::BootResult::kFail);
    CHECK(s_fake.degrade_calls == 0U);
    CHECK(s_fake.calls[1] == 1U);
}

/** 故障注入：命中的步骤不执行，原因记为 fault:<step>，并按 on_fail 语义处理。 */
void test_fault_injection() noexcept
{
    reset_fake();
    constexpr std::array<app::BootStep, 2U> kSteps{{
        {"a", &step_ok, app::OnFail::kDegrade},
        {"b", &step_ok_second, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 2U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    ctx.fault_step = "b";
    const size_t executed = app::run_boot_steps(kSteps, ctx, records);
    CHECK(executed == 2U);
    CHECK(records[1].result == app::BootResult::kFail);
    CHECK(records[1].reason != nullptr && std::string_view{records[1].reason} == "fault:b");
    CHECK(s_fake.calls[0] == 1U);
    CHECK(s_fake.calls[1] == 0U); // 注入的步骤体未执行
    CHECK(s_fake.degrade_calls == 1U);
}

/** 记录缓冲不足时只写满容量，不越界；空表返回 0。 */
void test_record_bounds() noexcept
{
    reset_fake();
    constexpr std::array<app::BootStep, 3U> kSteps{{
        {"a", &step_ok, app::OnFail::kDegrade},
        {"b", &step_ok_second, app::OnFail::kDegrade},
        {"c", &step_skipped, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 2U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::run_boot_steps(kSteps, ctx, records);
    CHECK(executed == records.size());
    CHECK(s_fake.calls[4] == 0U);

    const size_t empty = app::run_boot_steps(std::span<const app::BootStep>{}, ctx, records);
    CHECK(empty == 0U);
    CHECK(std::string_view{app::boot_result_name(app::BootResult::kSkipped)} == "skipped");
    CHECK(std::string_view{app::boot_result_name(app::BootResult::kFail)} == "fail");
}

// NOLINTEND(readability-magic-numbers)

} // namespace

int main()
{
    test_all_ok();
    test_degrade_stops();
    test_continue_keeps_going();
    test_fault_injection();
    test_record_bounds();
    if (s_failures != 0) {
        std::printf("boot_steps_check: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    std::printf("boot_steps_check: OK\n");
    return EXIT_SUCCESS;
}
