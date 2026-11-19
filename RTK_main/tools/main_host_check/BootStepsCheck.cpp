// Host 单元测试（R8a）：启动步骤执行器的结果分类、降级中断、继续语义、故障注入与记录边界。
// 线程：单线程测试进程，非 ISR。
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

#include "app/BootSteps.hpp"
#include "app/ConsoleProfile.hpp"

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
    uint32_t degradeCalls = 0U;
    bool consoleInitSucceeds = false;
    size_t consoleCallCount = 0U;
    std::array<char, 3U> consoleCalls{};
    const char *degradeReason = nullptr;
    std::array<uint32_t, kCallSlots> calls{};
};

Fake s_fake{};

app::BootHeap heapHook() noexcept
{
    return s_fake.heap;
}

uint32_t nowHook() noexcept
{
    // 每次读表推进 kStepMs，使 tookMs 可断言。
    s_fake.now += kStepMs;
    return s_fake.now;
}

void degradeHook(app::BootCtx & /*ctx*/, const char *reason) noexcept
{
    ++s_fake.degradeCalls;
    s_fake.degradeReason = reason;
}

void resetFake() noexcept
{
    s_fake.now = kBaseMs;
    s_fake.heap = kHeapSnapshot;
    s_fake.degradeCalls = 0U;
    s_fake.consoleCallCount = 0U;
    s_fake.degradeReason = nullptr;
    s_fake.calls.fill(0U);
}

constexpr app::BootHooks kFakeHooks{&nowHook, &heapHook, &degradeHook};

app::BootResult stepOk(app::BootCtx & /*ctx*/) noexcept
{
    ++s_fake.calls[0];
    return app::BootResult::kOk;
}

app::BootResult stepOkSecond(app::BootCtx & /*ctx*/) noexcept
{
    ++s_fake.calls[1];
    return app::BootResult::kOk;
}

app::BootResult stepFail(app::BootCtx &ctx) noexcept
{
    ++s_fake.calls[2];
    ctx.reason = "boom";
    return app::BootResult::kFail;
}

app::BootResult stepFailSilent(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    ++s_fake.calls[3];
    return app::BootResult::kFail;
}

app::BootResult stepSkipped(app::BootCtx & /*ctx*/) noexcept
{
    ++s_fake.calls[4];
    return app::BootResult::kSkipped;
}

/** 全部成功：记录数、结果、耗时与堆快照都按注入的钩子写入。 */
void testAllOk() noexcept
{
    resetFake();
    constexpr std::array<app::BootStep, 3U> kSteps{{
        {"a", &stepOk, app::OnFail::kDegrade},
        {"b", &stepOkSecond, app::OnFail::kDegrade},
        {"c", &stepSkipped, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 3U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::runBootSteps(kSteps, ctx, records);
    CHECK(executed == kSteps.size());
    CHECK(s_fake.degradeCalls == 0U);
    CHECK(records[0].name != nullptr && std::string_view{records[0].name} == "a");
    CHECK(records[0].result == app::BootResult::kOk);
    CHECK(records[0].tookMs == kStepMs);
    CHECK(records[0].heapFree == kHeapFreeBytes && records[0].heapMin == kHeapMinBytes &&
          records[0].heapLargest == kHeapLargestBytes);
    CHECK(records[2].result == app::BootResult::kSkipped);
    CHECK(records[0].reason == nullptr);
    CHECK(s_fake.calls[1] == 1U && s_fake.calls[4] == 1U);
}

/** kDegrade：记录失败与原因，调用降级回调，停止后续步骤（未执行记录保持 kPending）。 */
void testDegradeStops() noexcept
{
    resetFake();
    constexpr std::array<app::BootStep, 3U> kSteps{{
        {"a", &stepOk, app::OnFail::kDegrade},
        {"boom", &stepFail, app::OnFail::kDegrade},
        {"c", &stepOkSecond, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 3U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::runBootSteps(kSteps, ctx, records);
    CHECK(executed == 2U);
    CHECK(records[1].result == app::BootResult::kFail);
    CHECK(records[1].reason != nullptr && std::string_view{records[1].reason} == "boom");
    CHECK(s_fake.degradeCalls == 1U);
    CHECK(s_fake.degradeReason != nullptr && std::string_view{s_fake.degradeReason} == "boom");
    CHECK(s_fake.calls[1] == 0U); // 第三步未执行
    CHECK(records[2].result == app::BootResult::kPending);
}

/** kContinue：记录失败但继续执行下一步，且不触发降级回调。 */
void testContinueKeepsGoing() noexcept
{
    resetFake();
    constexpr std::array<app::BootStep, 2U> kSteps{{
        {"soft", &stepFailSilent, app::OnFail::kContinue},
        {"b", &stepOkSecond, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 2U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::runBootSteps(kSteps, ctx, records);
    CHECK(executed == kSteps.size());
    CHECK(records[0].result == app::BootResult::kFail);
    CHECK(s_fake.degradeCalls == 0U);
    CHECK(s_fake.calls[1] == 1U);
}

/** 故障注入：命中的步骤不执行，原因记为 fault:<step>，并按 onFail 语义处理。 */
void testFaultInjection() noexcept
{
    resetFake();
    constexpr std::array<app::BootStep, 2U> kSteps{{
        {"a", &stepOk, app::OnFail::kDegrade},
        {"b", &stepOkSecond, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 2U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    ctx.faultStep = "b";
    const size_t executed = app::runBootSteps(kSteps, ctx, records);
    CHECK(executed == 2U);
    CHECK(records[1].result == app::BootResult::kFail);
    CHECK(records[1].reason != nullptr && std::string_view{records[1].reason} == "fault:b");
    CHECK(s_fake.calls[0] == 1U);
    CHECK(s_fake.calls[1] == 0U); // 注入的步骤体未执行
    CHECK(s_fake.degradeCalls == 1U);
}

/** 记录缓冲不足时只写满容量，不越界；空表返回 0。 */
void testRecordBounds() noexcept
{
    resetFake();
    constexpr std::array<app::BootStep, 3U> kSteps{{
        {"a", &stepOk, app::OnFail::kDegrade},
        {"b", &stepOkSecond, app::OnFail::kDegrade},
        {"c", &stepSkipped, app::OnFail::kDegrade},
    }};
    std::array<app::BootRecord, 2U> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    const size_t executed = app::runBootSteps(kSteps, ctx, records);
    CHECK(executed == records.size());
    CHECK(s_fake.calls[4] == 0U);

    const size_t empty = app::runBootSteps(std::span<const app::BootStep>{}, ctx, records);
    CHECK(empty == 0U);
    CHECK(std::string_view{app::bootResultName(app::BootResult::kSkipped)} == "skipped");
    CHECK(std::string_view{app::bootResultName(app::BootResult::kFail)} == "fail");
}

/**
 * @brief 用生产表验证启动顺序以及识别失败后停止后续步骤
 * @note 单线程主机测试，非 ISR。
 */
void testIdentifyFailure() noexcept
{
    resetFake();
#define BOOT_STEP(name, fn, policy) app::BootStep{#name, &stepOk, app::OnFail::policy},
    std::array steps{
#include "app/BootStepList.inc"
    };
#undef BOOT_STEP
    CHECK(steps.size() == 15U);
    CHECK(std::string_view{steps[0].name} == "identify");
    CHECK(std::string_view{steps[1].name} == "power");
    CHECK(std::string_view{steps[2].name} == "banner");
    CHECK(std::string_view{steps[3].name} == "earlyConfig");
    steps[0].fn = &stepFail;
    std::array<app::BootRecord, app::kMaxBootSteps> records{};
    app::BootCtx ctx{};
    ctx.hooks = &kFakeHooks;
    CHECK(app::runBootSteps(steps, ctx, records) == 1U);
    CHECK(records[0].result == app::BootResult::kFail);
    CHECK(records[1].result == app::BootResult::kPending);
    CHECK(s_fake.degradeCalls == 1U);
    CHECK(s_fake.calls[0] == 0U);
}

/**
 * @brief 将一次控制台钩子调用写入顺序记录，超出预期容量则累计失败
 * @param call: I 表示 init、A 表示 attach、R 表示 run
 * @note 单线程主机测试，非 ISR。
 */
void recordConsoleCall(char call) noexcept
{
    CHECK(s_fake.consoleCallCount < s_fake.consoleCalls.size());
    if (s_fake.consoleCallCount < s_fake.consoleCalls.size()) {
        s_fake.consoleCalls[s_fake.consoleCallCount++] = call;
    }
}

/**
 * @brief 用生产步骤表验证各档位遇到故障时的控制台钩子调用顺序
 * @note 单线程主机测试，非 ISR。
 */
template <app::ConsoleProfile Profile> void testConsoleDegrade() noexcept
{
#define BOOT_STEP(name, fn, policy) app::BootStep{#name, &stepOk, app::OnFail::policy},
    constexpr std::array kSteps{
#include "app/BootStepList.inc"
    };
#undef BOOT_STEP
    CHECK(std::ranges::any_of(kSteps, [](const app::BootStep &step) {
        return std::string_view{step.name} == "console";
    }));

    struct ConsoleFault {
        const char *step;
        bool initSucceeds;
        bool consoleReached;
        std::string_view replCalls;
    };
    constexpr std::array kFaults{
        ConsoleFault{"power", true, false, "IAR"},
        ConsoleFault{"power", false, false, "I"},
        ConsoleFault{"console", false, true, "AR"},
        ConsoleFault{"alloc", false, true, "AR"},
    };
    for (const auto &fault : kFaults) {
        resetFake();
        s_fake.consoleInitSucceeds = fault.initSucceeds;
        std::array<app::BootRecord, app::kMaxBootSteps> records{};
        app::BootCtx ctx{};
        ctx.hooks = &kFakeHooks;
        ctx.faultStep = fault.step;
        const auto executed = app::runBootSteps(kSteps, ctx, records);
        const auto prefix = std::span{records}.first(executed);
        CHECK(!prefix.empty() && std::string_view{prefix.back().name} == fault.step);
        CHECK(!prefix.empty() && prefix.back().result == app::BootResult::kFail);
        CHECK(s_fake.degradeCalls == 1U);
        const bool consoleReached = std::ranges::any_of(prefix, [](const app::BootRecord &record) {
            return std::string_view{record.name} == "console";
        });
        CHECK(consoleReached == fault.consoleReached);
        app::finishDegradedConsole<Profile,
                                   []() noexcept {
                                       recordConsoleCall('I');
                                       return s_fake.consoleInitSucceeds;
                                   },
                                   []() noexcept { recordConsoleCall('A'); },
                                   []() noexcept { recordConsoleCall('R'); }>(consoleReached);
        const std::string_view calls{s_fake.consoleCalls.data(), s_fake.consoleCallCount};
        const auto expected =
            Profile == app::ConsoleProfile::kNone ? std::string_view{} : fault.replCalls;
        CHECK(calls == expected);
    }
}

} // namespace

int main()
{
    testAllOk();
    testDegradeStops();
    testContinueKeepsGoing();
    testFaultInjection();
    testRecordBounds();
    testIdentifyFailure();
    testConsoleDegrade<app::ConsoleProfile::kDev>();
    testConsoleDegrade<app::ConsoleProfile::kProduction>();
    testConsoleDegrade<app::ConsoleProfile::kNone>();
    if (s_failures != 0) {
        std::printf("boot_steps_check: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    std::printf("boot_steps_check: OK\n");
    return EXIT_SUCCESS;
}
